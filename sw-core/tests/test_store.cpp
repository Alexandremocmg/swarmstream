// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-10: alocador de slots e arquivos de store pré-alocados (docs/04 §1, ADR-0005).
//
// Critério de aceite: alocar/liberar 1 milhão de slots sem fragmentar. Como os slots têm
// tamanho fixo, "sem fragmentar" é verificado assim:
//   1. qualquer slot livre serve: allocate() só falha quando used() == capacity();
//   2. a política é sempre o menor slot livre, então, depois de qualquer sequência de
//      alocações e liberações, os slots usados são exatamente os que um modelo de referência
//      (std::set) diz, e high_water() nunca cresce além do necessário;
//   3. a memória do alocador é fixa (não cresce com o uso).

#include "check.hpp"
#include "store_files_testing.hpp"
#include "swarmstream/slot_allocator.hpp"
#include "swarmstream/store_files.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace swarmstream {

namespace {

namespace fs = std::filesystem;

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_10_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Apaga a pasta do teste sem lançar exceção (o store precisa estar fechado antes).
void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    SW_CHECK(!ec);
}

std::vector<std::byte> pattern(size_t n, uint8_t seed)
{
    std::vector<std::byte> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>(static_cast<uint8_t>(seed + i * 31));
    }
    return v;
}

// ------------------------------------------------------------------ SlotAllocator

void test_allocator_basics()
{
    std::cout << "[Test] SlotAllocator: básico\n";
    SlotAllocator a(10);
    SW_CHECK(a.capacity() == 10 && a.used() == 0 && a.high_water() == 0);
    for (SlotId expected = 0; expected < 10; ++expected) {
        auto s = a.allocate();
        SW_CHECK(s.has_value() && *s == expected); // sempre o menor livre
    }
    SW_CHECK(a.used() == 10 && a.high_water() == 10);
    auto full = a.allocate();
    SW_CHECK(!full.has_value() && full.error() == CacheErrorCode::OutOfSlots);

    SW_CHECK(a.release(3).has_value() && a.release(7).has_value());
    SW_CHECK(a.used() == 8 && !a.is_used(3) && !a.is_used(7));
    auto r1 = a.allocate();
    auto r2 = a.allocate();
    SW_CHECK(r1.has_value() && *r1 == 3 && r2.has_value() && *r2 == 7); // reaproveita os buracos

    auto twice = a.release(5);
    SW_CHECK(twice.has_value());
    auto again = a.release(5);
    SW_CHECK(!again.has_value() && again.error() == CacheErrorCode::SlotNotFound); // já livre
    auto out = a.release(10);
    SW_CHECK(!out.has_value() && out.error() == CacheErrorCode::SlotNotFound); // fora do intervalo

    // high_water cai quando o slot mais alto sai.
    SlotAllocator b(100);
    for (int i = 0; i < 5; ++i) {
        SW_CHECK(b.allocate().has_value());
    }
    SW_CHECK(b.release(4).has_value() && b.high_water() == 4);
    SW_CHECK(b.release(2).has_value() && b.high_water() == 4);
    SW_CHECK(b.release(3).has_value() && b.high_water() == 2);
}

void test_allocator_mark_used()
{
    std::cout << "[Test] SlotAllocator: mark_used (recuperação da F1-11)\n";
    SlotAllocator a(200);
    SW_CHECK(a.mark_used(150).has_value() && a.mark_used(0).has_value() &&
             a.mark_used(64).has_value());
    SW_CHECK(a.used() == 3 && a.high_water() == 151);
    auto dup = a.mark_used(64);
    SW_CHECK(!dup.has_value() && dup.error() == CacheErrorCode::SlotBusy);
    auto out = a.mark_used(200);
    SW_CHECK(!out.has_value() && out.error() == CacheErrorCode::SlotNotFound);
    auto s = a.allocate();
    SW_CHECK(s.has_value() && *s == 1); // pula os marcados
}

