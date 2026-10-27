// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-15: cache de blocos descomprimidos em RAM + pool de buffers.
//
// Critério de aceite: zero alocações por leitura no caminho quente (medido). O teste
// test_hot_path_does_not_allocate conta as chamadas a `operator new` durante acertos, faltas com
// preenchimento (que removem por CLOCK) e leases; o controle prova que o contador enxerga um new
// de verdade. Os demais testes fixam as regras: conteúdo, CLOCK, lease nunca removido, abortar
// devolve o buffer, duplicidade e um estresse com threads (o job TSan do CI roda este
// executável).
//
// Limites da medição de alocações (revisão independente): o contador substitui só
// `operator new(size_t)` (`new[]` e `nothrow` passam por ele na libstdc++). Alocação alinhada
// (`align_val_t`) e `malloc` direto NÃO são contados; este código não usa nenhum dos dois, mas
// quem trocar o alocador do pool (ex.: alinhar à página) precisa contar `align_val_t`. O GCC
// elimina o par `new`/`delete` quando o resultado não é usado (`delete new int`), por isso o
// controle e o mutante usam `::operator new` chamado direto com o ponteiro guardado.

#include "check.hpp"
#include "swarmstream/block_ram_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

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

constexpr size_t kBuf = 4096; // buffers pequenos nos testes; a lógica é a mesma dos 256 KiB

BlockKey key(uint32_t piece, uint32_t block)
{
    PieceHash h;
    h.bytes[0] = static_cast<uint8_t>(piece);
    h.bytes[1] = static_cast<uint8_t>(piece >> 8);
    return BlockKey{PieceId(piece, h), block};
}

// Conteúdo determinístico por chave: permite conferir que o buffer certo voltou.
uint8_t expected_byte(const BlockKey& k, size_t i)
{
    return static_cast<uint8_t>(k.piece.index * 31 + k.block * 7 + i);
}

bool insert(BlockRamCache& c, const BlockKey& k, size_t size = 1000)
{
    auto fill = c.reserve(k, size);
    if (!fill.has_value()) return false;
    auto buf = fill.value().buffer();
    SW_CHECK(buf.size() == size);
    for (size_t i = 0; i < size; ++i)
        buf[i] = expected_byte(k, i);
    BlockLease lease = fill.value().commit();
    return lease.valid();
}

bool content_ok(const BlockLease& l, const BlockKey& k, size_t size = 1000)
{
    if (!l.valid() || l.size() != size) return false;
    for (size_t i = 0; i < size; ++i) {
        if (l.data()[i] != expected_byte(k, i)) return false;
    }
    return true;
}

void test_hit_and_miss()
{
    std::cout << "[Test] bloco: falta, preenchimento, acerto e conteúdo\n";
    BlockRamCache c(8 * kBuf, kBuf);
    SW_CHECK(!c.acquire(key(1, 0)).valid());
    SW_CHECK(insert(c, key(1, 0)));
    SW_CHECK(insert(c, key(1, 1), 500));
    BlockLease a = c.acquire(key(1, 0));
    SW_CHECK(content_ok(a, key(1, 0)));
    BlockLease b = c.acquire(key(1, 1));
    SW_CHECK(content_ok(b, key(1, 1), 500));
    BlockLease b2 = c.acquire(key(1, 1)); // dois leases do mesmo bloco, mesmo buffer
    SW_CHECK(b2.data() == b.data());
    SW_CHECK(!c.acquire(key(2, 0)).valid());
    const BlockCacheStats s = c.stats();
    SW_CHECK(s.hits == 3 && s.misses == 2 && s.resident_blocks == 2 && s.leased_blocks == 2);
}

void test_invalid_and_duplicate()
{
    std::cout << "[Test] bloco: tamanho inválido, duplicado e preenchimento em andamento\n";
    BlockRamCache c(4 * kBuf, kBuf);
    SW_CHECK(c.reserve(key(1, 0), 0).error() == BlockCacheError::InvalidSize);
    SW_CHECK(c.reserve(key(1, 0), kBuf + 1).error() == BlockCacheError::InvalidSize);
    auto fill = c.reserve(key(1, 0), 100);
    SW_CHECK(fill.has_value());
    SW_CHECK(c.reserve(key(1, 0), 100).error() == BlockCacheError::Filling);
    SW_CHECK(!c.acquire(key(1, 0)).valid()); // meio preenchido não é visível
    BlockLease l = fill.value().commit();
    SW_CHECK(l.valid());
    SW_CHECK(c.reserve(key(1, 0), 100).error() == BlockCacheError::Exists);
}

