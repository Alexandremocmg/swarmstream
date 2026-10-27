// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "gate_core.h"

#include "swarmstream/memory_cache_store.hpp"
#include "swarmstream/memory_content_source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace swgate {

namespace {

using swarmstream::Lease;
using swarmstream::MemoryCacheStore;
using swarmstream::MemoryContentSource;
using swarmstream::PieceHash;
using swarmstream::PieceId;

constexpr size_t kPiece = swarmstream::kDefaultPieceSize; // 1 MiB (03 §1)

// Arquivos do portão. Os de sufixo ".low." são do grupo core: já estão no
// cache quando o jogo começa (06 §Política de falta).
struct PackEntry
{
    const char* path;
    bool core;
};
constexpr PackEntry kPack[] = {
    {"rock_4k.swtex", false},    {"rock_4k.low.swtex", true}, {"sphere.swmesh", false},
    {"sphere.low.swmesh", true}, {"tone.ogg", false},
};

struct VFile
{
    uint64_t size = 0;
    std::vector<PieceId> pieces;
};

bool read_whole_file(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    if (n > 0 && !in.read(reinterpret_cast<char*>(out.data()), n)) return false;
    return true;
}

} // namespace

struct GateCore::Impl
{
    mutable std::mutex mu;
    bool ready = false;
    std::shared_ptr<MemoryCacheStore> cache;
    std::unique_ptr<MemoryContentSource> source;
    std::unordered_map<std::string, VFile> files;

    // Peça em voo -> conclusão (true = gravada no cache). Um pedido de rede por peça.
    std::unordered_map<PieceId, std::shared_future<bool>> in_flight;
    std::vector<std::thread> threads;

    // Leases presos por ensure, até release().
    std::unordered_map<uint64_t, std::vector<Lease>> held;
    std::atomic<uint64_t> next_ensure{1};

    std::atomic<uint64_t> st_reads{0}, st_not_local{0}, st_ensures{0}, st_fetched{0};

    const VFile* find(const std::string& path) const
    {
        auto it = files.find(path);
        return it == files.end() ? nullptr : &it->second;
    }

    // Grava uma peça vinda da fonte no cache (dados -> commit, como no ADR-0005).
    // Roda SEM mu: só toca o MemoryCacheStore, que tem os próprios mutexes, e um atômico.
    bool store_piece(const PieceId& piece, const std::vector<uint8_t>& data)
    {
        auto slot = cache->alloc_slot();
        if (!slot) return false;
        if (!cache->write(*slot, 0, data)) return false;
        if (!cache->commit(*slot, piece, data.size())) return false;
        st_fetched.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Pede a peça à fonte, se ainda não está no cache nem em voo. Chamar com mu preso.
    std::shared_future<bool> fetch_locked(const PieceId& piece)
    {
        if (cache->has_piece(piece)) {
            std::promise<bool> p;
            p.set_value(true);
            return p.get_future().share();
        }
        auto it = in_flight.find(piece);
        if (it != in_flight.end()) return it->second;
        auto promise = std::make_shared<std::promise<bool>>();
        std::shared_future<bool> fut = promise->get_future().share();
        in_flight.emplace(piece, fut);
        // Demanda imediata: prazo = agora. O prazo é só prioridade (F1-56).
        auto ticket = source->request(piece, std::chrono::steady_clock::now());
        auto future =
            std::make_shared<std::future<swarmstream::PieceResponse>>(std::move(ticket.future));
        threads.emplace_back([this, piece, promise, future]() {
            swarmstream::PieceResponse resp = future->get();
            bool ok = resp.ok() && store_piece(piece, resp.data);
            {
                std::lock_guard<std::mutex> lock(mu);
                in_flight.erase(piece);
            }
            promise->set_value(ok);
        });
        return fut;
    }
};

GateCore& GateCore::instance()
{
    static GateCore core;
    return core;
}

GateCore::GateCore() : impl_(std::make_unique<Impl>()) {}

GateCore::~GateCore() { shutdown(); }

sw_result GateCore::init(const std::string& pack_dir, uint32_t latency_ms)
{
    shutdown();
    std::lock_guard<std::mutex> lock(impl_->mu);
    Impl& m = *impl_;
    m.cache = std::make_shared<MemoryCacheStore>(64, kPiece);
    m.source =
        std::make_unique<MemoryContentSource>("gate_cdn", std::chrono::milliseconds(latency_ms));
    m.files.clear();
    uint32_t next_index = 0;
    for (const PackEntry& entry : kPack) {
        std::vector<uint8_t> bytes;
        if (!read_whole_file(pack_dir + "/" + entry.path, bytes) || bytes.empty()) {
            return SW_E_NOT_FOUND;
        }
        VFile vf;
        vf.size = bytes.size();
        for (size_t off = 0; off < bytes.size(); off += kPiece) {
            const size_t n = std::min(kPiece, bytes.size() - off);
            std::vector<uint8_t> piece_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                                             bytes.begin() + static_cast<std::ptrdiff_t>(off + n));
            PieceId id{next_index++, PieceHash{}};
            if (entry.core) {
                if (!m.store_piece(id, piece_bytes)) return SW_E_IO;
            }
            m.source->set_piece(id, std::move(piece_bytes));
            vf.pieces.push_back(id);
        }
        m.files.emplace(entry.path, std::move(vf));
    }
    m.st_fetched.store(0);
    m.ready = true;
    return SW_OK;
}