void test_allocator_capacity_edges()
{
    std::cout << "[Test] SlotAllocator: capacidades nas bordas da palavra de 64 bits\n";
    for (uint32_t cap : {1u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 262145u}) {
        SlotAllocator a(cap);
        for (uint32_t i = 0; i < cap; ++i) {
            auto s = a.allocate();
            SW_CHECK(s.has_value() && *s == i);
        }
        SW_CHECK(!a.allocate().has_value());
        SW_CHECK(a.release(cap - 1).has_value());
        auto last = a.allocate();
        SW_CHECK(last.has_value() && *last == cap - 1);
    }
    SlotAllocator empty(0);
    SW_CHECK(!empty.allocate().has_value() && empty.high_water() == 0);
}

// Critério de aceite: 1 milhão de slots, contra um modelo de referência.
void test_allocator_one_million_no_fragmentation()
{
    std::cout << "[Test] SlotAllocator: 1 milhão de slots sem fragmentar\n";
    constexpr uint32_t kSlots = 1'000'000;
    SlotAllocator a(kSlots);
    const size_t bytes_before = a.memory_bytes();

    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kSlots; ++i) {
        auto s = a.allocate();
        SW_CHECK(s.has_value() && *s == i);
    }
    SW_CHECK(a.used() == kSlots && !a.allocate().has_value());

    // Libera metade, ao acaso, e confere que a realocação preenche exatamente os buracos,
    // do menor para o maior (o modelo é o conjunto ordenado dos livres).
    std::mt19937_64 rng(20261020);
    std::vector<SlotId> order(kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) {
        order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), rng);
    std::set<SlotId> free_model;
    for (uint32_t i = 0; i < kSlots / 2; ++i) {
        SW_CHECK(a.release(order[i]).has_value());
        free_model.insert(order[i]);
    }
    SW_CHECK(a.used() == kSlots / 2);
    while (!free_model.empty()) {
        auto s = a.allocate();
        SW_CHECK(s.has_value() && *s == *free_model.begin());
        free_model.erase(free_model.begin());
    }
    SW_CHECK(a.used() == kSlots && a.high_water() == kSlots);

    // 4 milhões de operações misturadas contra o modelo dos usados.
    std::set<SlotId> used_model;
    for (uint32_t i = 0; i < kSlots; ++i) {
        SW_CHECK(a.release(i).has_value());
    }
    SW_CHECK(a.used() == 0 && a.high_water() == 0);
    std::vector<SlotId> used_list; // para escolher um usado ao acaso em O(1)
    for (int op = 0; op < 4'000'000; ++op) {
        const bool do_alloc =
            used_list.empty() || (used_list.size() < kSlots && (rng() % 100) < 55);
        if (do_alloc) {
            auto s = a.allocate();
            SW_CHECK(s.has_value());
            if (op % 4096 == 0) { // achar o "menor livre" no modelo é O(n): amostrado
                SlotId expected = 0;
                auto it = used_model.begin();
                while (it != used_model.end() && *it == expected) {
                    ++expected;
                    ++it;
                }
                SW_CHECK(*s == expected);
            }
            SW_CHECK(!used_model.count(*s));
            used_model.insert(*s);
            used_list.push_back(*s);
        } else {
            const size_t k = static_cast<size_t>(rng() % used_list.size());
            const SlotId victim = used_list[k];
            used_list[k] = used_list.back();
            used_list.pop_back();
            SW_CHECK(a.release(victim).has_value());
            used_model.erase(victim);
        }
        if (op % 100'000 == 0) {
            SW_CHECK(a.used() == used_model.size());
            const SlotId hw = used_model.empty() ? 0 : *used_model.rbegin() + 1;
            SW_CHECK(a.high_water() == hw);
        }
    }
    SW_CHECK(a.used() == used_model.size());
    for (SlotId s : used_model) {
        SW_CHECK(a.is_used(s));
    }
    // Sem fragmentação: com N usados e a política do menor livre, a parte usada dos arquivos
    // é o que o modelo diz, e a memória do alocador não cresceu.
    SW_CHECK(a.memory_bytes() == bytes_before);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::cout << "    1M alocações + 500k liberações + 500k realocações + 4M operações: " << ms
              << " ms; memória do alocador: " << bytes_before << " bytes\n";
}

