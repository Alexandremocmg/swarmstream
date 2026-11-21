// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-67 parte B: registro de pedidos (pool fixo, ids do processo, threads de conclusão, "dentro de
// callback", protocolo de fechamento) e lista de leases por arquivo, no nível do componente
// (F1-60 §1.5-§1.7). Pedidos SINTÉTICOS: o teste faz o papel da fonte (complete por id). A prova
// pela API pública (sw_close, sw_read_async, sw_cancel) é da parte D e da etapa 2.
//
// Cobre: callback exatamente uma vez, numa thread de conclusão, com a marca "dentro de callback";
// pool cheio => QueueFull sem alocar; cancel (id 0, UINT64_MAX, nunca emitido, concluído, de outro
// dono) => NotFound; §1.5 testes 1 (dois registros, ids cruzados) e 2 (16 threads, 2 registros, ids
// distintos e != 0); close_file: pendentes cancelados, pedido "preso" EM_VOO não segura, o que já
// concluía recebe o resultado, pedido novo => Cancelled sem callback (inclusive vindo de callback),
// close_file de callback => Busy, nenhum callback depois do retorno; stop (ShuttingDown, Busy de
// callback, idempotente); leases por arquivo; criação: alinhamento do pool, falha do alloc_fn em
// cada ponto, varredura de operator new sem pontos excluídos (resumo [F1-67B]), thread que o
// sistema não cria => Internal. O job TSan roda este teste.
//
// O esgotamento dos ids (§1.5 teste 4) é do executável test_request_id_exhaustion.cpp (um processo
// por caso, porque mexe no contador global).
//
// O operator new deste executável é o de oom_support.hpp (contador e falha injetada).

#include "api_allocator.hpp"
#include "check.hpp"
#include "oom_support.hpp"
#include "request_registry.hpp"
#include "request_registry_testing.hpp"
#include "test_allocators.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using swarmstream::detail::AllocStatus;
using swarmstream::detail::ContextAllocator;
using swarmstream::detail::FileLeases;
using swarmstream::detail::FileRequests;
using swarmstream::detail::in_callback;
using swarmstream::detail::kRequestPoolAlign;
using swarmstream::detail::Request;
using swarmstream::detail::RequestKind;
using swarmstream::detail::RequestRegistry;
using swarmstream::detail::RequestRegistryConfig;
using swarmstream::detail::RequestRegistryCreate;
using swarmstream::detail::RequestStatus;
using swarmstream::detail::SubmitArgs;
using swarmstream::detail::SubmitResult;
using swarmstream::test::ExactAlignAllocator;

void wait_or_fail(const std::atomic<bool>& flag, std::chrono::milliseconds limit, const char* what)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > end) {
            std::fprintf(stderr, "TEMPO ESGOTADO esperando: %s\n", what);
            std::fflush(nullptr);
            std::_Exit(1);
        }
        std::this_thread::sleep_for(1ms);
    }
}

// Espera ativa (yield): no Windows o sleep_for tem granularidade de ~15 ms, e as esperas por
// callback são muitas.
template <class Pred>
void wait_until_or_fail(Pred pred, std::chrono::milliseconds limit, const char* what)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            std::fprintf(stderr, "TEMPO ESGOTADO esperando: %s\n", what);
            std::fflush(nullptr);
            std::_Exit(1);
        }
        std::this_thread::yield();
    }
}

// Registro de callbacks recebidos: por id, quantas vezes, com que resultado, em que thread.
struct CallbackLog
{
    static constexpr std::size_t kMax = 1 << 16;
    struct Item
    {
        sw_request_id id;
        sw_result result;
        std::uint64_t bytes;
        bool in_cb;
        std::thread::id thread;
    };
    std::mutex m;
    std::vector<Item> items;
    std::atomic<std::uint64_t> count{0};
    CallbackLog() { items.reserve(kMax); }

    static void SW_CALL cb(void* user, sw_request_id id, sw_result result, std::uint64_t bytes)
    {
        auto* self = static_cast<CallbackLog*>(user);
        const bool inside = in_callback();
        std::lock_guard<std::mutex> g(self->m);
        SW_CHECK(self->items.size() < kMax);
        self->items.push_back(Item{id, result, bytes, inside, std::this_thread::get_id()});
        self->count.fetch_add(1);
    }

    std::size_t times(sw_request_id id)
    {
        std::lock_guard<std::mutex> g(m);
        return static_cast<std::size_t>(
            std::count_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; }));
    }
    Item find(sw_request_id id)
    {
        std::lock_guard<std::mutex> g(m);
        for (const Item& i : items) {
            if (i.id == id) return i;
        }
        SW_CHECK(false);
        return {};
    }
};

