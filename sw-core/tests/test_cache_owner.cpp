// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-12: thread dona do cache e fila MPSC de pedidos (docs/04 §Concorrência, ADR-0005).
//
// Critério de aceite: sem data race no TSan com 16 threads leitoras. O teste de estresse
// (test_owner_sixteen_readers) roda 16 leitoras, uma escritora e uma removedora ao mesmo
// tempo contra a mesma CacheOwner; o job "TSan (ubuntu-latest)" do CI roda este executável
// com -fsanitize=thread e halt_on_error=1, então qualquer corrida reportada derruba o teste.
// Fora do TSan, o mesmo teste confere os bytes de toda leitura bem-sucedida (nunca dado de
// outra peça nem meia escrita) e que nenhum pedido fica sem resposta.

#include "check.hpp"
#include "swarmstream/cache_owner.hpp"
#include "swarmstream/mpsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- contagem de alocações
// Substitui o operator new global deste executável para provar que o caminho de leitura
// (submit + wait de um Get, e o atendimento na thread dona) não aloca. Conta só enquanto
// g_counting está ligado, em qualquer thread.
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

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_12_") + name + "_" +
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

// Conteúdo determinístico de cada peça: quem lê sabe o que deveria vir.
void fill(std::vector<std::byte>& v, uint32_t piece_index, uint32_t generation)
{
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] =
            static_cast<std::byte>(static_cast<uint8_t>(piece_index * 131 + generation * 17 + i));
    }
}

bool matches(std::span<const std::byte> v, uint32_t piece_index, uint32_t generation)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] !=
            static_cast<std::byte>(static_cast<uint8_t>(piece_index * 131 + generation * 17 + i))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- MpscQueue

struct TestNode : MpscNode
{
    uint32_t producer = 0;
    uint32_t seq = 0;
};

void test_queue_single_thread()
{
    std::cout << "[Test] MpscQueue: ordem FIFO, vazia e reuso do nó-sentinela\n";
    MpscQueue q;
    SW_CHECK(q.empty() && q.pop() == nullptr);
    std::vector<TestNode> nodes(10);
    for (uint32_t i = 0; i < 10; ++i)
        nodes[i].seq = i;

    q.push(&nodes[0]);
    SW_CHECK(!q.empty());
    SW_CHECK(q.pop() == &nodes[0]); // um só item: passa pelo ramo que reinsere o sentinela
    SW_CHECK(q.empty() && q.pop() == nullptr);

    for (uint32_t i = 1; i < 10; ++i)
        q.push(&nodes[i]);
    for (uint32_t i = 1; i < 5; ++i)
        SW_CHECK(q.pop() == &nodes[i]);
    q.push(&nodes[0]); // intercala push e pop
    for (uint32_t i = 5; i < 10; ++i)
        SW_CHECK(q.pop() == &nodes[i]);
    SW_CHECK(q.pop() == &nodes[0]);
    SW_CHECK(q.empty() && q.pop() == nullptr);
}

void test_queue_sixteen_producers()
{
    std::cout
        << "[Test] MpscQueue: 16 produtoras, nada perdido nem duplicado, FIFO por produtora\n";
    constexpr uint32_t kProducers = 16;
    constexpr uint32_t kPerProducer = 20000;
    MpscQueue q;
    std::vector<std::unique_ptr<TestNode[]>> nodes; // nós não se copiam (atômico dentro)
    for (uint32_t p = 0; p < kProducers; ++p)
        nodes.push_back(std::make_unique<TestNode[]>(kPerProducer));
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (uint32_t s = 0; s < kPerProducer; ++s) {
                nodes[p][s].producer = p;
                nodes[p][s].seq = s;
                q.push(&nodes[p][s]);
            }
        });
    }
    go.store(true, std::memory_order_release);
    std::vector<uint32_t> next(kProducers, 0);
    uint64_t received = 0;
    while (received < uint64_t{kProducers} * kPerProducer) {
        MpscNode* n = q.pop();
        if (n == nullptr) {
            std::this_thread::yield();
            continue;
        }
        auto* t = static_cast<TestNode*>(n);
        SW_CHECK(t->producer < kProducers);
        SW_CHECK(t->seq == next[t->producer]); // nem pulou nem repetiu
        ++next[t->producer];
        ++received;
    }
    for (auto& t : producers)
        t.join();
    SW_CHECK(q.empty() && q.pop() == nullptr);
    std::cout << "    " << received << " nós recebidos\n";
}

// ---------------------------------------------------------------- CacheOwner

constexpr StoreLayout kLayout{.slots_per_file = 8, .slot_size = 64 * 1024};

