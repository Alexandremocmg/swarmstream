// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-11: a CacheOwner com índice e journal de verdade, entre aberturas (docs/04 §3 a §5).
//
// - O que foi confirmado (Put/Remove com sucesso) volta depois de fechar e abrir, por
//   checkpoint ou relendo o journal; os checksums por bloco também.
// - Nada estragado sai do cache: um byte trocado no store é pego pelo XXH3 do bloco, a peça
//   sai do índice (FREE_SLOT durável) e Get devolve CorruptedChecksum.
// - Obrigações do F1-10: store-XXX.bin ausente vira peça perdida; teto menor ao reabrir não dá
//   IoError (arquivos além do teto são apagados ou encurtados); falha de sync deixa o cache
//   quebrado e reabrir recupera.
// O teste de queda com o processo morto de verdade está em crash_test.py (sw-core-crash-tool).

#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace swarmstream {

namespace {

namespace fs = std::filesystem;

constexpr StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_11r_") + name + "_" +
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

PieceId piece(uint32_t i)
{
    PieceHash h{};
    for (size_t b = 0; b < h.bytes.size(); ++b)
        h.bytes[b] = static_cast<uint8_t>(i * 13 + b + 1);
    return PieceId(i, h);
}

// Peça i: tamanho e blocos determinísticos (3 blocos de tamanhos diferentes).
struct Content
{
    std::vector<std::byte> bytes;
    std::array<uint32_t, 3> blocks{};
};

Content content(uint32_t i)
{
    Content c;
    const uint32_t size = 3000 + (i * 7919) % 200000;
    c.blocks = {size / 3, size / 5, size - size / 3 - size / 5};
    c.bytes.resize(size);
    for (size_t k = 0; k < size; ++k)
        c.bytes[k] = static_cast<std::byte>((i * 131 + k * 7) & 0xFF);
    return c;
}

std::unique_ptr<CacheOwner> open_owner(const fs::path& dir, uint32_t total_slots,
                                       uint64_t checkpoint_bytes = uint64_t{64} << 20)
{
    auto o = CacheOwner::start(CacheOwnerConfig{.dir = dir,
                                                .total_slots = total_slots,
                                                .layout = kLayout,
                                                .checkpoint_journal_bytes = checkpoint_bytes});
    SW_CHECK(o.has_value());
    return std::move(*o);
}

Result<size_t, CacheErrorCode> put(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    auto r = CacheRequest::put(piece(i), c.bytes, c.blocks);
    return o.call(r);
}

// Lê a peça i com os blocos do "manifesto" e confere o conteúdo.
Result<size_t, CacheErrorCode> get_checked(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    std::vector<std::byte> out(kLayout.slot_size);
    auto r = CacheRequest::get(piece(i), out, c.blocks);
    auto res = o.call(r);
    if (res.has_value()) {
        SW_CHECK(*res == c.bytes.size());
        SW_CHECK(std::memcmp(out.data(), c.bytes.data(), c.bytes.size()) == 0);
    }
    return res;
}

bool present(CacheOwner& o, uint32_t i)
{
    auto r = get_checked(o, i);
    SW_CHECK(r.has_value() || r.error() == CacheErrorCode::PieceNotFound);
    return r.has_value();
}

Result<size_t, CacheErrorCode> remove(CacheOwner& o, uint32_t i)
{
    auto r = CacheRequest::remove(piece(i));
    return o.call(r);
}

void flip_byte(const fs::path& file, uint64_t offset)
{
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x40);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&c, 1);
    SW_CHECK(f.good());
}

// ---------------------------------------------------------------- testes

void test_persists_across_restart()
{
    std::cout
        << "[Test] recuperação: Put/Remove confirmados voltam ao reabrir (checkpoint e journal)\n";
    const fs::path dir = fresh_dir("persist");
    {
        auto o = open_owner(dir, 8);
        SW_CHECK(o->recovery().journal_created && !o->recovery().index_loaded);
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(put(*o, i).has_value());
        SW_CHECK(remove(*o, 2).has_value());
        SW_CHECK(o->stop().has_value()); // fecha com checkpoint
    }
    {
        auto o = open_owner(dir, 8);
        const CacheRecovery& rec = o->recovery();
        SW_CHECK(rec.index_loaded && rec.journal_records_applied == 0 && rec.pieces == 5);
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(present(*o, i) == (i != 2));
        // Sem o checkpoint final: a próxima abertura relê o journal.
        SW_CHECK(put(*o, 6).has_value() && remove(*o, 0).has_value() && put(*o, 2).has_value());
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o);
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 8);
        const CacheRecovery& rec = o->recovery();
        std::cout << "    reabrir sem checkpoint: " << rec.journal_records_applied
                  << " registros aplicados do journal\n";
        SW_CHECK(rec.index_loaded && rec.journal_records_applied == 3 && rec.pieces == 6);
        for (uint32_t i = 0; i < 7; ++i)
            SW_CHECK(present(*o, i) == (i != 0));
    }
    cleanup(dir);
}