// ------------------------------------------------------------------ StoreFiles

constexpr uint32_t kSlotSize = 1u << 20;

void test_store_files_create_preallocated()
{
    std::cout << "[Test] StoreFiles: arquivos criados sob demanda e pré-alocados\n";
    const fs::path dir = fresh_dir("prealloc");
    StoreLayout layout{.slots_per_file = 4, .slot_size = kSlotSize};
    {
        auto st = StoreFiles::open(dir, 10, layout); // 10 slots => 3 arquivos (4 + 4 + 2)
        SW_CHECK(st.has_value());
        StoreFiles& store = *st;
        SW_CHECK(store.file_count() == 3 && store.files_created() == 0);
        SW_CHECK(!fs::exists(dir / "store-000.bin")); // nada criado antes de precisar

        SW_CHECK(store.ensure_slot(5).has_value()); // slot 5 fica no arquivo 1
        SW_CHECK(fs::exists(dir / "store-001.bin") && !fs::exists(dir / "store-000.bin"));
        SW_CHECK(fs::file_size(dir / "store-001.bin") == uint64_t{4} * kSlotSize);
        auto alloc = StoreFiles::allocated_bytes(dir / "store-001.bin");
        SW_CHECK(alloc.has_value() && *alloc >= uint64_t{4} * kSlotSize); // não é arquivo esparso
        std::cout << "    store-001.bin: tamanho " << fs::file_size(dir / "store-001.bin")
                  << ", espaço reservado no disco " << *alloc << "\n";

        SW_CHECK(store.ensure_slot(9).has_value()); // último arquivo tem só 2 slots
        SW_CHECK(fs::file_size(dir / "store-002.bin") == uint64_t{2} * kSlotSize);
        auto bad = store.ensure_slot(10);
        SW_CHECK(!bad.has_value() && bad.error() == CacheErrorCode::SlotNotFound);
        SW_CHECK(store.files_created() == 2);
    } // fecha o store antes de apagar a pasta (o Windows trava arquivo aberto)
    cleanup(dir);
}

void test_store_files_write_read_flush()
{
    std::cout << "[Test] StoreFiles: escrita, leitura e flush posicionais\n";
    const fs::path dir = fresh_dir("io");
    StoreLayout layout{.slots_per_file = 3, .slot_size = kSlotSize};
    {
        auto st = StoreFiles::open(dir, 6, layout);
        SW_CHECK(st.has_value());
        StoreFiles& store = *st;

        const auto full = pattern(kSlotSize, 7);       // peça de 1 MiB inteira
        const auto small = pattern(12345, 99);         // peça curta
        SW_CHECK(store.write(4, 0, full).has_value()); // cria o arquivo 1 sob demanda
        SW_CHECK(store.write(0, 0, small).has_value());
        SW_CHECK(store.write(2, kSlotSize - 10, std::span(full).subspan(0, 10)).has_value());
        SW_CHECK(store.flush().has_value());

        std::vector<std::byte> back(kSlotSize);
        SW_CHECK(store.read(4, 0, back).has_value() && back == full);
        std::vector<std::byte> back_small(small.size());
        SW_CHECK(store.read(0, 0, back_small).has_value() && back_small == small);
        std::vector<std::byte> tail(10);
        SW_CHECK(store.read(2, kSlotSize - 10, tail).has_value() &&
                 std::memcmp(tail.data(), full.data(), 10) == 0);

        // Slots vizinhos não se sobrepõem: o slot 3 (início do arquivo 1) continua zerado.
        std::vector<std::byte> neighbour(4096);
        SW_CHECK(store.read(3, 0, neighbour).has_value());
        SW_CHECK(std::all_of(neighbour.begin(), neighbour.end(),
                             [](std::byte b) { return b == std::byte{0}; }));

        // Limites: nada passa do slot.
        auto over = store.write(1, kSlotSize - 5, std::span(small).subspan(0, 6));
        SW_CHECK(!over.has_value() && over.error() == CacheErrorCode::InvalidOffset);
        auto over_read = store.read(1, kSlotSize, std::span(back).subspan(0, 1));
        SW_CHECK(!over_read.has_value() && over_read.error() == CacheErrorCode::InvalidOffset);
        auto no_slot = store.write(6, 0, small);
        SW_CHECK(!no_slot.has_value() && no_slot.error() == CacheErrorCode::SlotNotFound);
    } // fecha o store antes de apagar a pasta (o Windows trava arquivo aberto)
    cleanup(dir);
}