struct Reg
{
    RequestRegistry* r = nullptr;
    explicit Reg(const ContextAllocator& a, std::uint32_t capacity, std::uint32_t threads,
                 const void* owner)
    {
        RequestRegistryConfig cfg;
        cfg.capacity = capacity;
        cfg.completion_threads = threads;
        cfg.owner = owner;
        const RequestRegistryCreate c = RequestRegistry::create(a, cfg);
        SW_CHECK(c.status == RequestStatus::Ok && c.registry != nullptr);
        r = c.registry;
    }
    ~Reg()
    {
        if (r != nullptr) SW_CHECK(RequestRegistry::destroy(r) == RequestStatus::Ok);
    }
    RequestRegistry* operator->() const { return r; }
};

SubmitArgs args_for(FileRequests& f, CallbackLog& log)
{
    SubmitArgs a;
    a.file = &f;
    a.kind = RequestKind::Synthetic;
    a.cb = &CallbackLog::cb;
    a.user = &log;
    return a;
}

void test_create_layout_and_destroy()
{
    ExactAlignAllocator ea;
    const ContextAllocator ca(ea.as_sw());
    int owner = 1;
    {
        Reg reg(ca, 100, 2, &owner);
        SW_CHECK(reg->capacity() == 100);
        SW_CHECK(reg->owner() == &owner);
        SW_CHECK(reg->pending() == 0);
        // Objeto, pool (linha de cache) e tabela: três blocos do alocador do contexto (nível 1).
        SW_CHECK(ea.live() == 3);
    }
    SW_CHECK(ea.live() == 0);
    SW_CHECK(ea.alloc_calls() == ea.free_calls());
    // Alinhamento do pool: o alocador de teste viu o pedido de max(alignof(Request), 64).
    bool saw_pool_alignment = false;
    {
        ExactAlignAllocator eb;
        const ContextAllocator cb(eb.as_sw());
        Reg reg(cb, 3, 1, &owner);
        // Os três pedidos: o maior tamanho é o pool ou a tabela; conferimos pelo cabeçalho dos
        // blocos vivos pelo gancho de teste.
        const void* pool = swarmstream::detail::testing::RequestRegistryTestHook::pool(*reg.r);
        SW_CHECK(ExactAlignAllocator::header_of(pool).alignment == kRequestPoolAlign);
        SW_CHECK(reinterpret_cast<std::uintptr_t>(pool) % kRequestPoolAlign == 0);
        SW_CHECK(ExactAlignAllocator::header_of(pool).size == 3 * sizeof(Request));
        const void* table = swarmstream::detail::testing::RequestRegistryTestHook::table(*reg.r);
        SW_CHECK(ExactAlignAllocator::header_of(table).size == 6 * sizeof(Request*));
        saw_pool_alignment = true;
    }
    SW_CHECK(saw_pool_alignment);

    // Configuração inválida.
    RequestRegistryConfig bad;
    bad.capacity = 0;
    SW_CHECK(RequestRegistry::create(ca, bad).status == RequestStatus::InvalidArgument);
    bad.capacity = 4;
    bad.completion_threads = 0;
    SW_CHECK(RequestRegistry::create(ca, bad).status == RequestStatus::InvalidArgument);
    bad.completion_threads = RequestRegistry::kMaxCompletionThreads + 1;
    SW_CHECK(RequestRegistry::create(ca, bad).status == RequestStatus::InvalidArgument);
    SW_CHECK(ea.live() == 0);
    std::puts("ok: criação (objeto, pool alinhado à linha de cache, tabela 2x) e destruição");
}