void test_abort_returns_buffer()
{
    std::cout << "[Test] bloco: abortar devolve o buffer\n";
    BlockRamCache c(2 * kBuf, kBuf);
    {
        auto f1 = c.reserve(key(1, 0), 10);
        auto f2 = c.reserve(key(1, 1), 10);
        SW_CHECK(f1.has_value() && f2.has_value());
        SW_CHECK(c.reserve(key(1, 2), 10).error() == BlockCacheError::OutOfBuffers);
    } // os dois são destruídos sem commit
    SW_CHECK(c.stats().resident_blocks == 0);
    SW_CHECK(insert(c, key(1, 0)));
    SW_CHECK(insert(c, key(1, 1)));
    SW_CHECK(!c.acquire(key(9, 9)).valid());
}

void test_leased_block_never_evicted()
{
    std::cout << "[Test] bloco: com lease nunca é removido; sem lease sai por CLOCK\n";
    BlockRamCache c(4 * kBuf, kBuf);
    for (uint32_t b = 0; b < 4; ++b)
        SW_CHECK(insert(c, key(1, b)));
    BlockLease held = c.acquire(key(1, 2));
    SW_CHECK(held.valid());
    for (uint32_t b = 0; b < 100; ++b)
        SW_CHECK(insert(c, key(2, b)));
    SW_CHECK(content_ok(held, key(1, 2))); // o buffer preso não foi reaproveitado
    SW_CHECK(c.acquire(key(1, 2)).valid());
    SW_CHECK(c.stats().evictions >= 96);

    // Tudo preso: não há o que remover.
    BlockRamCache d(2 * kBuf, kBuf);
    BlockLease l0, l1;
    {
        auto f = d.reserve(key(1, 0), 10);
        l0 = f.value().commit();
        auto g = d.reserve(key(1, 1), 10);
        l1 = g.value().commit();
    }
    SW_CHECK(d.reserve(key(1, 2), 10).error() == BlockCacheError::OutOfBuffers);
    l1.reset();
    SW_CHECK(d.reserve(key(1, 2), 10).has_value()); // soltou: o buffer volta a ser candidato
}

void test_clock_second_chance()
{
    std::cout << "[Test] bloco: CLOCK remove exatamente a vítima sem marca\n";
    BlockRamCache c(4 * kBuf, kBuf);
    for (uint32_t b = 0; b < 4; ++b)
        SW_CHECK(insert(c, key(2, b)));
    // O 5º bloco dá a primeira volta: apaga as marcas e remove o 0; a mão para no 1.
    SW_CHECK(insert(c, key(2, 4)));
    SW_CHECK(!c.acquire(key(2, 0)).valid());
    // Marca o 1 e o 3 (sem usar acquire de verificação antes: ele também marcaria).
    (void)c.acquire(key(2, 1));
    (void)c.acquire(key(2, 3));
    // Mão no 1: marcado (perde a marca), 2 sem marca => sai o 2, e só ele. Sem a segunda chance
    // sairia o 1.
    SW_CHECK(insert(c, key(2, 5)));
    SW_CHECK(!c.acquire(key(2, 2)).valid());
    SW_CHECK(c.acquire(key(2, 1)).valid() && c.acquire(key(2, 3)).valid());
    SW_CHECK(c.acquire(key(2, 4)).valid() && c.acquire(key(2, 5)).valid());
}