void test_store_files_reopen_keeps_data()
{
    std::cout << "[Test] StoreFiles: reabrir não apaga nem trunca os dados\n";
    const fs::path dir = fresh_dir("reopen");
    StoreLayout layout{.slots_per_file = 2, .slot_size = kSlotSize};
    const auto data = pattern(70000, 3);
    {
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        SW_CHECK(st->write(3, 100, data).has_value() && st->flush().has_value());
    }
    {
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        std::vector<std::byte> back(data.size());
        SW_CHECK(st->read(3, 100, back).has_value() && back == data);
        SW_CHECK(fs::file_size(dir / "store-001.bin") == uint64_t{2} * kSlotSize);
    }
    // Arquivo maior que o layout esperado: recusa (não trunca dados de outro layout).
    {
        std::error_code ec;
        fs::resize_file(dir / "store-001.bin", uint64_t{3} * kSlotSize, ec);
        SW_CHECK(!ec);
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        auto r = st->ensure_slot(2);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::IoError);
        SW_CHECK(fs::file_size(dir / "store-001.bin") == uint64_t{3} * kSlotSize);
    }
    cleanup(dir);
}

void test_store_files_invalid_layout()
{
    std::cout << "[Test] StoreFiles: layout inválido é recusado\n";
    const fs::path dir = fresh_dir("layout");
    auto zero = StoreFiles::open(dir, 4, StoreLayout{.slots_per_file = 0, .slot_size = kSlotSize});
    SW_CHECK(!zero.has_value() && zero.error() == CacheErrorCode::InvalidSize);
    auto unaligned = StoreFiles::open(dir, 4, StoreLayout{.slots_per_file = 2, .slot_size = 1000});
    SW_CHECK(!unaligned.has_value() && unaligned.error() == CacheErrorCode::InvalidSize); // 4 KiB
    auto no_dir = StoreFiles::open(dir / "nao" / "existe", 4, StoreLayout{});
    SW_CHECK(!no_dir.has_value() && no_dir.error() == CacheErrorCode::IoError);
    cleanup(dir);
}

void test_store_files_disk_full()
{
    std::cout << "[Test] StoreFiles: disco cheio dá DiskFull na criação, sem gastar espaço\n";
    const fs::path dir = fresh_dir("diskfull");
    {
        std::error_code ec;
        const auto before = fs::space(dir, ec);
        SW_CHECK(!ec);
        // Um arquivo só, com o dobro do espaço livre (slots de 1 GiB): não cabe de jeito nenhum.
        // O livre é conferido antes de reservar, então nem a emulação da posix_fallocate (que
        // escreveria zeros) chega a rodar.
        constexpr uint32_t kHugeSlot = 1u << 30;
        const uint64_t wanted_slots = before.available / kHugeSlot * 2 + 2;
        SW_CHECK(wanted_slots < UINT32_MAX);
        const auto slots = static_cast<uint32_t>(wanted_slots);
        auto st = StoreFiles::open(dir, slots,
                                   StoreLayout{.slots_per_file = slots, .slot_size = kHugeSlot});
        SW_CHECK(st.has_value());

        auto r = st->ensure_slot(0);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::DiskFull);
        SW_CHECK(!fs::exists(dir / "store-000.bin")); // não deixa arquivo vazio para trás
        SW_CHECK(st->files_created() == 0 && !st->broken());
        auto w = st->write(slots - 1, 0, pattern(16, 1)); // a escrita cria sob demanda: idem
        SW_CHECK(!w.has_value() && w.error() == CacheErrorCode::DiskFull);

        const auto after = fs::space(dir, ec);
        SW_CHECK(!ec);
        const uint64_t moved = before.available > after.available
                                   ? before.available - after.available
                                   : after.available - before.available;
        std::cout << "    pedido " << (uint64_t{slots} * kHugeSlot >> 20) << " MiB com "
                  << (before.available >> 20) << " MiB livres; o livre mudou " << (moved >> 10)
                  << " KiB\n";
        SW_CHECK(moved < (uint64_t{256} << 20)); // folga só para outros processos da máquina
        SW_CHECK(st->close().has_value());
    }
    cleanup(dir);
}