void test_owner_put_get_remove()
{
    std::cout << "[Test] CacheOwner: put, get, remove e erros\n";
    const fs::path dir = fresh_dir("basic");
    {
        auto started =
            CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 3, .layout = kLayout});
        SW_CHECK(started.has_value());
        CacheOwner& owner = **started;
        SW_CHECK(!owner.is_owner_thread());

        std::vector<std::byte> a(5000), b(kLayout.slot_size), out(kLayout.slot_size);
        fill(a, 1, 0);
        fill(b, 2, 0);

        auto get_missing = CacheRequest::get(piece(1), out);
        auto r = owner.call(get_missing);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound);

        auto put_a = CacheRequest::put(piece(1), a);
        r = owner.call(put_a);
        SW_CHECK(r.has_value() && *r == a.size());
        auto put_b = CacheRequest::put(piece(2), b); // slot inteiro
        SW_CHECK(owner.call(put_b).has_value());
        auto put_again = CacheRequest::put(piece(1), a); // já existe: sucesso, sem regravar
        r = owner.call(put_again);
        SW_CHECK(r.has_value() && *r == a.size());
        SW_CHECK(owner.pieces() == 2);

        auto get_a = CacheRequest::get(piece(1), out);
        r = owner.call(get_a);
        SW_CHECK(r.has_value() && *r == a.size() && matches(std::span(out).first(a.size()), 1, 0));
        auto get_b = CacheRequest::get(piece(2), out);
        r = owner.call(get_b);
        SW_CHECK(r.has_value() && *r == b.size() && matches(out, 2, 0));

        std::vector<std::byte> small(100);
        auto get_small = CacheRequest::get(piece(1), small); // buffer menor que a peça
        r = owner.call(get_small);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidSize);

        std::vector<std::byte> too_big(kLayout.slot_size + 1);
        auto put_big = CacheRequest::put(piece(3), too_big);
        r = owner.call(put_big);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidSize);
        auto put_empty = CacheRequest::put(piece(3), std::span<const std::byte>{});
        r = owner.call(put_empty);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::InvalidSize);

        auto put_c = CacheRequest::put(piece(3), a);
        SW_CHECK(owner.call(put_c).has_value());
        auto put_d = CacheRequest::put(piece(4), a); // 3 slots: o 4º não cabe
        r = owner.call(put_d);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::OutOfSlots);

        auto rm = CacheRequest::remove(piece(2));
        SW_CHECK(owner.call(rm).has_value());
        auto rm_again = CacheRequest::remove(piece(2));
        r = owner.call(rm_again);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound);
        auto get_removed = CacheRequest::get(piece(2), out);
        r = owner.call(get_removed);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound);
        SW_CHECK(owner.call(put_d).has_value()); // o slot liberado é reaproveitado
        SW_CHECK(owner.pieces() == 3);

        SW_CHECK(owner.stop().has_value());
        auto after_stop = CacheRequest::get(piece(1), out);
        r = owner.call(after_stop);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::Stopped);
        SW_CHECK(owner.stop().has_value()); // de novo: nada a fazer
    }
    cleanup(dir);
}

void test_owner_request_lifecycle()
{
    std::cout << "[Test] CacheOwner: pedido pendente não é reenviado; pedido concluído pode ser\n";
    const fs::path dir = fresh_dir("lifecycle");
    {
        auto started =
            CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 4, .layout = kLayout});
        SW_CHECK(started.has_value());
        CacheOwner& owner = **started;
        std::vector<std::byte> data(1000), out(kLayout.slot_size);
        fill(data, 9, 0);
        auto put = CacheRequest::put(piece(9), data);
        SW_CHECK(owner.call(put).has_value());

        auto get = CacheRequest::get(piece(9), out);
        SW_CHECK(!get.done()); // nunca enviado
        // Enfileira muitos pedidos para que o `get` ainda esteja na fila no segundo submit.
        std::deque<CacheRequest> burst; // CacheRequest não se move: deque constrói no lugar
        for (int i = 0; i < 64; ++i) {
            burst.emplace_back(CacheOp::Get, piece(9), std::span<const std::byte>{},
                               std::span(out));
        }
        for (auto& b : burst)
            SW_CHECK(owner.submit(b));
        SW_CHECK(owner.submit(get));
        const bool second = owner.submit(get); // false se ainda pendente; true se já concluiu
        auto r = get.wait();
        SW_CHECK(r.has_value() && *r == data.size());
        if (!second) std::cout << "    segundo submit recusado enquanto pendente\n";
        SW_CHECK(get.done());
        for (auto& b : burst)
            SW_CHECK(b.wait().has_value());

        // Concluído pode ser reenviado (reuso sem alocar).
        SW_CHECK(owner.submit(get));
        r = get.wait();
        SW_CHECK(r.has_value() && *r == data.size());
        // Polling: done() nunca bloqueia e acaba ficando true.
        SW_CHECK(owner.submit(get));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!get.done()) {
            SW_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        SW_CHECK(get.result().has_value());
    }
    cleanup(dir);
}