void test_hot_path_does_not_allocate()
{
    std::cout << "[Test] bloco: caminho quente sem alocação (medido)\n";
    BlockRamCache c(16 * kBuf, kBuf);
    for (uint32_t b = 0; b < 16; ++b)
        SW_CHECK(insert(c, key(1, b)));

    g_allocations.store(0);
    g_counting.store(true);
    g_sink = ::operator new(16); // controle: o contador enxerga um new de verdade
    ::operator delete(g_sink);
    g_counting.store(false);
    SW_CHECK(g_allocations.load() == 1);

    // Acertos + leases movidos.
    g_allocations.store(0);
    g_counting.store(true);
    uint64_t bytes = 0;
    for (int i = 0; i < 20000; ++i) {
        BlockLease l = c.acquire(key(1, static_cast<uint32_t>(i % 16)));
        BlockLease m = std::move(l);
        bytes += m.size();
    }
    g_counting.store(false);
    const uint64_t hit_allocs = g_allocations.load();
    std::cout << "    alocações em 20000 acertos (acquire + mover + soltar): " << hit_allocs << " ("
              << bytes << " B vistos)\n";
    SW_CHECK(hit_allocs == 0);

    // Faltas: reservar, escrever, publicar, com remoção CLOCK a cada uma (16 buffers, 5000 chaves).
    g_allocations.store(0);
    g_counting.store(true);
    uint32_t inserted = 0;
    for (uint32_t i = 0; i < 5000; ++i) {
        auto f = c.reserve(key(100 + i, 0), 1000);
        if (!f.has_value()) continue;
        std::memset(f.value().buffer().data(), 0xAB, 1000);
        BlockLease l = f.value().commit();
        inserted += l.valid() ? 1 : 0;
    }
    // Reserva abandonada e erros também não alocam.
    for (int i = 0; i < 1000; ++i) {
        auto f = c.reserve(key(900000, 0), 1000);
        SW_CHECK(f.has_value());
        auto dup = c.reserve(key(900000, 0), 1000);
        SW_CHECK(!dup.has_value());
    }
    g_counting.store(false);
    const uint64_t miss_allocs = g_allocations.load();
    std::cout << "    alocações em 5000 faltas com remoção + 1000 abortos: " << miss_allocs << " ("
              << inserted << " inseridos, " << c.stats().evictions << " remoções)\n";
    SW_CHECK(inserted == 5000);
    SW_CHECK(c.stats().evictions >= 4900);
    SW_CHECK(miss_allocs == 0);
}

