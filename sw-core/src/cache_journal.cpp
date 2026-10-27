// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "cache_journal.hpp"

#include "swarmstream/xxh3.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace swarmstream::detail {

namespace {

namespace fs = std::filesystem;

// ---- little-endian, byte a byte (formato em disco independe da máquina)

void put16(std::byte* p, uint16_t v) noexcept
{
    p[0] = static_cast<std::byte>(v);
    p[1] = static_cast<std::byte>(v >> 8);
}
void put32(std::byte* p, uint32_t v) noexcept
{
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<std::byte>(v >> (8 * i));
}
void put64(std::byte* p, uint64_t v) noexcept
{
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<std::byte>(v >> (8 * i));
}
uint16_t get16(const std::byte* p) noexcept
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t get32(const std::byte* p) noexcept
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t get64(const std::byte* p) noexcept
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

fs::path tmp_of(const fs::path& file)
{
    fs::path t = file;
    t += ".tmp";
    return t;
}

int64_t now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Grava `bytes` num arquivo novo `tmp` (zerado antes) e sincroniza.
CacheErrorCode write_whole(const fs::path& tmp, std::span<const std::byte> bytes)
{
    File f;
    if (auto rc = f.open(tmp); rc != CacheErrorCode::Success) return rc;
    CacheErrorCode rc = f.truncate(0); // um tmp que sobrou de uma queda antiga
    if (rc == CacheErrorCode::Success) rc = f.write_at(0, bytes.data(), bytes.size());
    if (rc == CacheErrorCode::Success) rc = f.sync();
    const CacheErrorCode closed = f.close();
    return rc != CacheErrorCode::Success ? rc : closed;
}

} // namespace

// ------------------------------------------------------------------ registros

void encode_record(const JournalRecord& r, std::vector<std::byte>& out)
{
    const size_t n = r.type == JournalType::CommitPiece ? r.blocks.size() : 0;
    const size_t base = out.size();
    out.resize(base + 80 + 8 * n);
    std::byte* p = out.data() + base;
    put32(p + 0, kRecordMagic);
    put16(p + 4, static_cast<uint16_t>(r.type));
    put16(p + 6, r.flags);
    put64(p + 8, r.lsn);
    put64(p + 16, static_cast<uint64_t>(r.timestamp_us));
    std::memcpy(p + 24, r.piece.bytes.data(), 32);
    put32(p + 56, r.store_file_id);
    put32(p + 60, r.slot_in_file);
    put32(p + 64, r.piece_size);
    put32(p + 68, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i)
        put64(p + 72 + 8 * i, r.blocks[i]);
    put64(p + 72 + 8 * n, xxh3_64(p, 72 + 8 * n));
}

std::array<std::byte, kJournalHeaderSize> encode_journal_header(uint64_t checkpoint_lsn,
                                                                uint64_t last_lsn)
{
    std::array<std::byte, kJournalHeaderSize> h{};
    put64(h.data() + 0, kJournalMagic);
    put16(h.data() + 8, kJournalVersion);
    put16(h.data() + 10, 0);
    put32(h.data() + 12, kJournalPageSize);
    put64(h.data() + 16, checkpoint_lsn);
    put64(h.data() + 24, last_lsn);
    put64(h.data() + 56, xxh3_64(h.data(), 56));
    return h;
}

// ------------------------------------------------------------------ Journal

CacheErrorCode Journal::create_fresh(const fs::path& file, uint64_t checkpoint_lsn, CrashHook hook,
                                     void* hook_context)
{
    std::vector<std::byte> bytes;
    const auto header = encode_journal_header(checkpoint_lsn, checkpoint_lsn);
    bytes.insert(bytes.end(), header.begin(), header.end());
    JournalRecord cp;
    cp.type = JournalType::Checkpoint;
    cp.lsn = checkpoint_lsn == 0 ? 1 : checkpoint_lsn; // LSN 0 não existe
    cp.timestamp_us = now_us();
    encode_record(cp, bytes);
    const fs::path tmp = tmp_of(file);
    if (auto rc = write_whole(tmp, bytes); rc != CacheErrorCode::Success) return rc;
    if (hook) hook(CrashPoint::CheckpointJournalTmp, hook_context);
    return replace_file(tmp, file);
}

