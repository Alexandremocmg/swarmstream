// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-13: leases com contagem de referência na CacheOwner (ADR-0005, docs/04 §Concorrência).
//
// Critério de aceite: peça com lease nunca é removida (teste de estresse). O estresse
// (test_lease_stress) põe 8 threads que prendem peças e as leem enquanto presas contra 2
// removedoras que tiram peças ao acaso e as devolvem com conteúdo NOVO (outra geração). Se uma
// peça presa saísse do cache, a leitura sob o lease veria falta ou a geração mudar; se o slot
// fosse reaproveitado, um segundo Acquire daria outro slot. O job TSan do CI roda este
// executável (soltar o lease é um atômico fora da dona). Os outros testes fixam as regras uma
// a uma: Remove com lease dá SlotBusy, peça estragada com lease não libera o slot até o último
// lease sair, lease que sobrevive à CacheOwner e Acquire/soltar sem alocar.

#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Ver test_cache_owner.cpp: no job TSan este executável tem de estar instrumentado.
#if defined(SW_EXPECT_TSAN)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SW_TSAN_ON 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SW_TSAN_ON 1
#endif
#if !defined(SW_TSAN_ON)
#error "SANITIZE_THREAD ligado, mas este executável não foi compilado com -fsanitize=thread"
#endif
#endif

// ---------------------------------------------------------------- contagem de alocações
// Como em test_cache_owner.cpp: prova que Acquire e soltar o lease não alocam.
namespace {
std::atomic<bool> g_counting{false};
std::atomic<uint64_t> g_allocations{0};
void* volatile g_sink = nullptr;
} // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace swarmstream {

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr StoreLayout kLayout{.slots_per_file = 8, .slot_size = 64 * 1024};

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_13_") + name + "_" +
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
    for (size_t b = 0; b < h.bytes.size(); ++b) {
        h.bytes[b] = static_cast<uint8_t>(i * 7 + b);
    }
    return PieceId(i, h);
}

// Conteúdo da geração `gen` da peça: os 4 primeiros bytes são a geração; o resto depende da
// peça e da geração. Quem lê descobre a geração e confere o resto.
void fill(std::vector<std::byte>& v, uint32_t piece_index, uint32_t gen)
{
    std::memcpy(v.data(), &gen, sizeof gen);
    for (size_t i = sizeof gen; i < v.size(); ++i) {
        v[i] = static_cast<std::byte>(static_cast<uint8_t>(piece_index * 131 + gen * 17 + i));
    }
}

uint32_t generation(std::span<const std::byte> v)
{
    uint32_t gen = 0;
    std::memcpy(&gen, v.data(), sizeof gen);
    return gen;
}

bool matches(std::span<const std::byte> v, uint32_t piece_index, uint32_t gen)
{
    if (v.size() < sizeof gen || generation(v) != gen) return false;
    for (size_t i = sizeof gen; i < v.size(); ++i) {
        if (v[i] != static_cast<std::byte>(static_cast<uint8_t>(piece_index * 131 + gen * 17 + i)))
            return false;
    }
    return true;
}

std::unique_ptr<CacheOwner> open_owner(const fs::path& dir, uint32_t total_slots)
{
    auto started = CacheOwner::start(
        CacheOwnerConfig{.dir = dir, .total_slots = total_slots, .layout = kLayout});
    SW_CHECK(started.has_value());
    return std::move(*started);
}

void put(CacheOwner& owner, uint32_t i, uint32_t gen, size_t bytes = 4096)
{
    std::vector<std::byte> data(bytes);
    fill(data, i, gen);
    auto req = CacheRequest::put(piece(i), data);
    auto r = owner.call(req);
    SW_CHECK(r.has_value() && *r == bytes);
}

Result<size_t, CacheErrorCode> acquire(CacheOwner& owner, uint32_t i, PieceLease& lease)
{
    auto req = CacheRequest::acquire(piece(i), lease);
    return owner.call(req);
}

Result<size_t, CacheErrorCode> remove(CacheOwner& owner, uint32_t i)
{
    auto req = CacheRequest::remove(piece(i));
    return owner.call(req);
}