void test_many_checkpoints()
{
    std::cout << "[Test] recuperação: checkpoints frequentes (journal de 2 KiB) não perdem nada\n";
    const fs::path dir = fresh_dir("checkpoints");
    std::vector<bool> expected(12, false);
    for (int round = 0; round < 4; ++round) {
        auto o = open_owner(dir, 12, 2048);
        for (uint32_t i = 0; i < 12; ++i)
            SW_CHECK(present(*o, i) == expected[i]);
        for (uint32_t i = static_cast<uint32_t>(round); i < 12; i += 2) {
            if (expected[i]) {
                SW_CHECK(remove(*o, i).has_value());
            } else {
                SW_CHECK(put(*o, i).has_value());
            }
            expected[i] = !expected[i];
        }
        if (round % 2 == 1) CacheOwnerTestHook::skip_checkpoint_on_stop(*o);
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 12, 2048);
        for (uint32_t i = 0; i < 12; ++i)
            SW_CHECK(present(*o, i) == expected[i]);
    } // fecha antes de apagar a pasta (o Windows trava arquivo aberto)
    cleanup(dir);
}

void test_corrupted_block_is_never_served()
{
    std::cout << "[Test] recuperação: byte trocado no store dá CorruptedChecksum e a peça sai\n";
    const fs::path dir = fresh_dir("corrupt");
    {
        auto o = open_owner(dir, 4);
        SW_CHECK(put(*o, 1).has_value() && put(*o, 3).has_value());
        SW_CHECK(o->stop().has_value());
    }
    // A peça 3 está no slot 1 (o menor livre depois do 0). Troca um byte do 3º bloco dela.
    const Content c3 = content(3);
    flip_byte(StoreFiles::file_path(dir, 0), kLayout.slot_size + c3.blocks[0] + c3.blocks[1] + 5);
    {
        auto o = open_owner(dir, 4);
        auto r = get_checked(*o, 3);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::CorruptedChecksum);
        r = get_checked(*o, 3);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound); // saiu do índice
        SW_CHECK(present(*o, 1));
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o); // o FREE_SLOT tem de estar no journal
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 4);
        SW_CHECK(!present(*o, 3) && present(*o, 1));
        SW_CHECK(put(*o, 3).has_value() && present(*o, 3)); // baixada de novo
    }
    cleanup(dir);
}

void test_block_list_rules()
{
    std::cout << "[Test] recuperação: lista de blocos errada é recusada (Put e Get)\n";
    const fs::path dir = fresh_dir("blocks");
    {
        auto o = open_owner(dir, 4);
        const Content c = content(5);
        const std::array<uint32_t, 2> short_sum{100, 200};
        auto bad_sum = CacheRequest::put(piece(5), c.bytes, short_sum);
        auto r = o->call(bad_sum);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidSize);
        const std::array<uint32_t, 3> zero_block{0, c.blocks[0] + c.blocks[1], c.blocks[2]};
        auto bad_zero = CacheRequest::put(piece(5), c.bytes, zero_block);
        r = o->call(bad_zero);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidSize);
        SW_CHECK(put(*o, 5).has_value());
        std::vector<std::byte> out(kLayout.slot_size);
        auto no_list = CacheRequest::get(piece(5), out); // 3 blocos e nenhuma lista
        r = o->call(no_list);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidOffset);
        const std::array<uint32_t, 2> two{c.blocks[0], c.blocks[1] + c.blocks[2]};
        auto wrong = CacheRequest::get(piece(5), out, two);
        r = o->call(wrong);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidOffset);
        SW_CHECK(present(*o, 5)); // nada disso estragou a peça
    }
    cleanup(dir);
}

void test_missing_store_file_loses_pieces()
{
    std::cout << "[Test] recuperação: store-XXX.bin ausente vira peças perdidas, sem erro\n";
    const fs::path dir = fresh_dir("missing");
    {
        auto o = open_owner(dir, 8);
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(put(*o, i).has_value()); // slots 0–5: 2 arquivos
        SW_CHECK(o->stop().has_value());
    }
    std::error_code ec;
    SW_CHECK(fs::remove(StoreFiles::file_path(dir, 1), ec) && !ec);
    {
        auto o = open_owner(dir, 8);
        SW_CHECK(o->recovery().dropped_missing_file == 2 && o->recovery().pieces == 4);
        for (uint32_t i = 0; i < 6; ++i)
            SW_CHECK(present(*o, i) == (i < 4));
        SW_CHECK(put(*o, 4).has_value() && present(*o, 4)); // recria o arquivo
    }
    cleanup(dir);
}