Result<Journal, CacheErrorCode>
Journal::open(const fs::path& file, uint64_t after_lsn,
              const std::function<void(const JournalRecord&)>& apply, JournalReplay& info)
{
    info = JournalReplay{};
    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    if (ec) return Unexpected(CacheErrorCode::IoError);
    if (!exists) {
        info.created = true;
        if (auto rc = create_fresh(file, after_lsn, nullptr, nullptr);
            rc != CacheErrorCode::Success)
            return Unexpected(rc);
    }

    Journal j;
    if (auto rc = j.file_.open(file); rc != CacheErrorCode::Success) return Unexpected(rc);
    uint64_t total = 0;
    if (auto rc = j.file_.size(total); rc != CacheErrorCode::Success) return Unexpected(rc);

    // Cabeçalho
    std::array<std::byte, kJournalHeaderSize> h{};
    bool header_ok = total >= kJournalHeaderSize &&
                     j.file_.read_at(0, h.data(), h.size()) == CacheErrorCode::Success;
    header_ok = header_ok && get64(h.data()) == kJournalMagic &&
                get16(h.data() + 8) == kJournalVersion && get16(h.data() + 10) == 0 &&
                get32(h.data() + 12) == kJournalPageSize &&
                get64(h.data() + 56) == xxh3_64(h.data(), 56);
    if (!header_ok) {
        // Sem cabeçalho confiável não há como confiar nos registros: recomeça do índice.
        info.header_ok = false;
        if (auto rc = j.file_.close(); rc != CacheErrorCode::Success) return Unexpected(rc);
        if (auto rc = create_fresh(file, after_lsn, nullptr, nullptr);
            rc != CacheErrorCode::Success)
            return Unexpected(rc);
        JournalReplay ignored;
        auto again = open(file, after_lsn, apply, ignored);
        if (again) {
            info.checkpoint_lsn = ignored.checkpoint_lsn;
            info.last_lsn = ignored.last_lsn;
            info.truncated_bytes = total; // tudo o que havia foi descartado
        }
        return again;
    }
    info.checkpoint_lsn = get64(h.data() + 16);
    info.last_lsn = info.checkpoint_lsn;

    // Registros (04 §5, passo 3)
    uint64_t pos = kJournalHeaderSize;
    uint64_t prev_lsn = 0;
    std::array<std::byte, kRecordFixedSize> fixed{};
    std::vector<std::byte> rec;
    JournalRecord r;
    for (;;) {
        if (total - pos < kRecordFixedSize) break; // fim (ou cauda rasgada)
        if (j.file_.read_at(pos, fixed.data(), fixed.size()) != CacheErrorCode::Success) break;
        if (get32(fixed.data()) != kRecordMagic) break;
        const uint16_t type = get16(fixed.data() + 4);
        if (type < 1 || type > 3) break;
        const uint32_t piece_size = get32(fixed.data() + 64);
        const uint32_t block_count = get32(fixed.data() + 68);
        // Limites ANTES de ler os checksums: o pior caso é ler ~8 MiB, nunca o journal inteiro.
        if (type == static_cast<uint16_t>(JournalType::CommitPiece)) {
            if (piece_size < 1 || piece_size > kMaxPieceSize) break;
            if (block_count < 1 || block_count > piece_size) break;
        } else if (block_count != 0) {
            break;
        }
        const uint64_t len = 80 + 8 * uint64_t{block_count};
        if (len > total - pos) break;
        rec.resize(static_cast<size_t>(len));
        if (j.file_.read_at(pos, rec.data(), rec.size()) != CacheErrorCode::Success) break;
        const size_t body = rec.size() - 8;
        if (get64(rec.data() + body) != xxh3_64(rec.data(), body)) break;
        const uint64_t lsn = get64(rec.data() + 8);
        if (lsn == 0 || lsn <= prev_lsn) break;

        r.type = static_cast<JournalType>(type);
        r.flags = get16(rec.data() + 6);
        r.lsn = lsn;
        r.timestamp_us = static_cast<int64_t>(get64(rec.data() + 16));
        std::memcpy(r.piece.bytes.data(), rec.data() + 24, 32);
        r.store_file_id = get32(rec.data() + 56);
        r.slot_in_file = get32(rec.data() + 60);
        r.piece_size = piece_size;
        r.blocks.resize(block_count);
        for (uint32_t i = 0; i < block_count; ++i)
            r.blocks[i] = get64(rec.data() + 72 + 8 * i);

        prev_lsn = lsn;
        info.last_lsn = std::max(info.last_lsn, lsn);
        ++info.records;
        if (lsn > after_lsn && r.type != JournalType::Checkpoint) {
            apply(r);
            ++info.applied;
        }
        pos += len;
    }
    if (pos < total) {
        // Primeira escrita incompleta ou inválida: trunca ali (04 §5) para os appends seguintes
        // continuarem de um ponto válido.
        info.truncated_bytes = total - pos;
        if (auto rc = j.file_.truncate(pos); rc != CacheErrorCode::Success) return Unexpected(rc);
        if (auto rc = j.file_.sync(); rc != CacheErrorCode::Success) return Unexpected(rc);
    }
    j.end_ = pos;
    return j;
}