void test_owner_read_path_does_not_allocate()
{
    std::cout << "[Test] CacheOwner: caminho de leitura sem alocação (submit, atendimento, wait)\n";
    const fs::path dir = fresh_dir("noalloc");
    {
        auto started =
            CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 4, .layout = kLayout});
        SW_CHECK(started.has_value());
        CacheOwner& owner = **started;
        std::vector<std::byte> data(30000), out(kLayout.slot_size);
        fill(data, 5, 0);
        auto put = CacheRequest::put(piece(5), data);
        SW_CHECK(owner.call(put).has_value());
        auto get = CacheRequest::get(piece(5), out);
        SW_CHECK(owner.call(get).has_value()); // aquece (arquivo já aberto)

        // Controle: o contador enxerga um new de verdade (senão o "0" abaixo não prova nada).
        g_allocations.store(0);
        g_counting.store(true);
        // Chamada explícita guardada num volátil: um `delete new int` o compilador pode
        // eliminar em Release (elisão de alocação), e o controle falharia.
        g_sink = ::operator new(16);
        ::operator delete(g_sink);
        g_counting.store(false);
        SW_CHECK(g_allocations.load() == 1);

        g_allocations.store(0);
        g_counting.store(true);
        for (int i = 0; i < 2000; ++i) {
            auto r = owner.call(get);
            SW_CHECK(r.has_value() && *r == data.size());
        }
        auto missing = CacheRequest::get(piece(6), out);
        auto r = owner.call(missing); // falta também não aloca
        g_counting.store(false);
        SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound);
        std::cout << "    alocações em 2001 leituras: " << g_allocations.load() << "\n";
        SW_CHECK(g_allocations.load() == 0);
        SW_CHECK(matches(std::span(out).first(data.size()), 5, 0));
    }
    cleanup(dir);
}

void test_owner_sixteen_readers()
{
    std::cout << "[Test] CacheOwner: 16 leitoras + escritora + removedora (critério do TSan)\n";
    constexpr uint32_t kReaders = 16;
    constexpr uint32_t kPieces = 24;
    constexpr size_t kPieceBytes = 8192;
    const fs::path dir = fresh_dir("stress");
    {
        auto started = CacheOwner::start(
            CacheOwnerConfig{.dir = dir, .total_slots = kPieces, .layout = kLayout});
        SW_CHECK(started.has_value());
        CacheOwner& owner = **started;

        // Geração atual de cada peça, escrita só pela escritora/removedora; a leitora aceita o
        // conteúdo de qualquer geração já publicada (a peça pode ter sido trocada no meio).
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> hits{0}, misses{0}, bad{0}, puts{0}, removes{0};

        auto writer = std::thread([&] {
            std::vector<std::byte> data(kPieceBytes);
            for (uint32_t i = 0; i < kPieces && !stop.load(); ++i) {
                fill(data, i, 0);
                auto put = CacheRequest::put(piece(i), data);
                SW_CHECK(owner.call(put).has_value());
                puts.fetch_add(1);
            }
        });
        auto remover = std::thread([&] {
            std::mt19937 rng(12);
            std::vector<std::byte> data(kPieceBytes);
            while (!stop.load()) {
                const uint32_t i = rng() % kPieces;
                auto rm = CacheRequest::remove(piece(i));
                auto r = owner.call(rm);
                SW_CHECK(r.has_value() || r.error() == CacheErrorCode::PieceNotFound);
                if (r.has_value()) {
                    removes.fetch_add(1);
                    fill(data, i, 0); // devolve a peça (mesmo conteúdo)
                    auto put = CacheRequest::put(piece(i), data);
                    auto p = owner.call(put);
                    SW_CHECK(p.has_value() || p.error() == CacheErrorCode::OutOfSlots);
                }
                std::this_thread::sleep_for(1ms);
            }
        });
        std::vector<std::thread> readers;
        for (uint32_t t = 0; t < kReaders; ++t) {
            readers.emplace_back([&, t] {
                std::mt19937 rng(1000 + t);
                std::vector<std::byte> out(kLayout.slot_size);
                while (!stop.load()) {
                    const uint32_t i = rng() % kPieces;
                    auto req = CacheRequest::get(piece(i), out); // na pilha: não aloca
                    Result<size_t, CacheErrorCode> r = Unexpected(CacheErrorCode::IoError);
                    if (t % 2 == 0) {
                        r = owner.call(req); // metade espera bloqueando
                    } else {
                        SW_CHECK(owner.submit(req)); // metade faz polling, como o jogo
                        while (!req.done())
                            std::this_thread::yield();
                        r = req.result();
                    }
                    if (r.has_value()) {
                        hits.fetch_add(1);
                        if (*r != kPieceBytes || !matches(std::span(out).first(*r), i, 0)) {
                            bad.fetch_add(1);
                        }
                    } else {
                        SW_CHECK(r.error() == CacheErrorCode::PieceNotFound);
                        misses.fetch_add(1);
                    }
                }
            });
        }
        std::this_thread::sleep_for(3s);
        stop.store(true);
        writer.join();
        remover.join();
        for (auto& r : readers)
            r.join();
        std::cout << "    leituras: " << hits.load() << " acertos, " << misses.load() << " faltas; "
                  << puts.load() << " escritas, " << removes.load()
                  << " remoções; pedidos atendidos: " << owner.requests_processed() << "\n";
        SW_CHECK(bad.load() == 0);
        SW_CHECK(hits.load() > 0);
        SW_CHECK(owner.stop().has_value());
    }
    cleanup(dir);
}