void test_callback_exactly_once_in_completion_thread()
{
    const ContextAllocator ca;
    int owner = 2;
    Reg reg(ca, 16, 2, &owner);
    CallbackLog log;
    FileRequests file;
    SW_CHECK(!in_callback());
    sw_request_id ids[5] = {};
    for (auto& id : ids) {
        const SubmitResult s = reg->submit(args_for(file, log), &id);
        SW_CHECK(s.status == RequestStatus::Ok && id != 0);
    }
    SW_CHECK(reg->pending() == 5 && file.pending == 5);
    SW_CHECK(reg->mark_in_flight(ids[1]) == RequestStatus::Ok);
    SW_CHECK(reg->mark_in_flight(ids[1]) == RequestStatus::NotFound); // já EM_VOO
    for (std::size_t i = 0; i < 5; ++i) {
        SW_CHECK(reg->complete(ids[i], SW_OK, 100 + i) == RequestStatus::Ok);
    }
    wait_until_or_fail([&] { return log.count.load() == 5 && reg->pending() == 0; }, 10s,
                       "os 5 callbacks");
    for (std::size_t i = 0; i < 5; ++i) {
        SW_CHECK(log.times(ids[i]) == 1);
        const CallbackLog::Item it = log.find(ids[i]);
        SW_CHECK(it.result == SW_OK && it.bytes == 100 + i);
        SW_CHECK(it.in_cb);                                // marca "dentro de callback"
        SW_CHECK(it.thread != std::this_thread::get_id()); // nunca a thread que criou o pedido
        // Concluir de novo: o pedido não está mais pendente.
        SW_CHECK(reg->complete(ids[i], SW_OK, 0) == RequestStatus::NotFound);
    }
    SW_CHECK(!in_callback());
    SW_CHECK(file.pending == 0 && file.head == nullptr);
    std::this_thread::sleep_for(20ms);
    SW_CHECK(log.count.load() == 5); // nenhum callback a mais
    // Argumentos inválidos (uso interno): sem callback, *out_id = 0.
    SubmitArgs bad = args_for(file, log);
    bad.cb = nullptr;
    sw_request_id id = 77;
    SW_CHECK(reg->submit(bad, &id).status == RequestStatus::InvalidArgument && id == 0);
    bad = args_for(file, log);
    bad.file = nullptr;
    id = 77;
    SW_CHECK(reg->submit(bad, &id).status == RequestStatus::InvalidArgument && id == 0);
    // out_id nulo é permitido.
    SW_CHECK(reg->submit(args_for(file, log), nullptr).status == RequestStatus::Ok);
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    SW_CHECK(log.count.load() == 6);
    std::puts("ok: callback exatamente uma vez, em thread de conclusão, com a marca de callback");
}

void test_queue_full_without_allocating()
{
    const ContextAllocator ca;
    int owner = 3;
    Reg reg(ca, 4, 1, &owner);
    CallbackLog log;
    FileRequests file;
    sw_request_id ids[4] = {};
    const std::uint64_t before = oom::g_total.load();
    for (auto& id : ids) SW_CHECK(reg->submit(args_for(file, log), &id).status == RequestStatus::Ok);
    sw_request_id extra = 99;
    SW_CHECK(reg->submit(args_for(file, log), &extra).status == RequestStatus::QueueFull);
    SW_CHECK(extra == 0);
    SW_CHECK(reg->cancel(&owner, ids[0]) == RequestStatus::Ok);
    SW_CHECK(reg->mark_in_flight(ids[1]) == RequestStatus::Ok);
    SW_CHECK(reg->complete(ids[1], SW_OK, 1) == RequestStatus::Ok);
    const std::uint64_t after = oom::g_total.load();
    SW_CHECK(after == before); // submit, cancel, mark_in_flight, complete: nada alocado
    wait_until_or_fail([&] { return reg->pending() == 2; }, 10s, "dois pedidos livres de novo");
    SW_CHECK(reg->submit(args_for(file, log), &extra).status == RequestStatus::Ok && extra != 0);
    // Controle positivo do contador: uma alocação de verdade é vista.
    const std::uint64_t c0 = oom::g_total.load();
    auto probe = std::make_unique<int>(1);
    SW_CHECK(oom::g_total.load() > c0);
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    SW_CHECK(log.find(ids[0]).result == SW_E_CANCELLED);
    std::puts("ok: pool cheio => QueueFull, *out_id = 0; submit/cancel/complete sem alocar");
}

void test_cancel_not_found_cases()
{
    const ContextAllocator ca;
    int owner = 4;
    int other = 5;
    Reg reg(ca, 8, 1, &owner);
    CallbackLog log;
    FileRequests file;
    SW_CHECK(reg->cancel(&owner, 0) == RequestStatus::NotFound);
    SW_CHECK(reg->cancel(&owner, UINT64_MAX) == RequestStatus::NotFound);
    SW_CHECK(reg->cancel(&owner, swarmstream::detail::issue_request_id()) == RequestStatus::NotFound);
    sw_request_id id = 0;
    SW_CHECK(reg->submit(args_for(file, log), &id).status == RequestStatus::Ok);
    SW_CHECK(reg->cancel(&other, id) == RequestStatus::NotFound); // barreira do dono
    SW_CHECK(reg->complete(id, SW_OK, 5) == RequestStatus::Ok);
    wait_until_or_fail([&] { return log.count.load() == 1; }, 10s, "o callback");
    SW_CHECK(reg->cancel(&owner, id) == RequestStatus::NotFound); // já concluído
    SW_CHECK(log.find(id).result == SW_OK);
    // Cancelar pendente: Ok, o callback vem com SW_E_CANCELLED; cancelar de novo: NotFound; a
    // conclusão tardia da fonte é descartada.
    SW_CHECK(reg->submit(args_for(file, log), &id).status == RequestStatus::Ok);
    SW_CHECK(reg->mark_in_flight(id) == RequestStatus::Ok);
    SW_CHECK(reg->cancel(&owner, id) == RequestStatus::Ok);
    SW_CHECK(reg->cancel(&owner, id) == RequestStatus::NotFound);
    SW_CHECK(reg->complete(id, SW_OK, 9) == RequestStatus::NotFound);
    wait_until_or_fail([&] { return log.count.load() == 2; }, 10s, "o callback cancelado");
    SW_CHECK(log.times(id) == 1 && log.find(id).result == SW_E_CANCELLED);
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    std::puts("ok: cancel => NotFound para 0, UINT64_MAX, nunca emitido, concluído e outro dono");
}

