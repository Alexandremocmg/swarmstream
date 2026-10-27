// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Testes da inspeção do cache (F1-18): o relatório bate com o que a CacheOwner guardou e a
// pasta inspecionada não muda um byte (nem a cauda inválida do journal é truncada).
#include "cache_inspect.hpp"
#include "cache_journal.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace swarmstream {

namespace {

namespace fs = std::filesystem;
using detail::InspectFileState;

constexpr StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_18_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    SW_CHECK(!ec);
}

PieceHash hash_of(uint32_t i)
{
    PieceHash h{};
    for (size_t b = 0; b < h.bytes.size(); ++b)
        h.bytes[b] = static_cast<uint8_t>(i * 13 + b + 1);
    return h;
}

uint32_t size_of(uint32_t i) { return 3000 + (i * 7919) % 200000; }

// Conteúdo e blocos determinísticos da peça i (3 blocos).
Result<size_t, CacheErrorCode> put(CacheOwner& o, uint32_t i)
{
    const uint32_t size = size_of(i);
    std::vector<std::byte> bytes(size);
    for (size_t k = 0; k < size; ++k)
        bytes[k] = static_cast<std::byte>((i * 131 + k * 7) & 0xFF);
    const std::array<uint32_t, 3> blocks{size / 3, size / 5, size - size / 3 - size / 5};
    auto r = CacheRequest::put(PieceId(i, hash_of(i)), bytes, blocks);
    return o.call(r);
}

// Conteúdo de todos os arquivos da pasta, por nome: para provar que nada mudou.
std::map<std::string, std::string> snapshot(const fs::path& dir)
{
    std::map<std::string, std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::ifstream f(e.path(), std::ios::binary);
        SW_CHECK(f.good());
        out[e.path().filename().string()] =
            std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return out;
}

void test_after_clean_stop()
{
    std::cout << "[Test] inspeção: cache parado normalmente, peça removida, pasta intocada\n";
    const fs::path dir = fresh_dir("clean");
    {
        auto o = CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 8, .layout = kLayout});
        SW_CHECK(o.has_value());
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(put(**o, i).has_value()); // slots 0–5: store-000 e store-001
        auto rm = CacheRequest::remove(PieceId(2, hash_of(2)));
        SW_CHECK((*o)->call(rm).has_value());
        SW_CHECK((*o)->stop().has_value());
    }
    const auto before = snapshot(dir);
    auto r = detail::inspect_cache(dir, kLayout);
    SW_CHECK(r.has_value());
    SW_CHECK(snapshot(dir) == before);

    SW_CHECK(r->index == InspectFileState::Ok && r->index_entries == 5);
    SW_CHECK(r->journal == InspectFileState::Ok && r->journal_truncated_bytes == 0);
    SW_CHECK(r->pieces.size() == 5 && r->pieces_in_missing_files == 0);
    uint64_t total = 0;
    for (const auto& p : r->pieces) {
        SW_CHECK(p.piece != hash_of(2));
        uint32_t i = 0;
        while (!(hash_of(i) == p.piece))
            ++i;
        SW_CHECK(i < 6 && p.size == size_of(i) && p.block_count == 3 && p.store_file_present);
        total += p.size;
    }
    SW_CHECK(r->total_piece_bytes == total);
    SW_CHECK(r->store_files.size() == 2 && r->store_files[0].id == 0 && r->store_files[1].id == 1);
    SW_CHECK(r->store_files[0].present && r->store_files[0].pieces == 3 &&
             r->store_files[1].pieces == 2);
    cleanup(dir);
}

void test_journal_tail_garbage_and_missing_file()
{
    std::cout << "[Test] inspeção: cauda rasgada e store ausente são relatados, sem consertar\n";
    const fs::path dir = fresh_dir("damaged");
    {
        auto o = CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 8, .layout = kLayout});
        SW_CHECK(o.has_value());
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(put(**o, i).has_value());
        SW_CHECK((*o)->stop().has_value());
    }
    {
        std::ofstream j(dir / "journal.bin", std::ios::binary | std::ios::app);
        const std::string junk(37, '\x5A');
        j.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        SW_CHECK(j.good());
    }
    std::error_code ec;
    SW_CHECK(fs::remove(StoreFiles::file_path(dir, 1), ec) && !ec);

    const auto before = snapshot(dir);
    auto r = detail::inspect_cache(dir, kLayout);
    SW_CHECK(r.has_value());
    SW_CHECK(snapshot(dir) == before); // a cauda de 37 bytes continua lá

    SW_CHECK(r->journal == InspectFileState::Ok && r->journal_truncated_bytes == 37);
    SW_CHECK(r->pieces.size() == 6 && r->pieces_in_missing_files == 2);
    SW_CHECK(r->store_files.size() == 2 && !r->store_files[1].present &&
             r->store_files[1].pieces == 2 && r->store_files[0].present);
    cleanup(dir);
}