void test_owner_stop_with_concurrent_submitters()
{
    std::cout << "[Test] CacheOwner: stop() com produtoras ativas; todo pedido recebe resposta\n";
    const fs::path dir = fresh_dir("stop");
    {
        auto started =
            CacheOwner::start(CacheOwnerConfig{.dir = dir, .total_slots = 4, .layout = kLayout});
        SW_CHECK(started.has_value());
        CacheOwner& owner = **started;
        constexpr int kProducers = 8;
        std::atomic<uint64_t> served{0}, stopped{0};
        std::vector<std::thread> producers;
        // Cada produtora envia sem parar até receber Stopped. Assim a parada acontece sempre
        // com produtoras ativas, em qualquer máquina (antes, com um número fixo de pedidos, o
        // MSVC em Release terminava tudo antes do stop()).
        for (int t = 0; t < kProducers; ++t) {
            producers.emplace_back([&] {
                std::vector<std::byte> out(64);
                for (;;) {
                    auto req = CacheRequest::get(piece(1), out);
                    auto r = owner.call(req); // nunca pode ficar pendurado
                    SW_CHECK(!r.has_value());
                    if (r.error() == CacheErrorCode::Stopped) {
                        stopped.fetch_add(1);
                        return;
                    }
                    SW_CHECK(r.error() == CacheErrorCode::PieceNotFound);
                    served.fetch_add(1);
                }
            });
        }
        // Para só depois de a dona ter atendido bastante, com todas ainda enviando.
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (served.load() < 5000) {
            SW_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        SW_CHECK(owner.stop().has_value());
        for (auto& p : producers)
            p.join();
        std::cout << "    " << served.load() << " atendidos antes da parada; " << stopped.load()
                  << " de " << kProducers << " produtoras receberam Stopped\n";
        SW_CHECK(stopped.load() == kProducers); // todas saíram pela resposta, nenhuma pendurou
    }
    cleanup(dir);
}

void test_owner_start_errors()
{
    std::cout << "[Test] CacheOwner: start() devolve o erro do store\n";
    auto no_dir = CacheOwner::start(CacheOwnerConfig{
        .dir = fs::temp_directory_path() / "swarmstream_nao_existe_f1_12", .total_slots = 4});
    SW_CHECK(!no_dir.has_value() && no_dir.error() == CacheErrorCode::IoError);
    const fs::path dir = fresh_dir("errors");
    auto bad_layout = CacheOwner::start(
        CacheOwnerConfig{.dir = dir,
                         .total_slots = 4,
                         .layout = StoreLayout{.slots_per_file = 1, .slot_size = 1000}});
    SW_CHECK(!bad_layout.has_value() && bad_layout.error() == CacheErrorCode::InvalidSize);
    cleanup(dir);
}

} // namespace

void run_cache_owner_tests()
{
    test_queue_single_thread();
    test_queue_sixteen_producers();
    test_owner_start_errors();
    test_owner_put_get_remove();
    test_owner_request_lifecycle();
    test_owner_read_path_does_not_allocate();
    test_owner_stop_with_concurrent_submitters();
    test_owner_sixteen_readers();
}

} // namespace swarmstream

int main()
{
    std::cout << "--- Cache owner tests (F1-12) ---\n";
    swarmstream::run_cache_owner_tests();
    std::cout << "-> Cache owner tests passed.\n";
    return 0;
}