// §1.5 teste 1: dois registros (contextos), milhares de pedidos intercalados; cancelar em A o id de
// B => NotFound, e o pedido de B conclui normalmente.
void test_cross_context_cancel()
{
    const ContextAllocator ca;
    int owner_a = 10;
    int owner_b = 11;
    Reg a(ca, 4096, 2, &owner_a);
    Reg b(ca, 4096, 2, &owner_b);
    CallbackLog log_a;
    CallbackLog log_b;
    FileRequests fa;
    FileRequests fb;
    constexpr int kN = 3000;
    std::vector<sw_request_id> ids_a(kN);
    std::vector<sw_request_id> ids_b(kN);
    for (int i = 0; i < kN; ++i) {
        SW_CHECK(a->submit(args_for(fa, log_a), &ids_a[i]).status == RequestStatus::Ok);
        SW_CHECK(b->submit(args_for(fb, log_b), &ids_b[i]).status == RequestStatus::Ok);
    }
    for (int i = 0; i < kN; ++i) {
        SW_CHECK(ids_a[i] != ids_b[i]);
        SW_CHECK(a->cancel(&owner_a, ids_b[i]) == RequestStatus::NotFound); // id de B em A
        SW_CHECK(a->cancel(&owner_b, ids_a[i]) == RequestStatus::NotFound); // dono errado
        SW_CHECK(a->complete(ids_b[i], SW_E_IO, 0) == RequestStatus::NotFound);
    }
    for (int i = 0; i < kN; ++i) {
        SW_CHECK(b->complete(ids_b[i], SW_OK, static_cast<std::uint64_t>(i)) == RequestStatus::Ok);
        SW_CHECK(a->complete(ids_a[i], SW_OK, 1) == RequestStatus::Ok);
    }
    wait_until_or_fail([&] { return log_a.count.load() == kN && log_b.count.load() == kN; }, 30s,
                       "os callbacks dos dois contextos");
    for (int i = 0; i < kN; i += 97) {
        SW_CHECK(log_b.times(ids_b[i]) == 1);
        const CallbackLog::Item it = log_b.find(ids_b[i]);
        SW_CHECK(it.result == SW_OK && it.bytes == static_cast<std::uint64_t>(i));
    }
    SW_CHECK(a->pending() == 0 && b->pending() == 0);
    std::puts("ok: (1) cancel(A, idDeB) => NotFound e o pedido de B conclui (3000 intercalados)");
}

// §1.5 teste 2: 16 threads sobre 2 registros; todos os ids distintos e != 0.
void test_ids_unique_sixteen_threads()
{
    const ContextAllocator ca;
    int owner_a = 20;
    int owner_b = 21;
    Reg a(ca, 4096, 2, &owner_a);
    Reg b(ca, 4096, 2, &owner_b);
    CallbackLog log;
    constexpr int kThreads = 16;
    constexpr int kPer = 400;
    std::vector<std::vector<sw_request_id>> got(kThreads, std::vector<sw_request_id>(kPer));
    std::vector<FileRequests> files(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            RequestRegistry* r = (t % 2 == 0) ? a.r : b.r;
            for (int i = 0; i < kPer; ++i) {
                sw_request_id id = 0;
                SubmitResult s = r->submit(args_for(files[t], log), &id);
                while (s.status == RequestStatus::QueueFull) {
                    std::this_thread::yield();
                    s = r->submit(args_for(files[t], log), &id);
                }
                SW_CHECK(s.status == RequestStatus::Ok && id != 0);
                got[t][i] = id;
                SW_CHECK(r->complete(id, SW_OK, 0) == RequestStatus::Ok);
            }
        });
    }
    for (auto& th : threads) th.join();
    std::vector<sw_request_id> all;
    all.reserve(kThreads * kPer);
    for (const auto& v : got) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    SW_CHECK(all.front() != 0);
    SW_CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    wait_until_or_fail([&] { return log.count.load() == kThreads * kPer; }, 30s, "callbacks");
    for (int t = 0; t < kThreads; ++t) {
        RequestRegistry* r = (t % 2 == 0) ? a.r : b.r;
        SW_CHECK(r->close_file(files[t]) == RequestStatus::Ok);
    }
    std::printf("ok: (2) %d ids de 16 threads em 2 contextos: distintos e != 0\n", kThreads * kPer);
}