void test_stress_threads()
{
    std::cout << "[Test] bloco: estresse com 8 threads (conteúdo e lease sob remoção)\n";
    BlockRamCache c(32 * kBuf, kBuf);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    std::atomic<uint64_t> ops{0};
    auto worker = [&](uint32_t seed) {
        uint32_t x = seed * 2654435761u + 1;
        while (!stop.load(std::memory_order_relaxed)) {
            x = x * 1664525u + 1013904223u;
            const BlockKey k = key(x % 200, (x >> 8) % 4);
            BlockLease l = c.acquire(k);
            if (!l.valid()) {
                auto f = c.reserve(k, 1000);
                if (!f.has_value()) continue;
                auto buf = f.value().buffer();
                for (size_t i = 0; i < buf.size(); ++i)
                    buf[i] = expected_byte(k, i);
                l = f.value().commit();
            }
            if (!content_ok(l, k)) bad.fetch_add(1);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> ts;
    for (uint32_t t = 0; t < 8; ++t)
        ts.emplace_back(worker, t + 1);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);
    for (auto& t : ts)
        t.join();
    const BlockCacheStats s = c.stats();
    std::cout << "    " << ops.load() << " operações, " << s.evictions << " remoções, " << s.hits
              << " acertos\n";
    SW_CHECK(bad.load() == 0);
    SW_CHECK(s.leased_blocks == 0);
    SW_CHECK(s.evictions > 0);
}

void test_lease_held_long_under_churn()
{
    std::cout << "[Test] bloco: lease segurado por tempo: o conteudo nao muda sob troca intensa\n";
    BlockRamCache c(8 * kBuf, kBuf);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    std::atomic<uint64_t> held_checks{0};
    auto worker = [&](uint32_t seed) {
        uint32_t x = seed * 2654435761u + 1;
        while (!stop.load(std::memory_order_relaxed)) {
            x = x * 1664525u + 1013904223u;
            const BlockKey k = key(x % 40, (x >> 8) % 3);
            BlockLease l = c.acquire(k);
            if (!l.valid()) {
                auto f = c.reserve(k, 1000);
                if (!f.has_value()) continue;
                auto buf = f.value().buffer();
                for (size_t i = 0; i < buf.size(); ++i)
                    buf[i] = expected_byte(k, i);
                l = f.value().commit();
            }
            // segura o lease enquanto os outros trocam; confere antes e depois
            for (int rep = 0; rep < 3; ++rep) {
                for (size_t i = 0; i < l.size(); ++i) {
                    if (l.data()[i] != expected_byte(k, i)) bad.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50 + (x >> 20) % 200));
            }
            held_checks.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> ts;
    for (uint32_t t = 0; t < 8; ++t)
        ts.emplace_back(worker, t + 11);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop.store(true);
    for (auto& t : ts)
        t.join();
    const auto s = c.stats();
    std::cout << "    " << held_checks.load() << " leases segurados, " << s.evictions
              << " remocoes, " << s.hits << " acertos\n";
    SW_CHECK(bad.load() == 0);
    SW_CHECK(s.leased_blocks == 0 && held_checks.load() > 100);
}

void test_same_block_race()
{
    std::cout << "[Test] bloco: duas (ou mais) faltas do mesmo bloco: so uma preenche\n";
    for (int round = 0; round < 300; ++round) {
        BlockRamCache c(4 * kBuf, kBuf);
        std::atomic<int> go{0};
        std::atomic<int> winners{0};
        std::atomic<int> others{0};
        auto worker = [&] {
            while (go.load() == 0) {
            }
            auto f = c.reserve(key(7, 7), 100);
            if (f.has_value()) {
                winners.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                BlockLease l = f.value().commit();
                SW_CHECK(l.valid());
            } else {
                const auto e = f.error();
                SW_CHECK(e == BlockCacheError::Filling || e == BlockCacheError::Exists);
                others.fetch_add(1);
            }
        };
        std::vector<std::thread> ts;
        for (int t = 0; t < 6; ++t)
            ts.emplace_back(worker);
        go.store(1);
        for (auto& t : ts)
            t.join();
        SW_CHECK(winners.load() == 1 && others.load() == 5);
        const auto s = c.stats();
        SW_CHECK(s.resident_blocks == 1 && s.leased_blocks == 0);
    }
}

void test_tiny_pool_never_blocks()
{
    std::cout
        << "[Test] bloco: pool de 2 buffers com 8 threads: OutOfBuffers, sem travar nem estourar\n";
    BlockRamCache c(2 * kBuf, kBuf);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> oob{0};
    std::atomic<uint64_t> ok{0};
    auto worker = [&](uint32_t seed) {
        uint32_t x = seed * 2654435761u + 1;
        while (!stop.load(std::memory_order_relaxed)) {
            x = x * 1664525u + 1013904223u;
            const BlockKey k = key(x % 30, 0);
            auto f = c.reserve(k, 500);
            if (!f.has_value()) {
                if (f.error() == BlockCacheError::OutOfBuffers) oob.fetch_add(1);
                continue;
            }
            BlockLease l = f.value().commit();
            if (x & 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(100)); // preso um tempo
            }
            ok.fetch_add(1);
        }
    };
    std::vector<std::thread> ts;
    for (uint32_t t = 0; t < 8; ++t)
        ts.emplace_back(worker, t + 3);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);
    for (auto& t : ts)
        t.join(); // se algo bloqueasse, o join nao voltaria
    const auto s = c.stats();
    std::cout << "    " << ok.load() << " ok, " << oob.load() << " OutOfBuffers\n";
    SW_CHECK(s.leased_blocks == 0 && s.resident_blocks <= 2 && oob.load() > 0);
}

} // namespace
} // namespace swarmstream

int main()
{
    using namespace swarmstream;
    test_hit_and_miss();
    test_invalid_and_duplicate();
    test_abort_returns_buffer();
    test_leased_block_never_evicted();
    test_clock_second_chance();
    test_hot_path_does_not_allocate();
    test_stress_threads();
    test_lease_held_long_under_churn();
    test_same_block_race();
    test_tiny_pool_never_blocks();
    std::cout << "OK\n";
    return 0;
}