Result<size_t, CacheErrorCode> get(CacheOwner& owner, uint32_t i, std::vector<std::byte>& out)
{
    auto req = CacheRequest::get(piece(i), out);
    return owner.call(req);
}

bool has_error(const Result<size_t, CacheErrorCode>& r, CacheErrorCode code)
{
    return !r.has_value() && r.error() == code;
}

void test_lease_blocks_remove()
{
    std::cout << "[Test] lease: Remove de peça presa dá SlotBusy; solta o último, remove\n";
    const fs::path dir = fresh_dir("blocks");
    {
        auto owner = open_owner(dir, 4);
        put(*owner, 1, 7, 5000);
        std::vector<std::byte> out(kLayout.slot_size);

        PieceLease missing;
        SW_CHECK(has_error(acquire(*owner, 2, missing), CacheErrorCode::PieceNotFound));
        SW_CHECK(!missing.valid() && missing.slot() == kInvalidSlot && owner->leases() == 0);
        CacheRequest no_dest(CacheOp::Acquire, piece(1), {}, {}); // sem PieceLease
        SW_CHECK(has_error(owner->call(no_dest), CacheErrorCode::InvalidSize));
        SW_CHECK(owner->leases() == 0);

        PieceLease a, b;
        auto r = acquire(*owner, 1, a);
        SW_CHECK(r.has_value() && *r == 5000);
        SW_CHECK(a.valid() && a.size() == 5000 && a.piece() == piece(1).hash);
        SW_CHECK(acquire(*owner, 1, b).has_value());
        SW_CHECK(b.slot() == a.slot() && owner->leases() == 2);

        // Erro no Acquire não mexe no lease que já estava no destino.
        SW_CHECK(has_error(acquire(*owner, 2, b), CacheErrorCode::PieceNotFound));
        SW_CHECK(b.valid() && b.slot() == a.slot() && owner->leases() == 2);

        SW_CHECK(has_error(remove(*owner, 1), CacheErrorCode::SlotBusy));
        r = get(*owner, 1, out); // continua no cache e legível
        SW_CHECK(r.has_value() && matches(std::span(out).first(*r), 1, 7));
        SW_CHECK(owner->pieces() == 1);

        a.reset();
        SW_CHECK(!a.valid() && owner->leases() == 1);
        a.reset(); // soltar de novo não faz nada
        SW_CHECK(owner->leases() == 1);
        SW_CHECK(has_error(remove(*owner, 1), CacheErrorCode::SlotBusy)); // `b` ainda prende

        b.reset();
        SW_CHECK(owner->leases() == 0);
        SW_CHECK(remove(*owner, 1).has_value());
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::PieceNotFound));
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