void test_smaller_ceiling()
{
    std::cout << "[Test] recuperação: teto menor ao reabrir não dá IoError (F1-10, item 6)\n";
    const fs::path dir = fresh_dir("ceiling");
    {
        auto o = open_owner(dir, 8);
        for (uint32_t i = 0; i < 8; ++i)
            SW_CHECK(put(*o, i).has_value());
        SW_CHECK(o->stop().has_value());
    }
    SW_CHECK(fs::file_size(StoreFiles::file_path(dir, 1)) == uint64_t{4} * kLayout.slot_size);
    {
        auto o = open_owner(dir, 6); // o arquivo 1 passa a ter 2 slots
        SW_CHECK(o->recovery().dropped_ceiling == 2 && o->recovery().pieces == 6);
        SW_CHECK(fs::file_size(StoreFiles::file_path(dir, 1)) == uint64_t{2} * kLayout.slot_size);
        for (uint32_t i = 0; i < 8; ++i)
            SW_CHECK(present(*o, i) == (i < 6));
        auto r = put(*o, 7);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::OutOfSlots);
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 3); // o arquivo 1 inteiro sai
        SW_CHECK(o->recovery().dropped_ceiling == 3 && o->recovery().pieces == 3);
        SW_CHECK(!fs::exists(StoreFiles::file_path(dir, 1)));
        for (uint32_t i = 0; i < 8; ++i)
            SW_CHECK(present(*o, i) == (i < 3));
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 8); // e o teto pode voltar a crescer
        SW_CHECK(o->recovery().pieces == 3);
        SW_CHECK(put(*o, 6).has_value() && present(*o, 6));
    }
    cleanup(dir);
}

void test_sync_failure_breaks_until_reopen()
{
    std::cout << "[Test] recuperação: falha de sync quebra o cache; reabrir recupera\n";
    const fs::path dir = fresh_dir("broken");
    {
        auto o = open_owner(dir, 4);
        SW_CHECK(put(*o, 1).has_value());
        CacheOwnerTestHook::fail_next_store_sync(*o);
        auto r = put(*o, 2);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::IoError && o->broken());
        auto g = get_checked(*o, 1);
        SW_CHECK(!g.has_value() && g.error() == CacheErrorCode::IoError); // nada até reabrir
        r = put(*o, 3);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::IoError);
        SW_CHECK(o->stop().has_value()); // quebrado: fecha sem checkpoint
    }
    {
        auto o = open_owner(dir, 4);
        SW_CHECK(!o->broken());
        SW_CHECK(present(*o, 1) && !present(*o, 2) && !present(*o, 3));
        SW_CHECK(put(*o, 2).has_value() && present(*o, 2));
    }
    cleanup(dir);
}

void test_damaged_metadata_never_serves_bad_data()
{
    std::cout << "[Test] recuperação: index.bin ou cabeçalho do journal estragados\n";
    const fs::path dir = fresh_dir("metadata");
    {
        auto o = open_owner(dir, 8);
        for (uint32_t i = 0; i < 3; ++i)
            SW_CHECK(put(*o, i).has_value());
        SW_CHECK(o->stop().has_value()); // índice com 0–2
    }
    {
        auto o = open_owner(dir, 8);
        SW_CHECK(put(*o, 3).has_value() && put(*o, 4).has_value());
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o); // 3 e 4 só no journal
        SW_CHECK(o->stop().has_value());
    }
    const auto backup = dir / "index.bak";
    fs::copy_file(dir / "index.bin", backup);
    flip_byte(dir / "index.bin", 70);
    {
        // Índice descartado: só o journal (registros depois do checkpoint) sobra.
        auto o = open_owner(dir, 8);
        SW_CHECK(o->recovery().index_invalid && o->recovery().pieces == 2);
        for (uint32_t i = 0; i < 5; ++i)
            SW_CHECK(present(*o, i) == (i >= 3));
        SW_CHECK(o->stop().has_value());
    }
    cleanup(dir);
    // Cabeçalho do journal estragado: vale só o índice.
    const fs::path dir2 = fresh_dir("journalhdr");
    {
        auto o = open_owner(dir2, 8);
        SW_CHECK(put(*o, 0).has_value());
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir2, 8);
        SW_CHECK(put(*o, 1).has_value());
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o);
        SW_CHECK(o->stop().has_value());
    }
    flip_byte(dir2 / "journal.bin", 3);
    {
        auto o = open_owner(dir2, 8);
        SW_CHECK(!o->recovery().journal_header_ok && o->recovery().pieces == 1);
        SW_CHECK(present(*o, 0) && !present(*o, 1));
    }
    cleanup(dir2);
}

} // namespace

} // namespace swarmstream

int main()
{
    std::cout << "--- Recuperação do cache (F1-11) ---\n";
    swarmstream::test_persists_across_restart();
    swarmstream::test_many_checkpoints();
    swarmstream::test_corrupted_block_is_never_served();
    swarmstream::test_block_list_rules();
    swarmstream::test_missing_store_file_loses_pieces();
    swarmstream::test_smaller_ceiling();
    swarmstream::test_sync_failure_breaks_until_reopen();
    swarmstream::test_damaged_metadata_never_serves_bad_data();
    std::cout << "-> Recovery tests passed.\n";
    return 0;
}