// Protocolo de fechamento (F1-60 §1.7), com pedidos sintéticos.
struct SlowCb
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    sw_result got = SW_OK;
    // O que o callback faz enquanto o close_file espera (teste (f)).
    RequestRegistry* reg = nullptr;
    FileRequests* file = nullptr;
    CallbackLog* log = nullptr;
    RequestStatus submit_from_cb = RequestStatus::Ok;
    sw_request_id submit_from_cb_id = 1;
    RequestStatus close_from_cb = RequestStatus::Ok;
    RequestStatus stop_from_cb = RequestStatus::Ok;
    bool in_cb = false;

    static void SW_CALL cb(void* user, sw_request_id, sw_result result, std::uint64_t)
    {
        auto* self = static_cast<SlowCb*>(user);
        self->got = result;
        self->in_cb = in_callback();
        self->entered.store(true);
        while (!self->release.load()) std::this_thread::sleep_for(1ms);
        if (self->reg != nullptr) {
            // (f) chamada vinda de callback durante o fechamento do mesmo arquivo.
            self->submit_from_cb =
                self->reg->submit(args_for(*self->file, *self->log), &self->submit_from_cb_id).status;
            self->close_from_cb = self->reg->close_file(*self->file);
            self->stop_from_cb = self->reg->stop();
        }
        self->calls.fetch_add(1);
    }
};

void test_close_file_protocol()
{
    const ContextAllocator ca;
    int owner = 30;
    Reg reg(ca, 64, 2, &owner);
    CallbackLog log;
    FileRequests file;
    FileRequests other_file;
    // 3 enfileirados, 1 "preso" EM_VOO (a fonte nunca o conclui), 1 já concluindo com callback lento.
    sw_request_id q[3] = {};
    for (auto& id : q) SW_CHECK(reg->submit(args_for(file, log), &id).status == RequestStatus::Ok);
    sw_request_id stuck = 0;
    SW_CHECK(reg->submit(args_for(file, log), &stuck).status == RequestStatus::Ok);
    SW_CHECK(reg->mark_in_flight(stuck) == RequestStatus::Ok);
    SlowCb slow;
    slow.reg = reg.r;
    slow.file = &file;
    slow.log = &log;
    SubmitArgs sa = args_for(file, log);
    sa.cb = &SlowCb::cb;
    sa.user = &slow;
    sw_request_id slow_id = 0;
    SW_CHECK(reg->submit(sa, &slow_id).status == RequestStatus::Ok);
    SW_CHECK(reg->complete(slow_id, SW_OK, 42) == RequestStatus::Ok);
    wait_or_fail(slow.entered, 10s, "o callback lento começar");
    // Um pedido de OUTRO arquivo não é afetado.
    sw_request_id other_id = 0;
    SW_CHECK(reg->submit(args_for(other_file, log), &other_id).status == RequestStatus::Ok);

    std::atomic<bool> close_done{false};
    RequestStatus close_status = RequestStatus::Internal;
    std::thread closer([&] {
        close_status = reg->close_file(file);
        close_done.store(true);
    });
    // O fechamento cancela os pendentes sem esperar a fonte (o preso não segura), mas espera o
    // callback lento: enquanto ele não volta, close_file não volta.
    wait_until_or_fail([&] { return log.count.load() >= 4; }, 10s, "os 4 cancelados");
    std::this_thread::sleep_for(30ms);
    SW_CHECK(!close_done.load());
    // A partir do início do fechamento, pedido novo no arquivo => Cancelled na hora, sem callback.
    sw_request_id late = 5;
    SW_CHECK(reg->submit(args_for(file, log), &late).status == RequestStatus::Cancelled);
    SW_CHECK(late == 0);
    slow.release.store(true);
    wait_or_fail(close_done, 10s, "close_file voltar");
    closer.join();
    SW_CHECK(close_status == RequestStatus::Ok);
    // Cada pendente recebeu SW_E_CANCELLED; o que já concluía recebeu o resultado.
    for (sw_request_id id : q) SW_CHECK(log.times(id) == 1 && log.find(id).result == SW_E_CANCELLED);
    SW_CHECK(log.times(stuck) == 1 && log.find(stuck).result == SW_E_CANCELLED);
    SW_CHECK(slow.calls.load() == 1 && slow.got == SW_OK && slow.in_cb);
    // (f) de dentro de callback, durante o fechamento: submit no arquivo => Cancelled (sem
    // callback); close_file e stop => Busy (nada muda).
    SW_CHECK(slow.submit_from_cb == RequestStatus::Cancelled && slow.submit_from_cb_id == 0);
    SW_CHECK(slow.close_from_cb == RequestStatus::Busy);
    SW_CHECK(slow.stop_from_cb == RequestStatus::Busy);
    SW_CHECK(file.pending == 0 && file.head == nullptr);
    // Nenhum callback depois do retorno; a conclusão tardia do preso é descartada.
    const std::uint64_t n = log.count.load();
    SW_CHECK(reg->complete(stuck, SW_OK, 1) == RequestStatus::NotFound);
    std::this_thread::sleep_for(30ms);
    SW_CHECK(log.count.load() == n);
    // O outro arquivo segue: conclui normalmente.
    SW_CHECK(reg->complete(other_id, SW_OK, 3) == RequestStatus::Ok);
    wait_until_or_fail([&] { return log.times(other_id) == 1; }, 10s, "o pedido do outro arquivo");
    SW_CHECK(log.find(other_id).result == SW_OK);
    // Fechar de novo um arquivo já fechado: Ok (nada pendente); pedidos novos continuam recusados.
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    SW_CHECK(reg->submit(args_for(file, log), &late).status == RequestStatus::Cancelled);
    SW_CHECK(reg->close_file(other_file) == RequestStatus::Ok);
    std::puts("ok: close_file cancela sem esperar a fonte, espera os callbacks, recusa novos (f)");
}