CacheErrorCode Journal::append(const JournalRecord& r, CrashHook hook, void* hook_context)
{
    buffer_.clear();
    encode_record(r, buffer_);
    if (fail_append_ != CacheErrorCode::Success) {
        const CacheErrorCode injected = std::exchange(fail_append_, CacheErrorCode::Success);
        if (auto rc = file_.write_at(end_, buffer_.data(), buffer_.size() / 2);
            rc != CacheErrorCode::Success)
            return rc;
        return injected;
    }
    if (hook) { // teste de queda: metade, o gancho, o resto
        const size_t half = buffer_.size() / 2;
        if (auto rc = file_.write_at(end_, buffer_.data(), half); rc != CacheErrorCode::Success)
            return rc;
        hook(CrashPoint::JournalHalfWritten, hook_context);
        if (auto rc = file_.write_at(end_ + half, buffer_.data() + half, buffer_.size() - half);
            rc != CacheErrorCode::Success)
            return rc;
    } else if (auto rc = file_.write_at(end_, buffer_.data(), buffer_.size());
               rc != CacheErrorCode::Success) {
        return rc;
    }
    end_ += buffer_.size();
    if (hook) hook(CrashPoint::AfterJournalAppend, hook_context);
    return CacheErrorCode::Success;
}

CacheErrorCode Journal::sync() noexcept
{
    if (fail_next_sync_) {
        fail_next_sync_ = false;
        return CacheErrorCode::IoError;
    }
    const CacheErrorCode rc = file_.sync();
    if (rc == CacheErrorCode::Success) ++syncs_;
    return rc;
}

CacheErrorCode Journal::close() noexcept { return file_.close(); }

// ------------------------------------------------------------------ index.bin

CacheErrorCode write_index_file(const fs::path& file, const fs::path& tmp, uint64_t checkpoint_lsn,
                                const StoreLayout& layout,
                                std::span<const IndexEntry* const> entries, CrashHook hook,
                                void* hook_context)
{
    uint64_t checksums = 0;
    for (const IndexEntry* e : entries)
        checksums += e->blocks.size();
    if (entries.size() > std::numeric_limits<uint32_t>::max() ||
        checksums > std::numeric_limits<uint32_t>::max()) {
        return CacheErrorCode::InvalidSize;
    }
    std::vector<std::byte> b(kIndexHeaderSize + kIndexEntrySize * entries.size() + 8 * checksums);
    std::byte* h = b.data();
    put64(h + 0, kIndexMagic);
    put16(h + 8, kIndexVersion);
    put16(h + 10, 0);
    put32(h + 12, static_cast<uint32_t>(entries.size()));
    put64(h + 16, checksums);
    put64(h + 24, checkpoint_lsn);
    put32(h + 32, layout.slot_size);
    put32(h + 36, layout.slots_per_file);
    std::byte* e_out = b.data() + kIndexHeaderSize;
    std::byte* c_out = e_out + kIndexEntrySize * entries.size();
    uint32_t cks_first = 0;
    for (const IndexEntry* e : entries) {
        const uint32_t file_id = e->slot / layout.slots_per_file;
        if (file_id > std::numeric_limits<uint16_t>::max()) return CacheErrorCode::InvalidSize;
        std::memcpy(e_out + 0, e->piece.bytes.data(), 32);
        put32(e_out + 32, e->slot % layout.slots_per_file);
        put32(e_out + 36, e->size);
        put32(e_out + 40, e->access_counter);
        put32(e_out + 44, cks_first);
        put32(e_out + 48, static_cast<uint32_t>(e->blocks.size()));
        put16(e_out + 52, static_cast<uint16_t>(file_id));
        e_out[54] = static_cast<std::byte>(e->state);
        e_out[55] = static_cast<std::byte>(e->flags);
        e_out += kIndexEntrySize;
        for (uint64_t c : e->blocks) {
            put64(c_out, c);
            c_out += 8;
        }
        cks_first += static_cast<uint32_t>(e->blocks.size());
    }
    put64(h + 56, xxh3_64(b.data(), b.size())); // calculado com o campo zerado
    if (auto rc = write_whole(tmp, b); rc != CacheErrorCode::Success) return rc;
    if (hook) hook(CrashPoint::CheckpointIndexTmp, hook_context);
    if (auto rc = replace_file(tmp, file); rc != CacheErrorCode::Success) return rc;
    if (hook) hook(CrashPoint::CheckpointIndexRenamed, hook_context);
    return CacheErrorCode::Success;
}

