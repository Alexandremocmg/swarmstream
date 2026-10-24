// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-11: formato e recuperação do journal.bin e do index.bin (docs/04 §2, §4 e §5).
//
// - O layout dos registros e dos cabeçalhos é conferido byte a byte contra o 04 §2.
// - Cauda rasgada: o journal é cortado em TODO comprimento possível; a recuperação aplica só
//   os registros inteiros e trunca o resto (o que uma queda no meio de uma escrita deixa).
// - Corrupção: cada byte de um registro do meio é trocado; a leitura para nele e nada depois
//   dele é aplicado (sem ressincronização, 04 §5).
// - Limites: registro com block_count enorme, tamanhos fora dos limites, tipo desconhecido e
//   LSN que não cresce param a leitura sem ler (nem alocar) além do limite.

#include "cache_journal.hpp"
#include "check.hpp"
#include "swarmstream/xxh3.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace swarmstream {

namespace {

namespace fs = std::filesystem;
using detail::JournalRecord;
using detail::JournalType;

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_11j_") + name + "_" +
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

std::vector<std::byte> read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

void write_file(const fs::path& p, std::span<const std::byte> bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    SW_CHECK(out.good());
}

uint32_t get32(const std::vector<std::byte>& b, size_t off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    return v;
}
uint64_t get64(const std::vector<std::byte>& b, size_t off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
}

PieceHash hash_of(uint8_t seed)
{
    PieceHash h{};
    for (size_t i = 0; i < h.bytes.size(); ++i)
        h.bytes[i] = static_cast<uint8_t>(seed * 3 + i);
    return h;
}

JournalRecord commit(uint64_t lsn, uint8_t seed, uint32_t slot, uint32_t size, size_t blocks)
{
    JournalRecord r;
    r.type = JournalType::CommitPiece;
    r.lsn = lsn;
    r.timestamp_us = 1234567;
    r.piece = hash_of(seed);
    r.store_file_id = 0;
    r.slot_in_file = slot;
    r.piece_size = size;
    for (size_t i = 0; i < blocks; ++i)
        r.blocks.push_back(0x1111111111111111ULL * (i + 1) + seed);
    return r;
}

struct Applied
{
    std::vector<JournalRecord> records;
    std::function<void(const JournalRecord&)> fn()
    {
        return [this](const JournalRecord& r) { records.push_back(r); };
    }
};

// ---------------------------------------------------------------- layout

void test_record_layout()
{
    std::cout << "[Test] journal: layout do registro e do cabeçalho (04 §2)\n";
    std::vector<std::byte> b;
    JournalRecord c = commit(42, 7, 5, 300000, 2);
    c.flags = detail::kFlagPinned;
    c.store_file_id = 3;
    detail::encode_record(c, b);
    SW_CHECK(b.size() == 80 + 8 * 2);
    SW_CHECK(get32(b, 0) == 0x57414C52);
    SW_CHECK((get32(b, 4) & 0xFFFF) == 1 && (get32(b, 4) >> 16) == 1); // tipo e flags
    SW_CHECK(get64(b, 8) == 42 && get64(b, 16) == 1234567);
    SW_CHECK(std::memcmp(b.data() + 24, c.piece.bytes.data(), 32) == 0);
    SW_CHECK(get32(b, 56) == 3 && get32(b, 60) == 5 && get32(b, 64) == 300000 && get32(b, 68) == 2);
    SW_CHECK(get64(b, 72) == c.blocks[0] && get64(b, 80) == c.blocks[1]);
    SW_CHECK(get64(b, 88) == xxh3_64(b.data(), 88));

    std::vector<std::byte> f;
    JournalRecord fr;
    fr.type = JournalType::FreeSlot;
    fr.lsn = 43;
    fr.piece = c.piece;
    fr.blocks = {1, 2, 3}; // FREE_SLOT nunca leva checksums
    detail::encode_record(fr, f);
    SW_CHECK(f.size() == 80 && get32(f, 68) == 0 && get64(f, 72) == xxh3_64(f.data(), 72));

    const auto h = detail::encode_journal_header(10, 20);
    const std::vector<std::byte> hv(h.begin(), h.end());
    SW_CHECK(get64(hv, 0) == 0x004E52554F4A5753ULL); // "SWJOURN\0"
    SW_CHECK(std::memcmp(hv.data(), "SWJOURN", 8) == 0);
    SW_CHECK((get32(hv, 8) & 0xFFFF) == 1 && get32(hv, 12) == 4096);
    SW_CHECK(get64(hv, 16) == 10 && get64(hv, 24) == 20 && get64(hv, 56) == xxh3_64(hv.data(), 56));
}

// Um journal pronto: CHECKPOINT (do create_fresh) e três COMMIT_PIECE de tamanhos diferentes.
// Devolve os limites (fim de cada registro) para os testes de corte.
std::vector<uint64_t> build_journal(const fs::path& file)
{
    detail::JournalReplay info;
    Applied a;
    auto j = detail::Journal::open(file, 0, a.fn(), info);
    SW_CHECK(j.has_value() && info.created && info.records == 1 && info.applied == 0);
    std::vector<uint64_t> ends{j->size_bytes()};
    const JournalRecord recs[] = {commit(2, 1, 0, 1000, 1), commit(3, 2, 1, 500000, 7),
                                  commit(4, 3, 2, 70000, 3)};
    for (const auto& r : recs) {
        SW_CHECK(j->append(r, nullptr, nullptr) == CacheErrorCode::Success);
        ends.push_back(j->size_bytes());
    }
    SW_CHECK(j->sync() == CacheErrorCode::Success && j->close() == CacheErrorCode::Success);
    return ends; // [fim do CHECKPOINT, fim c1, fim c2, fim c3]
}

void test_replay_roundtrip()
{
    std::cout << "[Test] journal: reabrir aplica os registros na ordem, com os checksums\n";
    const fs::path dir = fresh_dir("roundtrip");
    const fs::path file = dir / "journal.bin";
    const auto ends = build_journal(file);
    SW_CHECK(ends.size() == 4 && ends[0] == 64 + 80 && ends[1] == ends[0] + 88);
    detail::JournalReplay info;
    Applied a;
    auto j = detail::Journal::open(file, 0, a.fn(), info);
    SW_CHECK(j.has_value() && !info.created && info.header_ok && info.truncated_bytes == 0);
    SW_CHECK(info.records == 4 && info.applied == 3 && info.last_lsn == 4);
    SW_CHECK(a.records.size() == 3 && a.records[1].blocks.size() == 7 &&
             a.records[1].piece_size == 500000 && a.records[1].slot_in_file == 1);
    SW_CHECK(a.records[1].blocks[6] == commit(3, 2, 1, 500000, 7).blocks[6]);
    SW_CHECK(j->close() == CacheErrorCode::Success);
    // after_lsn pula o que o índice já tem.
    Applied b;
    auto j2 = detail::Journal::open(file, 3, b.fn(), info);
    SW_CHECK(j2.has_value() && info.applied == 1 && b.records[0].lsn == 4);
    SW_CHECK(j2->close() == CacheErrorCode::Success);
    cleanup(dir);
}

void test_replay_torn_tail_every_length()
{
    std::cout << "[Test] journal: cauda rasgada em todo comprimento possível\n";
    const fs::path dir = fresh_dir("torn");
    const fs::path file = dir / "journal.bin";
    const auto ends = build_journal(file);
    const auto full = read_file(file);
    SW_CHECK(full.size() == ends.back());
    const fs::path cut = dir / "cut.bin";
    uint64_t cases = 0;
    for (size_t len = 64; len <= full.size(); ++len) {
        write_file(cut, std::span(full).first(len));
        detail::JournalReplay info;
        Applied a;
        auto j = detail::Journal::open(cut, 0, a.fn(), info);
        SW_CHECK(j.has_value() && info.header_ok);
        size_t whole = 0; // registros inteiros dentro de `len`
        uint64_t boundary = 64;
        for (size_t k = 0; k < ends.size(); ++k) {
            if (ends[k] <= len) {
                whole = k + 1;
                boundary = ends[k];
            }
        }
        const size_t commits = whole == 0 ? 0 : whole - 1;
        SW_CHECK(info.records == whole && a.records.size() == commits);
        SW_CHECK(info.truncated_bytes == len - boundary && j->size_bytes() == boundary);
        SW_CHECK(j->close() == CacheErrorCode::Success);
        SW_CHECK(fs::file_size(cut) == boundary); // o resto foi cortado do arquivo
        ++cases;
    }
    std::cout << "    " << cases << " comprimentos conferidos\n";
    cleanup(dir);
}

void test_replay_stops_at_corruption()
{
    std::cout << "[Test] journal: byte trocado no 2º registro descarta ele e o resto\n";
    const fs::path dir = fresh_dir("flip");
    const fs::path file = dir / "journal.bin";
    const auto ends = build_journal(file);
    const auto full = read_file(file);
    const fs::path bad = dir / "bad.bin";
    for (uint64_t off = ends[1]; off < ends[2]; ++off) { // cada byte do 2º COMMIT
        auto copy = full;
        copy[off] ^= std::byte{0x5A};
        write_file(bad, copy);
        detail::JournalReplay info;
        Applied a;
        auto j = detail::Journal::open(bad, 0, a.fn(), info);
        SW_CHECK(j.has_value());
        SW_CHECK(a.records.size() == 1 && a.records[0].lsn == 2); // só o 1º COMMIT
        SW_CHECK(j->size_bytes() == ends[1] && info.truncated_bytes == full.size() - ends[1]);
        SW_CHECK(j->close() == CacheErrorCode::Success);
    }
    // Cabeçalho estragado: nenhum registro é aproveitado e o journal é recriado.
    for (size_t off : {size_t{0}, size_t{9}, size_t{17}, size_t{60}}) {
        auto copy = full;
        copy[off] ^= std::byte{0x01};
        write_file(bad, copy);
        detail::JournalReplay info;
        Applied a;
        auto j = detail::Journal::open(bad, 7, a.fn(), info);
        SW_CHECK(j.has_value() && !info.header_ok && a.records.empty());
        SW_CHECK(info.checkpoint_lsn == 7 && j->size_bytes() == 64 + 80);
        SW_CHECK(j->close() == CacheErrorCode::Success);
    }
    cleanup(dir);
}

// Monta um registro à mão (CRC certo) com campos fora dos limites.
std::vector<std::byte> raw_record(uint16_t type, uint64_t lsn, uint32_t piece_size,
                                  uint32_t block_count, size_t checksum_bytes)
{
    std::vector<std::byte> b(72 + checksum_bytes + 8);
    auto put = [&](size_t off, uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            b[off + i] = static_cast<std::byte>(v >> (8 * i));
    };
    put(0, 0x57414C52, 4);
    put(4, type, 2);
    put(8, lsn, 8);
    put(64, piece_size, 4);
    put(68, block_count, 4);
    put(72 + checksum_bytes, xxh3_64(b.data(), 72 + checksum_bytes), 8);
    return b;
}

void test_replay_bounds()
{
    std::cout << "[Test] journal: limites conferidos antes de ler os checksums\n";
    const fs::path dir = fresh_dir("bounds");
    const fs::path file = dir / "journal.bin";
    const auto ends = build_journal(file);
    const auto base = read_file(file);
    struct Case
    {
        const char* name;
        std::vector<std::byte> rec;
    };
    const Case cases[] = {
        {"block_count enorme (não cabe no arquivo)", raw_record(1, 9, 1048576, 0x00100000, 0)},
        {"block_count > piece_size", raw_record(1, 9, 10, 11, 88)},
        {"piece_size 0", raw_record(1, 9, 0, 1, 8)},
        {"piece_size > 1 MiB", raw_record(1, 9, 1048577, 1, 8)},
        {"block_count 0 num COMMIT", raw_record(1, 9, 100, 0, 0)},
        {"FREE_SLOT com block_count", raw_record(2, 9, 0, 1, 8)},
        {"tipo desconhecido", raw_record(7, 9, 0, 0, 0)},
        {"LSN que não cresce", raw_record(2, 4, 0, 0, 0)},
        {"LSN 0", raw_record(2, 0, 0, 0, 0)},
    };
    const fs::path f = dir / "case.bin";
    for (const Case& c : cases) {
        auto bytes = base;
        bytes.insert(bytes.end(), c.rec.begin(), c.rec.end());
        auto valid = raw_record(2, 50, 0, 0, 0); // um registro bom depois: não pode ser lido
        bytes.insert(bytes.end(), valid.begin(), valid.end());
        write_file(f, bytes);
        detail::JournalReplay info;
        Applied a;
        auto j = detail::Journal::open(f, 0, a.fn(), info);
        SW_CHECK(j.has_value());
        if (a.records.size() != 3 || j->size_bytes() != ends.back()) {
            std::cout << "    falhou: " << c.name << "\n";
        }
        SW_CHECK(a.records.size() == 3 && j->size_bytes() == ends.back());
        SW_CHECK(j->close() == CacheErrorCode::Success);
    }
    std::cout << "    " << std::size(cases) << " registros fora dos limites recusados\n";
    cleanup(dir);
}

// ---------------------------------------------------------------- index.bin

void test_index_file()
{
    std::cout << "[Test] index.bin: ida e volta, e qualquer defeito descarta o índice inteiro\n";
    const fs::path dir = fresh_dir("index");
    const fs::path file = dir / "index.bin";
    const fs::path tmp = dir / "index.bin.tmp";
    const StoreLayout layout{.slots_per_file = 4, .slot_size = 1 << 20};
    detail::LoadedIndex loaded;
    SW_CHECK(detail::read_index_file(file, layout, loaded) == detail::IndexLoad::Missing);

    std::vector<detail::IndexEntry> entries(3);
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].piece = hash_of(static_cast<uint8_t>(i + 10));
        entries[i].slot = static_cast<SlotId>(i * 3); // 0, 3 (arquivo 0) e 6 (arquivo 1)
        entries[i].size = static_cast<uint32_t>(1000 * (i + 1));
        for (size_t k = 0; k <= i; ++k)
            entries[i].blocks.push_back(0xABC0 + i * 16 + k);
    }
    std::vector<const detail::IndexEntry*> ptrs{&entries[0], &entries[1], &entries[2]};
    SW_CHECK(detail::write_index_file(file, tmp, 77, layout, ptrs, nullptr, nullptr) ==
             CacheErrorCode::Success);
    SW_CHECK(!fs::exists(tmp));
    const auto bytes = read_file(file);
    SW_CHECK(bytes.size() == 64 + 56 * 3 + 8 * 6);
    SW_CHECK(std::memcmp(bytes.data(), "SWINDEX", 8) == 0);
    SW_CHECK((get32(bytes, 64 + 56 * 2 + 52) & 0xFFFF) == 1 && get32(bytes, 64 + 56 * 2 + 32) == 2);

    SW_CHECK(detail::read_index_file(file, layout, loaded) == detail::IndexLoad::Ok);
    SW_CHECK(loaded.checkpoint_lsn == 77 && loaded.entries.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        SW_CHECK(loaded.entries[i].piece == entries[i].piece &&
                 loaded.entries[i].slot == entries[i].slot);
        SW_CHECK(loaded.entries[i].size == entries[i].size &&
                 loaded.entries[i].blocks == entries[i].blocks);
    }
    // Layout diferente: o índice é de outro cache.
    SW_CHECK(detail::read_index_file(file, StoreLayout{.slots_per_file = 8, .slot_size = 1 << 20},
                                     loaded) == detail::IndexLoad::Invalid);
    // Cada byte trocado e cada corte invalida o arquivo inteiro.
    for (size_t off = 0; off < bytes.size(); ++off) {
        auto copy = bytes;
        copy[off] ^= std::byte{0x80};
        write_file(file, copy);
        SW_CHECK(detail::read_index_file(file, layout, loaded) == detail::IndexLoad::Invalid);
        SW_CHECK(loaded.entries.empty());
    }
    write_file(file, std::span(bytes).first(bytes.size() - 1));
    SW_CHECK(detail::read_index_file(file, layout, loaded) == detail::IndexLoad::Invalid);
    // Duas entradas no mesmo slot: índice incoerente, recusado.
    entries[1].slot = entries[0].slot;
    SW_CHECK(detail::write_index_file(file, tmp, 78, layout, ptrs, nullptr, nullptr) ==
             CacheErrorCode::Success);
    SW_CHECK(detail::read_index_file(file, layout, loaded) == detail::IndexLoad::Invalid);
    std::cout << "    " << bytes.size() << " bytes trocados um a um: todos recusados\n";
    cleanup(dir);
}

} // namespace

} // namespace swarmstream

int main()
{
    std::cout << "--- Journal e índice (F1-11) ---\n";
    swarmstream::test_record_layout();
    swarmstream::test_replay_roundtrip();
    swarmstream::test_replay_torn_tail_every_length();
    swarmstream::test_replay_stops_at_corruption();
    swarmstream::test_replay_bounds();
    swarmstream::test_index_file();
    std::cout << "-> Journal tests passed.\n";
    return 0;
}