void test_store_files_close_reports()
{
    std::cout << "[Test] StoreFiles: close() confere o fechamento e encerra o store\n";
    const fs::path dir = fresh_dir("close");
    StoreLayout layout{.slots_per_file = 2, .slot_size = kSlotSize};
    {
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        const auto data = pattern(4096, 5);
        SW_CHECK(st->write(1, 0, data).has_value() && st->flush().has_value());
        SW_CHECK(st->close().has_value());
        SW_CHECK(st->close().has_value()); // de novo: nada mais a fechar

        std::vector<std::byte> back(data.size());
        auto w = st->write(1, 0, data);
        auto r = st->read(1, 0, back);
        auto f = st->flush();
        auto e = st->ensure_slot(3);
        SW_CHECK(!w.has_value() && w.error() == CacheErrorCode::IoError);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::IoError);
        SW_CHECK(!f.has_value() && f.error() == CacheErrorCode::IoError);
        SW_CHECK(!e.has_value() && e.error() == CacheErrorCode::IoError);
        SW_CHECK(!fs::exists(dir / "store-001.bin")); // fechado não recria arquivo

        // Fechou de verdade: com o objeto ainda vivo, o arquivo já pode ser apagado (o Windows
        // recusaria se o handle continuasse aberto).
        std::error_code ec;
        SW_CHECK(fs::remove(dir / "store-000.bin", ec) && !ec);
    }
    cleanup(dir);
}

void test_store_files_sync_failure_is_sticky()
{
    std::cout << "[Test] StoreFiles: falha de sync é pegajosa até reabrir\n";
    const fs::path dir = fresh_dir("sticky");
    StoreLayout layout{.slots_per_file = 2, .slot_size = kSlotSize};
    const auto data = pattern(8192, 11);
    {
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        SW_CHECK(st->write(0, 0, data).has_value());
        StoreFilesTestHook::fail_next_sync(*st);
        auto f1 = st->flush();
        SW_CHECK(!f1.has_value() && f1.error() == CacheErrorCode::IoError);
        SW_CHECK(st->broken());
        // O segundo flush NÃO pode dar sucesso (no Linux o fdatasync seguinte daria, sem os
        // dados no disco).
        auto f2 = st->flush();
        SW_CHECK(!f2.has_value() && f2.error() == CacheErrorCode::IoError);
        std::vector<std::byte> back(data.size());
        auto w = st->write(1, 0, data);
        auto r = st->read(0, 0, back);
        auto e = st->ensure_slot(3);
        SW_CHECK(!w.has_value() && w.error() == CacheErrorCode::IoError);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::IoError);
        SW_CHECK(!e.has_value() && e.error() == CacheErrorCode::IoError);
        SW_CHECK(!fs::exists(dir / "store-001.bin"));
        SW_CHECK(st->close().has_value()); // fechar continua possível
    }
    {
        // Falha de sync na criação do arquivo também quebra o store.
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value() && !st->broken());
        StoreFilesTestHook::fail_next_sync(*st);
        auto e = st->ensure_slot(2);
        SW_CHECK(!e.has_value() && e.error() == CacheErrorCode::IoError && st->broken());
        auto f = st->flush();
        SW_CHECK(!f.has_value() && f.error() == CacheErrorCode::IoError);
    }
    {
        // Reabrir dá um store novo, usável (a F1-11 refaz a recuperação e confere checksums).
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value() && !st->broken());
        std::vector<std::byte> back(data.size());
        SW_CHECK(st->read(0, 0, back).has_value());
        SW_CHECK(st->write(2, 0, data).has_value() && st->flush().has_value());
        SW_CHECK(st->close().has_value());
    }
    cleanup(dir);
}