void test_lease_move_and_replace()
{
    std::cout << "[Test] lease: mover, atribuir por cima e Acquire num destino ocupado\n";
    const fs::path dir = fresh_dir("move");
    {
        auto owner = open_owner(dir, 4);
        put(*owner, 1, 0);
        put(*owner, 2, 0);

        PieceLease a;
        SW_CHECK(acquire(*owner, 1, a).has_value());
        PieceLease moved(std::move(a));
        // NOLINTNEXTLINE(bugprone-use-after-move): o estado de um lease movido é parte do contrato
        SW_CHECK(!a.valid() && moved.valid() && owner->leases() == 1);
        SW_CHECK(moved.piece() == piece(1).hash);

        PieceLease other;
        SW_CHECK(acquire(*owner, 2, other).has_value());
        SW_CHECK(owner->leases() == 2);
        moved = std::move(other); // solta o lease da peça 1 e fica com o da 2
        // NOLINTNEXTLINE(bugprone-use-after-move): idem
        SW_CHECK(owner->leases() == 1 && moved.piece() == piece(2).hash && !other.valid());
        SW_CHECK(remove(*owner, 1).has_value());
        SW_CHECK(has_error(remove(*owner, 2), CacheErrorCode::SlotBusy));

        // Acquire num destino que já tem lease: troca o antigo pelo novo, sem vazar.
        SW_CHECK(acquire(*owner, 2, moved).has_value());
        SW_CHECK(owner->leases() == 1);
        put(*owner, 1, 0);
        SW_CHECK(acquire(*owner, 1, moved).has_value()); // troca a peça 2 pela 1
        SW_CHECK(owner->leases() == 1 && moved.piece() == piece(1).hash);
        SW_CHECK(remove(*owner, 2).has_value());
        SW_CHECK(has_error(remove(*owner, 1), CacheErrorCode::SlotBusy));
        moved.reset();
        SW_CHECK(remove(*owner, 1).has_value() && owner->leases() == 0);
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

void test_lease_corrupted_piece_keeps_slot()
{
    std::cout << "[Test] lease: bloco estragado com lease não é servido e não libera o slot\n";
    const fs::path dir = fresh_dir("corrupt");
    {
        auto owner = open_owner(dir, 2);
        put(*owner, 1, 3);
        put(*owner, 2, 0);
        std::vector<std::byte> out(kLayout.slot_size);

        PieceLease lease;
        SW_CHECK(acquire(*owner, 1, lease).has_value());
        CacheOwnerTestHook::corrupt_next_read(*owner);
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::CorruptedChecksum));
        // Estragada e presa: nada sai do cache, mas o slot continua da peça.
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::CorruptedChecksum));
        PieceLease second;
        SW_CHECK(has_error(acquire(*owner, 1, second), CacheErrorCode::CorruptedChecksum));
        SW_CHECK(!second.valid());
        SW_CHECK(has_error(remove(*owner, 1), CacheErrorCode::SlotBusy));
        std::vector<std::byte> fresh(4096);
        fill(fresh, 1, 4);
        auto reput = CacheRequest::put(piece(1), fresh); // não regrava por cima do slot preso
        SW_CHECK(has_error(owner->call(reput), CacheErrorCode::SlotBusy));
        SW_CHECK(owner->pieces() == 2 && lease.valid() && owner->leases() == 1);
        // Sem slot livre: o slot da peça presa não é reaproveitado.
        std::vector<std::byte> other(4096);
        fill(other, 3, 0);
        auto put3 = CacheRequest::put(piece(3), other);
        SW_CHECK(has_error(owner->call(put3), CacheErrorCode::OutOfSlots));

        lease.reset();
        // O primeiro pedido depois do último lease libera o slot (FREE_SLOT) e diz por quê.
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::CorruptedChecksum));
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::PieceNotFound));
        SW_CHECK(owner->pieces() == 1);
        SW_CHECK(owner->call(reput).has_value()); // baixada de novo
        auto r = get(*owner, 1, out);
        SW_CHECK(r.has_value() && matches(std::span(out).first(*r), 1, 4));

        // Os outros caminhos também liberam a peça estragada depois do último lease.
        SW_CHECK(acquire(*owner, 2, lease).has_value());
        CacheOwnerTestHook::corrupt_next_read(*owner);
        SW_CHECK(has_error(get(*owner, 2, out), CacheErrorCode::CorruptedChecksum));
        lease.reset();
        SW_CHECK(has_error(acquire(*owner, 2, lease), CacheErrorCode::CorruptedChecksum));
        SW_CHECK(!lease.valid() && owner->pieces() == 1);
        SW_CHECK(has_error(get(*owner, 2, out), CacheErrorCode::PieceNotFound));
        put(*owner, 2, 1);

        SW_CHECK(acquire(*owner, 2, lease).has_value());
        CacheOwnerTestHook::corrupt_next_read(*owner);
        SW_CHECK(has_error(get(*owner, 2, out), CacheErrorCode::CorruptedChecksum));
        lease.reset();
        SW_CHECK(remove(*owner, 2).has_value()); // Remove de peça estragada sem lease
        SW_CHECK(has_error(get(*owner, 2, out), CacheErrorCode::PieceNotFound));

        SW_CHECK(acquire(*owner, 1, lease).has_value());
        CacheOwnerTestHook::corrupt_next_read(*owner);
        SW_CHECK(has_error(get(*owner, 1, out), CacheErrorCode::CorruptedChecksum));
        lease.reset();
        r = owner->call(reput); // Put da mesma peça: libera o slot estragado e grava de novo
        SW_CHECK(r.has_value() && *r == fresh.size());
        r = get(*owner, 1, out);
        SW_CHECK(r.has_value() && matches(std::span(out).first(*r), 1, 4));
        SW_CHECK(owner->leases() == 0);
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