void test_stop()
{
    const ContextAllocator ca;
    int owner = 40;
    RequestRegistryConfig cfg;
    cfg.capacity = 8;
    cfg.completion_threads = 3;
    cfg.owner = &owner;
    RequestRegistryCreate c = RequestRegistry::create(ca, cfg);
    SW_CHECK(c.status == RequestStatus::Ok);
    RequestRegistry* reg = c.registry;
    CallbackLog log;
    FileRequests file;
    sw_request_id id = 0;
    SW_CHECK(reg->submit(args_for(file, log), &id).status == RequestStatus::Ok);
    SW_CHECK(reg->complete(id, SW_OK, 0) == RequestStatus::Ok);
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    SW_CHECK(log.count.load() == 1);
    SW_CHECK(reg->stop() == RequestStatus::Ok);
    SW_CHECK(reg->stop() == RequestStatus::Ok); // idempotente
    FileRequests f2;
    id = 9;
    SW_CHECK(reg->submit(args_for(f2, log), &id).status == RequestStatus::ShuttingDown && id == 0);
    SW_CHECK(RequestRegistry::destroy(reg) == RequestStatus::Ok);
    SW_CHECK(RequestRegistry::destroy(nullptr) == RequestStatus::Ok);
    std::puts("ok: stop recusa pedidos novos (ShuttingDown), é idempotente; destroy depois");
}

void test_file_leases()
{
    struct Releaser final : swarmstream::ILeaseReleaser
    {
        std::atomic<int> released{0};
        void release_lease(swarmstream::SlotId, const swarmstream::PieceId&) noexcept override
        {
            released.fetch_add(1);
        }
    };
    auto rel = std::make_shared<Releaser>();
    static const std::uint8_t data[4] = {1, 2, 3, 4};
    ExactAlignAllocator ea;
    const ContextAllocator ca(ea.as_sw());
    FileLeases leases;
    for (int i = 0; i < 5; ++i) {
        swarmstream::Lease l(data, sizeof(data), static_cast<swarmstream::SlotId>(i),
                             swarmstream::PieceId{}, rel);
        SW_CHECK(leases.add(ca, std::move(l)) == AllocStatus::Ok);
        SW_CHECK(!l.valid()); // foi movido para o nó
    }
    SW_CHECK(leases.size() == 5 && ea.live() == 5 && rel->released.load() == 0);
    // Falha do alocador: o lease fica com o chamador, nada muda.
    ea.fail_at(1);
    swarmstream::Lease keep(data, sizeof(data), 9, swarmstream::PieceId{}, rel);
    SW_CHECK(leases.add(ca, std::move(keep)) == AllocStatus::OutOfMemory);
    SW_CHECK(keep.valid() && leases.size() == 5);
    ea.fail_at(0);
    leases.release_all(ca);
    SW_CHECK(rel->released.load() == 5); // a peça deixou de estar presa
    SW_CHECK(leases.size() == 0 && ea.live() == 0);
    leases.release_all(ca); // idempotente
    keep.reset();
    SW_CHECK(rel->released.load() == 6);
    std::puts("ok: leases por arquivo soltos no fechamento; nós devolvidos ao alocador");
}