void test_journal_only_and_invalid_index()
{
    std::cout << "[Test] inspeção: só journal (aplica COMMIT e FREE) e índice inválido\n";
    const fs::path dir = fresh_dir("journal_only");
    auto commit = [&](detail::Journal& j, uint64_t lsn, uint32_t i, uint32_t slot) {
        detail::JournalRecord rec;
        rec.type = detail::JournalType::CommitPiece;
        rec.lsn = lsn;
        rec.piece = hash_of(i);
        rec.store_file_id = slot / kLayout.slots_per_file;
        rec.slot_in_file = slot % kLayout.slots_per_file;
        rec.piece_size = 5000;
        rec.blocks = {1, 2};
        SW_CHECK(j.append(rec, nullptr, nullptr) == CacheErrorCode::Success);
    };
    SW_CHECK(detail::Journal::create_fresh(dir / "journal.bin", 1, nullptr, nullptr) ==
             CacheErrorCode::Success);
    {
        detail::JournalReplay info;
        auto j = detail::Journal::open(dir / "journal.bin", 0, [](const detail::JournalRecord&) {}, info);
        SW_CHECK(j.has_value());
        commit(*j, 2, 10, 0);
        commit(*j, 3, 11, 1);
        commit(*j, 4, 12, 5);
        detail::JournalRecord fr;
        fr.type = detail::JournalType::FreeSlot;
        fr.lsn = 5;
        fr.piece = hash_of(11);
        fr.store_file_id = 0;
        fr.slot_in_file = 1;
        SW_CHECK(j->append(fr, nullptr, nullptr) == CacheErrorCode::Success);
        SW_CHECK(j->sync() == CacheErrorCode::Success && j->close() == CacheErrorCode::Success);
    }
    {
        std::ofstream idx(dir / "index.bin", std::ios::binary);
        idx << "isto nao e um indice";
    }
    const auto before = snapshot(dir);
    auto r = detail::inspect_cache(dir, kLayout);
    SW_CHECK(r.has_value());
    SW_CHECK(snapshot(dir) == before);
    SW_CHECK(r->index == InspectFileState::Invalid);
    SW_CHECK(r->journal == InspectFileState::Ok && r->journal_applied == 4);
    SW_CHECK(r->pieces.size() == 2); // a 11 foi liberada
    SW_CHECK(r->pieces[0].piece == hash_of(10) && r->pieces[0].slot_in_file == 0);
    SW_CHECK(r->pieces[1].piece == hash_of(12) && r->pieces[1].store_file_id == 1 &&
             r->pieces[1].slot_in_file == 1);
    SW_CHECK(r->pieces_in_missing_files == 2); // não há store-XXX.bin nesta pasta
    cleanup(dir);
}

void test_empty_and_bad_dir()
{
    std::cout << "[Test] inspeção: pasta vazia é um cache vazio; caminho inexistente é erro\n";
    const fs::path dir = fresh_dir("empty");
    auto r = detail::inspect_cache(dir, kLayout);
    SW_CHECK(r.has_value());
    SW_CHECK(r->index == InspectFileState::Missing && r->journal == InspectFileState::Missing &&
             r->pieces.empty() && r->store_files.empty());
    SW_CHECK(fs::is_empty(dir)); // não criou index.bin nem journal.bin
    auto bad = detail::inspect_cache(dir / "nao-existe", kLayout);
    SW_CHECK(!bad.has_value());
    cleanup(dir);
}

} // namespace

} // namespace swarmstream

int main()
{
    std::cout << "--- Inspeção do cache (F1-18) ---\n";
    swarmstream::test_after_clean_stop();
    swarmstream::test_journal_tail_garbage_and_missing_file();
    swarmstream::test_journal_only_and_invalid_index();
    swarmstream::test_empty_and_bad_dir();
    std::cout << "-> Inspect tests passed.\n";
    return 0;
}