void test_store_files_directory_sync_and_missing_file()
{
    std::cout << "[Test] StoreFiles: pasta sincronizada ao criar; arquivo sumido volta zerado\n";
    const fs::path dir = fresh_dir("dirsync");
    StoreLayout layout{.slots_per_file = 2, .slot_size = kSlotSize};
    const auto data0 = pattern(5000, 21);
    const auto data4 = pattern(5000, 42);
    {
        auto st = StoreFiles::open(dir, 6, layout);
        SW_CHECK(st.has_value());
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 0);
        SW_CHECK(st->write(0, 0, data0).has_value()); // cria store-000.bin
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 1);
        SW_CHECK(st->write(1, 0, data0).has_value()); // mesmo arquivo: nada a sincronizar
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 1);
        SW_CHECK(st->write(4, 0, data4).has_value()); // cria store-002.bin
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 2);
        SW_CHECK(st->flush().has_value() && st->close().has_value());
    }
    // O arquivo sumiu (queda de energia antes de a pasta ir ao disco, ou alguém apagou).
    std::error_code ec;
    SW_CHECK(fs::remove(dir / "store-000.bin", ec) && !ec);
    {
        auto st = StoreFiles::open(dir, 6, layout);
        SW_CHECK(st.has_value());
        std::vector<std::byte> back(data4.size());
        SW_CHECK(st->read(4, 0, back).has_value() && back == data4); // o outro arquivo está intacto
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 0); // abrir arquivo pronto não cria
        // O slot do arquivo sumido volta zerado (o arquivo é recriado e pré-alocado): o checksum
        // por bloco da F1-11 não confere e a peça é dada por perdida, sem erro fatal.
        SW_CHECK(st->read(0, 0, back).has_value());
        SW_CHECK(
            std::all_of(back.begin(), back.end(), [](std::byte b) { return b == std::byte{0}; }));
        SW_CHECK(StoreFilesTestHook::directory_syncs(*st) == 1);
        SW_CHECK(st->close().has_value());
    }
    cleanup(dir);
}