void test_lease_outlives_owner()
{
    std::cout << "[Test] lease: soltar depois de a CacheOwner parar e ser destruída\n";
    const fs::path dir = fresh_dir("outlive");
    PieceLease lease;
    {
        auto owner = open_owner(dir, 2);
        put(*owner, 1, 0);
        SW_CHECK(acquire(*owner, 1, lease).has_value());
        SW_CHECK(owner->stop().has_value());
        // Depois de stop() o pedido termina com Stopped e o lease fica como estava.
        SW_CHECK(has_error(acquire(*owner, 1, lease), CacheErrorCode::Stopped));
        SW_CHECK(lease.valid() && owner->leases() == 1);
    }
    SW_CHECK(lease.valid());
    lease.reset(); // a tabela é compartilhada: soltar não toca em memória liberada (ASan)
    SW_CHECK(!lease.valid());
    {
        auto owner = open_owner(dir, 2); // reabrir começa sem leases
        SW_CHECK(owner->leases() == 0 && owner->pieces() == 1);
        SW_CHECK(remove(*owner, 1).has_value());
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

void test_lease_does_not_allocate()
{
    std::cout << "[Test] lease: Acquire e soltar sem alocação\n";
    const fs::path dir = fresh_dir("noalloc");
    {
        auto owner = open_owner(dir, 4);
        put(*owner, 1, 0);
        PieceLease lease;
        auto req = CacheRequest::acquire(piece(1), lease);
        SW_CHECK(owner->call(req).has_value()); // aquece
        lease.reset();

        g_allocations.store(0);
        g_counting.store(true);
        g_sink = ::operator new(16); // controle: o contador enxerga um new de verdade
        ::operator delete(g_sink);
        g_counting.store(false);
        SW_CHECK(g_allocations.load() == 1);

        g_allocations.store(0);
        g_counting.store(true);
        for (int i = 0; i < 2000; ++i) {
            SW_CHECK(owner->call(req).has_value());
            lease.reset();
        }
        g_counting.store(false);
        std::cout << "    alocações em 2000 Acquire + soltar: " << g_allocations.load() << "\n";
        SW_CHECK(g_allocations.load() == 0);
        SW_CHECK(owner->leases() == 0);
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

// O critério de aceite. Removedoras tiram peças ao acaso e as devolvem com uma geração nova
// (conteúdo diferente); presas, as peças não podem sumir, mudar de geração nem de slot.
void test_lease_stress()
{
    std::cout << "[Test] lease: estresse, 8 threads com lease contra 2 removedoras\n";
    constexpr uint32_t kHolders = 8;
    constexpr uint32_t kRemovers = 2;
    constexpr uint32_t kPieces = 12;
    constexpr size_t kPieceBytes = 4096;
    const fs::path dir = fresh_dir("stress");
    {
        // Teto = número de peças: o slot liberado por uma remoção é o que a devolução usa, e
        // um slot preso reaproveitado apareceria como outra peça no mesmo slot.
        auto owner = open_owner(dir, kPieces);
        std::array<std::atomic<uint32_t>, kPieces> next_gen{};
        for (uint32_t i = 0; i < kPieces; ++i)
            put(*owner, i, 0, kPieceBytes);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> holds{0}, leased_reads{0}, not_found{0}, busy{0}, removes{0};
        std::vector<std::thread> threads;
        for (uint32_t t = 0; t < kRemovers; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937 rng(77 + t);
                std::vector<std::byte> data(kPieceBytes);
                while (!stop.load()) {
                    const uint32_t i = rng() % kPieces;
                    auto r = remove(*owner, i);
                    if (has_error(r, CacheErrorCode::SlotBusy)) {
                        busy.fetch_add(1);
                        continue;
                    }
                    if (has_error(r, CacheErrorCode::PieceNotFound)) continue; // a outra tirou
                    SW_CHECK(r.has_value());
                    removes.fetch_add(1);
                    const uint32_t gen = next_gen[i].fetch_add(1) + 1;
                    fill(data, i, gen); // devolve com conteúdo novo
                    auto p = CacheRequest::put(piece(i), data);
                    SW_CHECK(owner->call(p).has_value());
                }
            });
        }
        for (uint32_t t = 0; t < kHolders; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937 rng(1000 + t);
                std::vector<std::byte> out(kLayout.slot_size);
                PieceLease lease, again;
                while (!stop.load()) {
                    const uint32_t i = rng() % kPieces;
                    auto r = acquire(*owner, i, lease);
                    if (!r.has_value()) {
                        SW_CHECK(r.error() == CacheErrorCode::PieceNotFound);
                        not_found.fetch_add(1);
                        continue;
                    }
                    SW_CHECK(*r == kPieceBytes && lease.size() == kPieceBytes);
                    holds.fetch_add(1);
                    // Primeira leitura sob o lease: fixa a geração que está no cache.
                    r = get(*owner, i, out);
                    SW_CHECK(r.has_value() && *r == kPieceBytes);
                    const uint32_t gen = generation(out);
                    SW_CHECK(matches(std::span(out).first(kPieceBytes), i, gen));
                    const uint32_t reads = 1 + rng() % 4;
                    for (uint32_t k = 0; k < reads; ++k) {
                        if (rng() % 2 == 0) std::this_thread::yield(); // dá tempo às removedoras
                        r = get(*owner, i, out);
                        SW_CHECK(r.has_value()); // presa: nunca falta
                        SW_CHECK(matches(std::span(out).first(kPieceBytes), i, gen)); // nem muda
                        leased_reads.fetch_add(1);
                    }
                    SW_CHECK(acquire(*owner, i, again).has_value());
                    SW_CHECK(again.slot() == lease.slot()); // o slot não foi reaproveitado
                    again.reset();
                    lease.reset(); // solta fora da dona, nesta thread
                }
            });
        }
        std::this_thread::sleep_for(3s);
        stop.store(true);
        for (auto& th : threads)
            th.join();
        std::cout << "    " << holds.load() << " leases, " << leased_reads.load()
                  << " leituras sob lease, " << not_found.load() << " faltas no Acquire; "
                  << removes.load() << " remoções, " << busy.load()
                  << " recusadas por lease (SlotBusy)\n";
        // As duas pontas foram exercitadas: houve remoção de verdade e recusa por lease.
        SW_CHECK(holds.load() > 0 && leased_reads.load() > 0);
        SW_CHECK(removes.load() > 0 && busy.load() > 0);
        SW_CHECK(owner->leases() == 0);
        for (uint32_t i = 0; i < kPieces; ++i) // nenhum lease ficou preso por engano
            SW_CHECK(remove(*owner, i).has_value());
        SW_CHECK(owner->pieces() == 0);
        SW_CHECK(owner->stop().has_value());
    }
    cleanup(dir);
}

} // namespace

// `only_stress`: só o critério de aceite (para o teste com mutantes da revisão).
void run_cache_lease_tests(bool only_stress)
{
    if (only_stress) {
        test_lease_stress();
        return;
    }
    test_lease_blocks_remove();
    test_lease_move_and_replace();
    test_lease_corrupted_piece_keeps_slot();
    test_lease_outlives_owner();
    test_lease_does_not_allocate();
    test_lease_stress();
}

} // namespace swarmstream

// Sem argumentos roda tudo; `stress` roda só o estresse.
int main(int argc, char** argv)
{
    const bool only_stress = argc > 1 && std::string(argv[1]) == "stress";
    std::cout << "--- Cache lease tests (F1-13) ---\n";
    swarmstream::run_cache_lease_tests(only_stress);
    std::cout << "-> Cache lease tests passed.\n";
    return 0;
}