void GateCore::shutdown()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        threads.swap(impl_->threads);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->held.clear();
    impl_->in_flight.clear();
    impl_->files.clear();
    impl_->source.reset();
    impl_->cache.reset();
    impl_->ready = false;
}

bool GateCore::initialized() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->ready;
}

sw_result GateCore::file_size(const std::string& path, uint64_t* out_size) const
{
    if (out_size) *out_size = 0;
    std::lock_guard<std::mutex> lock(impl_->mu);
    const VFile* vf = impl_->find(path);
    if (!vf) return SW_E_NOT_FOUND;
    if (out_size) *out_size = vf->size;
    return SW_OK;
}

bool GateCore::is_local(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const VFile* vf = impl_->find(path);
    if (!vf) return false;
    return std::all_of(vf->pieces.begin(), vf->pieces.end(),
                       [&](const PieceId& p) { return impl_->cache->has_piece(p); });
}

sw_result GateCore::read(const std::string& path, uint64_t offset, void* dst, uint64_t size,
                         uint64_t* out_bytes_read)
{
    if (out_bytes_read) *out_bytes_read = 0;
    Impl& m = *impl_;
    m.st_reads.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.ready) return SW_E_INVALID_ARGUMENT;
    const VFile* vf = m.find(path);
    if (!vf) return SW_E_NOT_FOUND;
    if (offset > vf->size) return SW_E_OUT_OF_RANGE;
    const uint64_t end = std::min<uint64_t>(vf->size, offset + size);
    if (end == offset) return SW_OK;
    const size_t first = static_cast<size_t>(offset / kPiece);
    const size_t last = static_cast<size_t>((end - 1) / kPiece);

    // Tudo ou nada: se falta alguma peça, pede as que faltam e devolve NOT_LOCAL.
    bool missing = false;
    for (size_t i = first; i <= last; ++i) {
        if (!m.cache->has_piece(vf->pieces[i])) {
            missing = true;
            (void)m.fetch_locked(vf->pieces[i]);
        }
    }
    if (missing) {
        m.st_not_local.fetch_add(1, std::memory_order_relaxed);
        return SW_E_NOT_LOCAL;
    }
    auto* out = static_cast<uint8_t*>(dst);
    uint64_t pos = offset;
    for (size_t i = first; i <= last; ++i) {
        auto lease = m.cache->read(vf->pieces[i]);
        if (!lease) return SW_E_IO;
        const uint64_t piece_start = static_cast<uint64_t>(i) * kPiece;
        const uint64_t from = pos - piece_start;
        const uint64_t n = std::min<uint64_t>(lease->size() - from, end - pos);
        std::memcpy(out, lease->data() + from, static_cast<size_t>(n));
        out += n;
        pos += n;
    }
    if (out_bytes_read) *out_bytes_read = end - offset;
    return SW_OK;
}

uint64_t GateCore::ensure_async(const std::string& path, EnsureCallback cb)
{
    Impl& m = *impl_;
    std::vector<std::shared_future<bool>> waits;
    std::vector<PieceId> pieces;
    uint64_t size = 0;
    const uint64_t id = m.next_ensure.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m.mu);
        if (!m.ready || !cb) return 0;
        const VFile* vf = m.find(path);
        if (!vf) return 0;
        pieces = vf->pieces;
        size = vf->size;
        for (const PieceId& p : pieces)
            waits.push_back(m.fetch_locked(p));
        m.st_ensures.fetch_add(1, std::memory_order_relaxed);
        // Thread de conclusão: o callback nunca roda dentro de ensure_async.
        m.threads.emplace_back([this, id, pieces, size, waits, cb = std::move(cb)]() {
            bool ok = true;
            for (const auto& w : waits)
                ok = w.get() && ok;
            sw_result result = ok ? SW_OK : SW_E_UNAVAILABLE;
            if (ok) {
                std::vector<Lease> leases;
                std::lock_guard<std::mutex> lock(impl_->mu);
                for (const PieceId& p : pieces) {
                    auto lease = impl_->cache->read(p);
                    if (!lease) {
                        result = SW_E_IO;
                        break;
                    }
                    leases.push_back(std::move(*lease));
                }
                if (result == SW_OK) impl_->held.emplace(id, std::move(leases));
            }
            cb(result, result == SW_OK ? size : 0, id);
        });
    }
    return id;
}

void GateCore::release(uint64_t ensure_id)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->held.erase(ensure_id);
}

GateStats GateCore::stats() const
{
    const Impl& m = *impl_;
    return GateStats{m.st_reads.load(), m.st_not_local.load(), m.st_ensures.load(),
                     m.st_fetched.load()};
}

} // namespace swgate