// O header: "*out_id é escrito antes disso" (antes de o callback poder rodar). Uma fonte que conclui
// QUALQUER pedido enfileirado assim que ele aparece; o callback confere que a variável de saída do
// chamador (o user) já tem o id. Sob TSan, escrever *out_id depois de soltar o mutex seria corrida.
struct OutIdProbe
{
    sw_request_id out = 0;
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};
    static void SW_CALL cb(void* user, sw_request_id id, sw_result, std::uint64_t)
    {
        auto* self = static_cast<OutIdProbe*>(user);
        if (self->out == id) {
            self->ok.fetch_add(1);
        } else {
            self->bad.fetch_add(1);
        }
    }
};

void test_out_id_written_before_completion()
{
    const ContextAllocator ca;
    int owner = 80;
    Reg reg(ca, 64, 2, &owner);
    FileRequests file;
    std::atomic<bool> stop_source{false};
    std::thread source([&] {
        while (!stop_source.load()) {
            (void)swarmstream::detail::testing::RequestRegistryTestHook::complete_first_queued(*reg.r,
                                                                                           SW_OK);
        }
    });
    constexpr int kN = 2000;
    for (int i = 0; i < kN; ++i) {
        // Um pedido por vez (o probe guarda um id): espera o callback antes do próximo.
        auto probe = std::make_unique<OutIdProbe>();
        SubmitArgs a;
        a.file = &file;
        a.cb = &OutIdProbe::cb;
        a.user = probe.get();
        SW_CHECK(reg->submit(a, &probe->out).status == RequestStatus::Ok);
        wait_until_or_fail([&] { return probe->ok.load() + probe->bad.load() == 1; }, 10s,
                           "o callback do probe");
        SW_CHECK(probe->bad.load() == 0);
        wait_until_or_fail([&] { return reg->pending() == 0; }, 10s, "o pedido voltar ao pool");
    }
    stop_source.store(true);
    source.join();
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    std::printf("ok: *out_id escrito antes do callback (%d pedidos com uma fonte que conclui na hora)\n",
                kN);
}

// stop com pedidos pendentes que ninguém concluiu: cada um recebe SW_E_CANCELLED exatamente uma vez
// e o stop não trava (destroy faz o stop).
void test_stop_with_pending_cancels_them()
{
    const ContextAllocator ca;
    int owner = 81;
    RequestRegistryConfig cfg;
    cfg.capacity = 8;
    cfg.completion_threads = 2;
    cfg.owner = &owner;
    RequestRegistryCreate c = RequestRegistry::create(ca, cfg);
    SW_CHECK(c.status == RequestStatus::Ok);
    CallbackLog log;
    FileRequests file;
    sw_request_id a = 0;
    sw_request_id b = 0;
    SW_CHECK(c.registry->submit(args_for(file, log), &a).status == RequestStatus::Ok);
    SW_CHECK(c.registry->submit(args_for(file, log), &b).status == RequestStatus::Ok);
    SW_CHECK(c.registry->mark_in_flight(b) == RequestStatus::Ok);
    SW_CHECK(RequestRegistry::destroy(c.registry) == RequestStatus::Ok);
    SW_CHECK(log.count.load() == 2);
    SW_CHECK(log.times(a) == 1 && log.find(a).result == SW_E_CANCELLED);
    SW_CHECK(log.times(b) == 1 && log.find(b).result == SW_E_CANCELLED);
    std::puts("ok: stop/destroy com pendentes: cada um recebe SW_E_CANCELLED uma vez, sem travar");
}

// Falha do alloc_fn em cada ponto da criação: nada vivo, status OutOfMemory.
void test_create_alloc_fn_failure_each_point()
{
    int owner = 50;
    RequestRegistryConfig cfg;
    cfg.capacity = 32;
    cfg.completion_threads = 2;
    cfg.owner = &owner;
    std::size_t points = 0;
    for (std::int64_t k = 1;; ++k) {
        ExactAlignAllocator ea;
        const ContextAllocator ca(ea.as_sw());
        ea.fail_at(k);
        const RequestRegistryCreate c = RequestRegistry::create(ca, cfg);
        if (ea.failed_calls() == 0) {
            SW_CHECK(c.status == RequestStatus::Ok);
            SW_CHECK(RequestRegistry::destroy(c.registry) == RequestStatus::Ok);
            SW_CHECK(ea.live() == 0);
            break;
        }
        ++points;
        SW_CHECK(c.status == RequestStatus::OutOfMemory && c.registry == nullptr);
        SW_CHECK(c.alloc == AllocStatus::OutOfMemory);
        SW_CHECK(ea.live() == 0);
    }
    SW_CHECK(points == 3); // objeto, pool, tabela
    std::printf("[F1-67B] cenario='registro_pedidos_alloc_fn' pontos_de_falha=%zu\n", points);
    std::puts("ok: falha do alloc_fn em cada um dos 3 pontos da criação => OutOfMemory, vivos == 0");
}