IndexLoad read_index_file(const fs::path& file, const StoreLayout& layout, LoadedIndex& out)
{
    out = LoadedIndex{};
    std::error_code ec;
    if (!fs::exists(file, ec) || ec) return IndexLoad::Missing;
    File f;
    if (f.open(file) != CacheErrorCode::Success) return IndexLoad::Invalid;
    uint64_t total = 0;
    if (f.size(total) != CacheErrorCode::Success || total < kIndexHeaderSize ||
        total > std::numeric_limits<size_t>::max()) {
        (void)f.close();
        return IndexLoad::Invalid;
    }
    std::array<std::byte, kIndexHeaderSize> h{};
    if (f.read_at(0, h.data(), h.size()) != CacheErrorCode::Success) {
        (void)f.close();
        return IndexLoad::Invalid;
    }
    const uint32_t count = get32(h.data() + 12);
    const uint64_t checksums = get64(h.data() + 16);
    // Os tamanhos declarados têm de bater com o arquivo ANTES de alocar.
    const bool header_ok =
        get64(h.data()) == kIndexMagic && get16(h.data() + 8) == kIndexVersion &&
        get16(h.data() + 10) == 0 && get32(h.data() + 32) == layout.slot_size &&
        get32(h.data() + 36) == layout.slots_per_file &&
        checksums <= (total - kIndexHeaderSize) / 8 &&
        total == kIndexHeaderSize + kIndexEntrySize * uint64_t{count} + 8 * checksums;
    if (!header_ok) {
        (void)f.close();
        return IndexLoad::Invalid;
    }
    std::vector<std::byte> b(static_cast<size_t>(total));
    const bool read_ok = f.read_at(0, b.data(), b.size()) == CacheErrorCode::Success;
    if (f.close() != CacheErrorCode::Success || !read_ok) return IndexLoad::Invalid;
    const uint64_t stored = get64(b.data() + 56);
    put64(b.data() + 56, 0);
    if (xxh3_64(b.data(), b.size()) != stored) return IndexLoad::Invalid;

    out.checkpoint_lsn = get64(b.data() + 24);
    out.entries.resize(count);
    const std::byte* e_in = b.data() + kIndexHeaderSize;
    const std::byte* c_in = e_in + kIndexEntrySize * uint64_t{count};
    std::unordered_set<SlotId> slots;
    std::unordered_set<PieceHash> pieces;
    for (uint32_t i = 0; i < count; ++i, e_in += kIndexEntrySize) {
        IndexEntry& e = out.entries[i];
        std::memcpy(e.piece.bytes.data(), e_in, 32);
        const uint32_t slot_in_file = get32(e_in + 32);
        e.size = get32(e_in + 36);
        e.access_counter = get32(e_in + 40);
        const uint32_t cks_first = get32(e_in + 44);
        const uint32_t block_count = get32(e_in + 48);
        const uint32_t file_id = get16(e_in + 52);
        e.state = static_cast<uint8_t>(e_in[54]);
        e.flags = static_cast<uint8_t>(e_in[55]);
        const uint64_t slot = uint64_t{file_id} * layout.slots_per_file + slot_in_file;
        const bool ok = slot_in_file < layout.slots_per_file && slot < kInvalidSlot &&
                        e.size >= 1 && e.size <= layout.slot_size && e.size <= kMaxPieceSize &&
                        block_count >= 1 && block_count <= e.size &&
                        uint64_t{cks_first} + block_count <= checksums && e.state == 2 &&
                        slots.insert(static_cast<SlotId>(slot)).second &&
                        pieces.insert(e.piece).second;
        if (!ok) {
            out = LoadedIndex{};
            return IndexLoad::Invalid; // um índice incoerente não é usado em parte
        }
        e.slot = static_cast<SlotId>(slot);
        e.blocks.resize(block_count);
        for (uint32_t k = 0; k < block_count; ++k)
            e.blocks[k] = get64(c_in + 8 * (cks_first + k));
    }
    return IndexLoad::Ok;
}

} // namespace swarmstream::detail