// F1-12 (pendência do F1-10): o DiskFull que vem do próprio SO, sem a conferência prévia do
// espaço livre. Provocar isso num disco de verdade o encheria; por isso só roda numa pasta de
// um sistema de arquivos pequeno e isolado, indicada por SW_TEST_SMALL_FS (o job build+test do
// Linux no CI monta um tmpfs de 16 MiB). Roda as duas reservas: fallocate(2) e o ramo da
// posix_fallocate.
void test_store_files_native_disk_full_small_fs()
{
    std::cout << "[Test] StoreFiles: DiskFull do próprio SO num sistema de arquivos pequeno\n";
    const char* small = std::getenv("SW_TEST_SMALL_FS");
    if (small == nullptr || *small == '\0') {
        // No job que monta o tmpfs, pular seria um teste que passa sem testar (revisão da R2).
        const char* required = std::getenv("SW_REQUIRE_SMALL_FS");
        SW_CHECK(required == nullptr || *required == '\0');
        std::cout << "    PULADO: SW_TEST_SMALL_FS não definida (o CI do Linux define)\n";
        return;
    }
    for (const bool native : {true, false}) {
        const fs::path dir = fs::path(small) / (native ? "f1_12_native" : "f1_12_fallback");
        fs::create_directories(dir);
        {
            std::error_code ec;
            const auto before = fs::space(dir, ec);
            SW_CHECK(!ec);
            const auto slots = static_cast<uint32_t>(before.available / kSlotSize * 2 + 2);
            auto st = StoreFiles::open(
                dir, slots, StoreLayout{.slots_per_file = slots, .slot_size = kSlotSize});
            SW_CHECK(st.has_value());
            StoreFilesTestHook::skip_space_check(*st);
            if (!native) StoreFilesTestHook::force_posix_fallocate(*st);
            auto r = st->ensure_slot(0);
            SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::DiskFull);
            SW_CHECK(!fs::exists(dir / "store-000.bin")); // reserva desfeita e arquivo apagado
            SW_CHECK(!st->broken());
            const auto after = fs::space(dir, ec);
            SW_CHECK(!ec);
            std::cout << "    " << (native ? "fallocate(2)" : "posix_fallocate") << ": pedido "
                      << (uint64_t{slots} * kSlotSize >> 10) << " KiB com "
                      << (before.available >> 10) << " KiB livres; livre depois "
                      << (after.available >> 10) << " KiB\n";
            SW_CHECK(after.available == before.available); // sistema isolado: nada pode vazar
            SW_CHECK(st->close().has_value());
        }
        cleanup(dir);
    }
}

// F1-12 (pendência do F1-10): o ramo EOPNOTSUPP -> posix_fallocate. Num sistema de arquivos
// com fallocate(2), a glibc também o usa por dentro; aqui se prova que o nosso ramo reserva o
// arquivo inteiro e que os dados vão e voltam. A emulação da glibc (escrever zeros) só
// aconteceria num sistema de arquivos sem fallocate, que o CI não tem.
void test_store_files_posix_fallocate_fallback()
{
    std::cout << "[Test] StoreFiles: ramo da posix_fallocate (fallocate(2) indisponível)\n";
#if defined(_WIN32)
    std::cout << "    PULADO: o ramo só existe no POSIX\n";
#else
    const fs::path dir = fresh_dir("fallback");
    StoreLayout layout{.slots_per_file = 4, .slot_size = kSlotSize};
    {
        auto st = StoreFiles::open(dir, 4, layout);
        SW_CHECK(st.has_value());
        StoreFilesTestHook::force_posix_fallocate(*st);
        SW_CHECK(st->ensure_slot(0).has_value());
        SW_CHECK(fs::file_size(dir / "store-000.bin") == uint64_t{4} * kSlotSize);
        auto alloc = StoreFiles::allocated_bytes(dir / "store-000.bin");
        SW_CHECK(alloc.has_value() && *alloc >= uint64_t{4} * kSlotSize); // não esparso
        const auto data = pattern(9000, 77);
        std::vector<std::byte> back(data.size());
        SW_CHECK(st->write(3, 0, data).has_value() && st->flush().has_value());
        SW_CHECK(st->read(3, 0, back).has_value() && back == data);
        SW_CHECK(st->close().has_value());
    }
    cleanup(dir);
#endif
}

} // namespace

void run_store_tests()
{
    test_allocator_basics();
    test_allocator_mark_used();
    test_allocator_capacity_edges();
    test_allocator_one_million_no_fragmentation();
    test_store_files_create_preallocated();
    test_store_files_write_read_flush();
    test_store_files_reopen_keeps_data();
    test_store_files_invalid_layout();
    test_store_files_disk_full();
    test_store_files_close_reports();
    test_store_files_sync_failure_is_sticky();
    test_store_files_directory_sync_and_missing_file();
    test_store_files_native_disk_full_small_fs();
    test_store_files_posix_fallocate_fallback();
}

} // namespace swarmstream

int main()
{
    std::cout << "--- Store Tests (F1-10) ---\n";
    swarmstream::run_store_tests();
    std::cout << "-> Store tests passed.\n";
    return 0;
}