// Varredura de operator new na criação com o alocador do SISTEMA (inclui o estado das threads de
// conclusão), sem pontos excluídos: toda falha => OutOfMemory, nada vivo a mais, nenhuma thread.
void test_create_operator_new_sweep()
{
    oom::g_progress_scenario.store("registro_pedidos_create");
    int owner = 60;
    RequestRegistryConfig cfg;
    cfg.capacity = 16;
    cfg.completion_threads = 4;
    cfg.owner = &owner;
    const ContextAllocator sys;
    std::size_t fired_points = 0;
    std::int64_t ok_at = 0;
    for (std::int64_t k = 1;; ++k) {
        const std::int64_t live_before = oom::g_live.load();
        oom::g_progress_k.store(k);
        oom::arm(k);
        const RequestRegistryCreate c = RequestRegistry::create(sys, cfg);
        const bool fired = oom::g_fired.load();
        oom::disarm();
        if (!fired) {
            SW_CHECK(c.status == RequestStatus::Ok);
            SW_CHECK(RequestRegistry::destroy(c.registry) == RequestStatus::Ok);
            SW_CHECK(oom::g_live.load() == live_before);
            ok_at = k;
            break;
        }
        ++fired_points;
        if (c.status != RequestStatus::OutOfMemory) {
            std::fprintf(stderr, "CODIGO ERRADO no ponto K=%lld: %s\n", static_cast<long long>(k),
                         swarmstream::detail::to_string(c.status));
        }
        SW_CHECK(c.status == RequestStatus::OutOfMemory && c.registry == nullptr);
        SW_CHECK(oom::g_live.load() == live_before);
    }
    // Pontos: objeto, pool, tabela e o estado de cada uma das 4 threads (>= 7 em qualquer STL).
    SW_CHECK(fired_points >= 7);
    std::printf("[F1-67B] cenario='registro_pedidos_create' pontos_de_falha=%zu "
                "create_completo_na_alocacao=%lld\n",
                fired_points, static_cast<long long>(ok_at));
    std::printf("ok: varredura de operator new na criação: %zu pontos, todos OutOfMemory\n",
                fired_points);
}

// Thread de conclusão que o sistema não cria (std::system_error) => Internal, tudo desfeito.
void test_thread_start_failure_is_internal()
{
    int owner = 70;
    RequestRegistryConfig cfg;
    cfg.capacity = 8;
    cfg.completion_threads = 3;
    cfg.owner = &owner;
    for (std::uint32_t nth = 1; nth <= 3; ++nth) {
        ExactAlignAllocator ea;
        const ContextAllocator ca(ea.as_sw());
        swarmstream::detail::testing::RequestRegistryTestHook::fail_thread_start(nth);
        const RequestRegistryCreate c = RequestRegistry::create(ca, cfg);
        SW_CHECK(c.status == RequestStatus::Internal && c.registry == nullptr);
        SW_CHECK(ea.live() == 0);
    }
    swarmstream::detail::testing::RequestRegistryTestHook::fail_thread_start(0);
    std::puts("ok: thread de conclusão que o sistema não cria => Internal, tudo desfeito");
}

void print_stl_facts()
{
    std::printf("[F1-67B] STL:");
#if defined(_MSC_FULL_VER)
    std::printf(" _MSC_FULL_VER=%d", _MSC_FULL_VER);
#endif
#if defined(__GNUC__) && !defined(__clang__)
    std::printf(" __GNUC__=%d", __GNUC__);
#endif
#if defined(__clang__)
    std::printf(" __clang_major__=%d", __clang_major__);
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    std::printf(" _ITERATOR_DEBUG_LEVEL=%d", _ITERATOR_DEBUG_LEVEL);
#endif
#if defined(NDEBUG)
    std::printf(" NDEBUG=1\n");
#else
    std::printf(" NDEBUG=0\n");
#endif
}

} // namespace

int main()
{
    oom::make_abort_fail_instead_of_hang();
    oom::warm_up_exceptions();
    print_stl_facts();
    test_create_layout_and_destroy();
    test_callback_exactly_once_in_completion_thread();
    test_queue_full_without_allocating();
    test_cancel_not_found_cases();
    test_cross_context_cancel();
    test_ids_unique_sixteen_threads();
    test_close_file_protocol();
    test_stop();
    test_file_leases();
    test_out_id_written_before_completion();
    test_stop_with_pending_cancels_them();
    test_create_alloc_fn_failure_each_point();
    test_create_operator_new_sweep();
    test_thread_start_failure_is_internal();
    std::puts("TODOS OS TESTES DO REGISTRO DE PEDIDOS (F1-67 B) PASSARAM");
    return 0;
}
