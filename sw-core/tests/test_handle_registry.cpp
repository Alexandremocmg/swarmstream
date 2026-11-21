// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-67 parte B: registro global de handles, pin/FECHANDO e quarentena de cascas, no nível do
// componente (F1-60 §1.3 e §1.4). A prova pela API pública (sw_close, sw_file_size, sw_dir_next)
// é da parte D; a do sw_ctx no sw_shutdown, da parte C.
//
// Cobre: busca sem ler o endereço (NULL, nunca emitido, interior, tipo errado); estados VIVO /
// FECHANDO / ausente distinguíveis (testes (a) e (b)); sw_close com pin de outra thread e progresso
// da MESMA fatia durante a espera (c); fechador sem esperar o próprio uso (d); dois fechadores (e);
// desfazer o fechamento (sw_shutdown que acha handle aberto); quarentena 256/64 e o LIMITE
// DOCUMENTADO (testes 2 e 3 do §1.4); a tabela devolve a memória ao esvaziar (6); busca e pin sem
// alocar, com controle positivo (7); falta de memória na inserção, varredura sem pontos excluídos
// (resumo [F1-67B] para o log do CI); 16 threads (5; o job TSan roda este teste).
//
// O operator new deste executável é o de oom_support.hpp (contador e falha injetada).

#include "check.hpp"
#include "handle_registry.hpp"
#include "handle_registry_testing.hpp"
#include "oom_support.hpp"
#include "test_allocators.hpp"
#include "api_allocator.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using swarmstream::detail::ContextAllocator;
using swarmstream::detail::HandleKind;
using swarmstream::detail::HandleLookup;
using swarmstream::detail::HandleRegistry;
using swarmstream::detail::HandleStatus;
using swarmstream::detail::kCtxQuarantine;
using swarmstream::detail::kFileQuarantine;
using swarmstream::detail::PinCore;
using swarmstream::detail::ProcessCtxQuarantine;
using swarmstream::detail::QuarantinedShell;
using swarmstream::detail::ShellQuarantine;

// Cão de guarda: se `flag` não virar true em `limit`, o teste falha (em vez de travar o CI).
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

// Endereços de teste: um vetor de cascas (nunca liberado durante o teste).
struct Shells
{
    explicit Shells(std::size_t n) : files(n) {}
    std::vector<sw_file> files;
    const void* at(std::size_t i) const { return &files[i]; }
};

// Um endereço em `shard` diferente de `avoid` e de `avoid2`, tirado de `pool`.
std::size_t find_same_shard(const Shells& pool, std::size_t shard, std::size_t avoid,
                            std::size_t avoid2 = static_cast<std::size_t>(-1))
{
    for (std::size_t i = 0; i < pool.files.size(); ++i) {
        if (i != avoid && i != avoid2 && HandleRegistry::shard_of(pool.at(i)) == shard) return i;
    }
    SW_CHECK(false);
    return 0;
}

void test_basic_states()
{
    HandleRegistry reg;
    Shells s(4);
    PinCore core;
    int owner = 7;
    SW_CHECK(reg.size() == 0 && reg.table_bytes() == 0);
    SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, &core) == HandleStatus::Ok);
    SW_CHECK(reg.size() == 1 && reg.table_bytes() > 0);
    SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, &core) == HandleStatus::Duplicate);
    // Erros de uso: nada é registrado.
    SW_CHECK(reg.insert(nullptr, HandleKind::File, &owner, &core) == HandleStatus::Unknown);
    SW_CHECK(reg.insert(s.at(1), HandleKind::None, &owner, &core) == HandleStatus::Unknown);
    SW_CHECK(reg.insert(s.at(1), HandleKind::File, &owner, nullptr) == HandleStatus::Unknown);
    SW_CHECK(reg.size() == 1);

    // VIVO: pin, dono e core vêm DA ENTRADA.
    HandleLookup l = reg.acquire(s.at(0), HandleKind::File);
    SW_CHECK(l.status == HandleStatus::Ok && l.owner == &owner && l.core == &core);
    SW_CHECK(core.pins() == 1);
    l.core->unpin();
    SW_CHECK(core.pins() == 0);

    // Ausentes: NULL, nunca emitido, endereço interior, ponteiro que não é handle.
    SW_CHECK(reg.acquire(nullptr, HandleKind::File).status == HandleStatus::Unknown);
    SW_CHECK(reg.acquire(s.at(1), HandleKind::File).status == HandleStatus::Unknown);
    const auto* interior = reinterpret_cast<const unsigned char*>(s.at(0)) + 8;
    SW_CHECK(reg.acquire(interior, HandleKind::File).status == HandleStatus::Unknown);
    SW_CHECK(reg.acquire(&owner, HandleKind::File).status == HandleStatus::Unknown);
    // Tipo errado: arquivo no lugar de diretório e de contexto.
    SW_CHECK(reg.acquire(s.at(0), HandleKind::Dir).status == HandleStatus::WrongKind);
    SW_CHECK(reg.acquire(s.at(0), HandleKind::Context).status == HandleStatus::WrongKind);
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::Dir).status == HandleStatus::WrongKind);
    SW_CHECK(core.pins() == 0); // nenhum pin tomado nos erros
    // remove de entrada VIVA é uso incorreto e não muda nada.
    SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::NotClosing);
    SW_CHECK(reg.cancel_close(s.at(0), HandleKind::File) == HandleStatus::NotClosing);

    // (a) FECHANDO: pedido novo/uso => Closing (o mapeamento dá SW_E_CANCELLED), sem pin.
    l = reg.begin_close(s.at(0), HandleKind::File);
    SW_CHECK(l.status == HandleStatus::Ok && l.owner == &owner && l.core == &core);
    SW_CHECK(core.closing());
    HandleLookup c = reg.acquire(s.at(0), HandleKind::File);
    SW_CHECK(c.status == HandleStatus::Closing && core.pins() == 0);
    // (e) segundo fechador: perde (Closing), nada corrompe.
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::File).status == HandleStatus::Closing);
    core.wait_unpinned(); // (d) sem pins de outros: volta na hora (o fechador não tem pin)
    SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::Ok);
    // (b) depois de remover: ausente (distinguível de (a)).
    SW_CHECK(reg.acquire(s.at(0), HandleKind::File).status == HandleStatus::Unknown);
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::File).status == HandleStatus::Unknown);
    SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::Unknown);
    SW_CHECK(reg.size() == 0);
    SW_CHECK(reg.table_bytes() == 0); // (6) a tabela voltou quando a fatia esvaziou
    std::puts("ok: VIVO/FECHANDO/ausente, tipo errado, interior, NULL, dois fechadores (a, b, d, e)");
}

void test_cancel_close_restores_live()
{
    HandleRegistry reg;
    Shells s(1);
    PinCore core;
    int owner = 1;
    SW_CHECK(reg.insert(s.at(0), HandleKind::Dir, &owner, &core) == HandleStatus::Ok);
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::Dir).status == HandleStatus::Ok);
    SW_CHECK(reg.acquire(s.at(0), HandleKind::Dir).status == HandleStatus::Closing);
    SW_CHECK(reg.cancel_close(s.at(0), HandleKind::Dir) == HandleStatus::Ok);
    SW_CHECK(!core.closing());
    HandleLookup l = reg.acquire(s.at(0), HandleKind::Dir);
    SW_CHECK(l.status == HandleStatus::Ok && core.pins() == 1);
    l.core->unpin();
    // Pode fechar de novo depois.
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::Dir).status == HandleStatus::Ok);
    core.wait_unpinned();
    SW_CHECK(reg.remove(s.at(0), HandleKind::Dir) == HandleStatus::Ok);
    std::puts("ok: desfazer o fechamento (FECHANDO -> VIVO), como no sw_shutdown com handle aberto");
}

// (c) sw_close com um pin segurado por outra thread: só volta depois do unpin, e outras buscas na
// MESMA fatia progridem durante a espera (prova que nenhum mutex de fatia fica segurado).
void test_close_waits_other_pin_without_holding_shard()
{
    HandleRegistry reg;
    Shells s(4096);
    PinCore core_a;
    PinCore core_b;
    int owner = 3;
    const std::size_t a = 0;
    const std::size_t b = find_same_shard(s, HandleRegistry::shard_of(s.at(a)), a);
    const std::size_t extra = find_same_shard(s, HandleRegistry::shard_of(s.at(a)), a, b);
    SW_CHECK(reg.insert(s.at(a), HandleKind::File, &owner, &core_a) == HandleStatus::Ok);
    SW_CHECK(reg.insert(s.at(b), HandleKind::File, &owner, &core_b) == HandleStatus::Ok);

    // A "outra thread" segura um pin de A.
    HandleLookup held = reg.acquire(s.at(a), HandleKind::File);
    SW_CHECK(held.status == HandleStatus::Ok);

    std::atomic<bool> closer_started{false};
    std::atomic<bool> closer_done{false};
    std::thread closer([&] {
        HandleLookup l = reg.begin_close(s.at(a), HandleKind::File);
        SW_CHECK(l.status == HandleStatus::Ok);
        closer_started.store(true);
        l.core->wait_unpinned();
        closer_done.store(true);
    });
    wait_or_fail(closer_started, 10s, "o fechador marcar FECHANDO");
    // Enquanto o fechador espera, a mesma fatia responde: busca/pin de B, inserção e remoção de outro.
    PinCore core_extra;
    for (int i = 0; i < 1000; ++i) {
        HandleLookup l = reg.acquire(s.at(b), HandleKind::File);
        SW_CHECK(l.status == HandleStatus::Ok);
        l.core->unpin();
    }
    SW_CHECK(reg.insert(s.at(extra), HandleKind::File, &owner, &core_extra) == HandleStatus::Ok);
    SW_CHECK(reg.begin_close(s.at(extra), HandleKind::File).status == HandleStatus::Ok);
    core_extra.wait_unpinned();
    SW_CHECK(reg.remove(s.at(extra), HandleKind::File) == HandleStatus::Ok);
    std::this_thread::sleep_for(50ms);
    SW_CHECK(!closer_done.load()); // ainda espera o pin de A
    held.core->unpin();
    wait_or_fail(closer_done, 10s, "o fechador voltar depois do unpin");
    closer.join();
    SW_CHECK(reg.remove(s.at(a), HandleKind::File) == HandleStatus::Ok);
    SW_CHECK(reg.begin_close(s.at(b), HandleKind::File).status == HandleStatus::Ok);
    core_b.wait_unpinned();
    SW_CHECK(reg.remove(s.at(b), HandleKind::File) == HandleStatus::Ok);
    SW_CHECK(reg.size() == 0 && reg.table_bytes() == 0);
    std::puts("ok: (c) fechador espera o pin de outra thread sem segurar a fatia (mesma fatia anda)");
}

// Vários pins soltos um a um: o fechador só volta no último (o despertar não se perde).
void test_many_pins_released_one_by_one()
{
    HandleRegistry reg;
    Shells s(1);
    PinCore core;
    int owner = 4;
    SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, &core) == HandleStatus::Ok);
    constexpr int kPins = 8;
    for (int i = 0; i < kPins; ++i) SW_CHECK(reg.acquire(s.at(0), HandleKind::File).status == HandleStatus::Ok);
    SW_CHECK(core.pins() == kPins);
    std::atomic<bool> done{false};
    std::thread closer([&] {
        HandleLookup l = reg.begin_close(s.at(0), HandleKind::File);
        SW_CHECK(l.status == HandleStatus::Ok);
        l.core->wait_unpinned();
        done.store(true);
    });
    std::vector<std::thread> releasers;
    for (int i = 0; i < kPins; ++i) {
        std::this_thread::sleep_for(5ms);
        SW_CHECK(!done.load());
        releasers.emplace_back([&core] { core.unpin(); });
    }
    for (auto& t : releasers) t.join();
    wait_or_fail(done, 10s, "o fechador voltar depois do último unpin");
    closer.join();
    SW_CHECK(core.pins() == 0);
    SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::Ok);
    std::puts("ok: oito pins soltos de threads diferentes; o fechador só volta no último");
}

// O fechador LIBERA o PinCore assim que wait_unpinned volta (como o sw_close faz com o corpo do
// arquivo), com vários pins soltos pelo caminho lento ao mesmo tempo. Se um unpin tocasse no objeto
// depois de o fechador poder vê-lo em 0 (por exemplo, decrementar fora do mutex e só depois travá-lo
// para acordar), isto seria uso depois da liberação: o ASan e o TSan do CI o veem (por amostragem;
// sem sanitizer, só se a memória reescrita quebrar o mutex). A memória liberada é reescrita com 0xAB
// para aumentar a chance de o defeito aparecer também sem sanitizer.
void test_closer_frees_core_right_after_wait()
{
    HandleRegistry reg;
    Shells s(1);
    int owner = 12;
    constexpr int kRounds = 400;
    constexpr int kPins = 6;
    for (int round = 0; round < kRounds; ++round) {
        auto* storage = static_cast<unsigned char*>(::operator new(sizeof(PinCore)));
        auto* core = ::new (storage) PinCore();
        SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, core) == HandleStatus::Ok);
        for (int i = 0; i < kPins; ++i) SW_CHECK(reg.acquire(s.at(0), HandleKind::File).status == HandleStatus::Ok);
        std::atomic<int> go{0};
        std::vector<std::thread> unpinners;
        for (int i = 0; i < kPins; ++i) {
            unpinners.emplace_back([core, &go] {
                while (go.load() == 0) std::this_thread::yield();
                core->unpin(); // caminho lento: o fechador já ligou FECHANDO
            });
        }
        HandleLookup l = reg.begin_close(s.at(0), HandleKind::File);
        SW_CHECK(l.status == HandleStatus::Ok);
        go.store(1);
        l.core->wait_unpinned();
        SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::Ok);
        core->~PinCore();
        std::memset(storage, 0xAB, sizeof(PinCore));
        ::operator delete(storage);
        for (auto& t : unpinners) t.join();
    }
    std::printf("ok: fechador libera o PinCore logo depois da espera (%d rodadas x %d pins)\n", kRounds,
                kPins);
}

// (7) Busca, pin, unpin, begin_close, cancel_close e remove NÃO alocam (contador de operator new);
// controle positivo: a inserção numa fatia vazia aloca a tabela.
void test_lookup_and_pin_do_not_allocate()
{
    HandleRegistry reg;
    Shells s(64);
    PinCore cores[64];
    int owner = 5;
    const std::uint64_t before_insert = oom::g_total.load();
    SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, &cores[0]) == HandleStatus::Ok);
    SW_CHECK(oom::g_total.load() > before_insert); // controle positivo: o contador vê a tabela
    for (std::size_t i = 1; i < 64; ++i) {
        SW_CHECK(reg.insert(s.at(i), HandleKind::File, &owner, &cores[i]) == HandleStatus::Ok);
    }
    const std::uint64_t before = oom::g_total.load();
    for (int round = 0; round < 200; ++round) {
        for (std::size_t i = 0; i < 64; ++i) {
            HandleLookup l = reg.acquire(s.at(i), HandleKind::File);
            SW_CHECK(l.status == HandleStatus::Ok);
            l.core->unpin();
            SW_CHECK(reg.acquire(s.at(i), HandleKind::Dir).status == HandleStatus::WrongKind);
        }
        SW_CHECK(reg.acquire(nullptr, HandleKind::File).status == HandleStatus::Unknown);
        SW_CHECK(reg.acquire(&owner, HandleKind::File).status == HandleStatus::Unknown);
        SW_CHECK(reg.begin_close(s.at(3), HandleKind::File).status == HandleStatus::Ok);
        SW_CHECK(reg.acquire(s.at(3), HandleKind::File).status == HandleStatus::Closing);
        SW_CHECK(reg.cancel_close(s.at(3), HandleKind::File) == HandleStatus::Ok);
    }
    for (std::size_t i = 0; i < 63; ++i) {
        SW_CHECK(reg.begin_close(s.at(i), HandleKind::File).status == HandleStatus::Ok);
        cores[i].wait_unpinned();
        SW_CHECK(reg.remove(s.at(i), HandleKind::File) == HandleStatus::Ok);
    }
    const std::uint64_t after = oom::g_total.load();
    SW_CHECK(after == before);
    // Remover a última entrada devolve memória (não aloca).
    SW_CHECK(reg.begin_close(s.at(63), HandleKind::File).status == HandleStatus::Ok);
    cores[63].wait_unpinned();
    SW_CHECK(reg.remove(s.at(63), HandleKind::File) == HandleStatus::Ok);
    SW_CHECK(oom::g_total.load() == before);
    SW_CHECK(reg.table_bytes() == 0);
    std::printf("ok: (7) busca/pin/fechar sem alocar (%llu buscas; controle positivo na insercao)\n",
                static_cast<unsigned long long>(200ull * 64 * 2));
}

// (6) A tabela devolve a memória quando esvazia, inclusive depois de crescer; vivos do operator new
// voltam ao nível anterior.
void test_table_memory_returned_when_empty()
{
    HandleRegistry reg;
    const std::int64_t live_before = oom::g_live.load();
    Shells s(20000);
    const std::int64_t live_with_shells = oom::g_live.load();
    std::vector<PinCore> cores(20000);
    const std::int64_t live_with_cores = oom::g_live.load();
    int owner = 6;
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        SW_CHECK(reg.insert(s.at(i), HandleKind::File, &owner, &cores[i]) == HandleStatus::Ok);
    }
    SW_CHECK(reg.size() == s.files.size());
    const std::size_t grown = reg.table_bytes();
    SW_CHECK(grown > 0);
    SW_CHECK(oom::g_live.load() > live_with_cores);
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        SW_CHECK(reg.begin_close(s.at(i), HandleKind::File).status == HandleStatus::Ok);
        SW_CHECK(reg.remove(s.at(i), HandleKind::File) == HandleStatus::Ok);
    }
    SW_CHECK(reg.size() == 0);
    SW_CHECK(reg.table_bytes() == 0);
    SW_CHECK(oom::g_live.load() == live_with_cores);
    (void)live_before;
    (void)live_with_shells;
    std::printf("ok: (6) 20000 entradas (tabela de %zu bytes) e, vazia, a tabela voltou ao sistema\n",
                grown);
}

// Falta de memória na inserção: varredura em cada ponto (primeira tabela da fatia e cada
// crescimento), sem pontos excluídos. Em toda falha: OutOfMemory, a entrada não existe, as outras
// continuam iguais e nada ficou alocado a mais.
void test_insert_out_of_memory_sweep()
{
    oom::g_progress_scenario.store("registro_insert");
    HandleRegistry reg;
    Shells s(3000);
    std::vector<PinCore> cores(3000);
    int owner = 8;
    std::size_t fired_points = 0;
    std::size_t inserted = 0;
    // Endereços todos na mesma fatia, para forçar crescimentos da mesma tabela.
    std::vector<std::size_t> same;
    const std::size_t shard = HandleRegistry::shard_of(s.at(0));
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        if (HandleRegistry::shard_of(s.at(i)) == shard) same.push_back(i);
    }
    SW_CHECK(same.size() >= 30); // ~3000/64
    for (std::size_t idx : same) {
        // Para cada inserção, falha a 1ª, 2ª... alocação até ela passar sem disparar.
        for (std::int64_t k = 1;; ++k) {
            const std::size_t size_before = reg.size();
            const std::size_t bytes_before = reg.table_bytes();
            const std::int64_t live_before = oom::g_live.load();
            oom::g_progress_k.store(k);
            oom::arm(k);
            const HandleStatus st = reg.insert(s.at(idx), HandleKind::File, &owner, &cores[idx]);
            const bool fired = oom::g_fired.load();
            oom::disarm();
            if (!fired) {
                SW_CHECK(st == HandleStatus::Ok);
                ++inserted;
                break;
            }
            ++fired_points;
            SW_CHECK(st == HandleStatus::OutOfMemory);
            SW_CHECK(reg.size() == size_before);
            SW_CHECK(reg.table_bytes() == bytes_before);
            SW_CHECK(oom::g_live.load() == live_before);
            SW_CHECK(reg.acquire(s.at(idx), HandleKind::File).status == HandleStatus::Unknown);
        }
    }
    SW_CHECK(inserted == same.size());
    // Todas as anteriores continuam lá e VIVAS.
    for (std::size_t idx : same) {
        HandleLookup l = reg.acquire(s.at(idx), HandleKind::File);
        SW_CHECK(l.status == HandleStatus::Ok && l.core == &cores[idx]);
        l.core->unpin();
    }
    // Pontos: a primeira tabela e cada crescimento (16 -> 32 -> 64 com ~30 entradas: >= 2).
    SW_CHECK(fired_points >= 2);
    for (std::size_t idx : same) {
        SW_CHECK(reg.begin_close(s.at(idx), HandleKind::File).status == HandleStatus::Ok);
        SW_CHECK(reg.remove(s.at(idx), HandleKind::File) == HandleStatus::Ok);
    }
    SW_CHECK(reg.table_bytes() == 0);
    std::printf("ok: falta de memoria na insercao: %zu pontos, todos OutOfMemory sem mudar nada\n",
                fired_points);
    std::printf("[F1-67B] cenario='registro_insert' insercoes=%zu pontos_de_falha=%zu\n", inserted,
                fired_points);
}

// Quarentena de tamanho fixo: ordem de saída e de esvaziamento.
void test_shell_quarantine_order()
{
    ShellQuarantine<4> q;
    int x[6];
    for (int i = 0; i < 4; ++i) {
        const QuarantinedShell out = q.push(&x[i], 10 + i);
        SW_CHECK(out.ptr == nullptr);
    }
    SW_CHECK(q.size() == 4);
    QuarantinedShell out = q.push(&x[4], 14);
    SW_CHECK(out.ptr == &x[0] && out.size == 10); // sai a mais antiga
    out = q.push(&x[5], 15);
    SW_CHECK(out.ptr == &x[1] && out.size == 11);
    SW_CHECK(!q.contains(&x[0]) && !q.contains(&x[1]) && q.contains(&x[2]) && q.contains(&x[5]));
    int order[4];
    int n = 0;
    q.drain([&](QuarantinedShell sh) { order[n++] = static_cast<int>(static_cast<int*>(sh.ptr) - x); });
    SW_CHECK(n == 4 && order[0] == 2 && order[1] == 3 && order[2] == 4 && order[3] == 5);
    SW_CHECK(q.size() == 0);
    std::puts("ok: quarentena de tamanho fixo (sai a mais antiga; esvazia em ordem)");
}

// Ciclo de abrir/fechar como a parte D fará: casca do alocador do contexto, registro, fechamento
// (FECHANDO, espera, remove), casca para a quarentena, a que sai volta ao alocador.
struct FakeContext
{
    ContextAllocator alloc;
    ShellQuarantine<kFileQuarantine> quarantine;
};

sw_file* open_shell(HandleRegistry& reg, FakeContext& ctx, PinCore& core)
{
    sw_file* f = ctx.alloc.create<sw_file>(nullptr);
    SW_CHECK(f != nullptr);
    SW_CHECK(reg.insert(f, HandleKind::File, &ctx, &core) == HandleStatus::Ok);
    return f;
}

void close_shell(HandleRegistry& reg, FakeContext& ctx, sw_file* f)
{
    HandleLookup l = reg.begin_close(f, HandleKind::File);
    SW_CHECK(l.status == HandleStatus::Ok);
    l.core->wait_unpinned();
    SW_CHECK(reg.remove(f, HandleKind::File) == HandleStatus::Ok);
    f->body = nullptr;
    const QuarantinedShell out = ctx.quarantine.push(f, sizeof(sw_file));
    if (out.ptr != nullptr) ctx.alloc.destroy(static_cast<sw_file*>(out.ptr));
}

// Testes (1), (2) e (3) do F1-60 §1.4, com um alocador que reemite SEMPRE o último bloco liberado.
void test_quarantine_window_and_documented_limit()
{
    HandleRegistry reg;
    swarmstream::test::ReissueAllocator ra(64);
    FakeContext ctx{ContextAllocator(ra.as_sw()), {}};
    PinCore core; // o mesmo PinCore serve: um handle por vez
    std::vector<sw_file*> closed;
    for (std::size_t i = 0; i < kFileQuarantine + 1; ++i) {
        sw_file* f = open_shell(reg, ctx, core);
        // (3) dentro da janela, o alocador nunca reemitiu um endereço em quarentena.
        SW_CHECK(!ctx.quarantine.contains(f));
        for (sw_file* old : closed) SW_CHECK(old != f);
        close_shell(reg, ctx, f);
        closed.push_back(f);
    }
    // 257 fechados: o mais antigo saiu da quarentena e voltou ao alocador; os 256 mais novos estão
    // retidos e (1) são rejeitados.
    SW_CHECK(ctx.quarantine.size() == kFileQuarantine);
    SW_CHECK(!ctx.quarantine.contains(closed[0]));
    for (std::size_t i = 1; i < closed.size(); ++i) {
        SW_CHECK(ctx.quarantine.contains(closed[i])); // (3) o 256º mais recente ainda retido
        SW_CHECK(reg.acquire(closed[i], HandleKind::File).status == HandleStatus::Unknown);
        SW_CHECK(reg.begin_close(closed[i], HandleKind::File).status == HandleStatus::Unknown);
    }
    SW_CHECK(reg.acquire(closed[0], HandleKind::File).status == HandleStatus::Unknown);

    // (2) LIMITE DOCUMENTADO: o próximo handle reusa o endereço do mais antigo, e o ponteiro antigo
    // passa a valer como o handle NOVO. A API não distingue. Isto prova ausência de erro de memória,
    // não detecção.
    PinCore core_new;
    sw_file* reopened = open_shell(reg, ctx, core_new);
    SW_CHECK(reopened == closed[0]);
    HandleLookup l = reg.acquire(closed[0], HandleKind::File);
    SW_CHECK(l.status == HandleStatus::Ok && l.core == &core_new && l.owner == &ctx);
    l.core->unpin();
    close_shell(reg, ctx, reopened);

    // Fim: a quarentena é esvaziada (no sw_shutdown) e tudo volta ao alocador.
    ctx.quarantine.drain([&](QuarantinedShell sh) { ctx.alloc.destroy(static_cast<sw_file*>(sh.ptr)); });
    SW_CHECK(ra.live() == 0);
    SW_CHECK(reg.size() == 0 && reg.table_bytes() == 0);
    std::puts("ok: (1)(2)(3) janela de 256 rejeita; fora dela o endereco reemitido vale como o novo");
}

void test_process_ctx_quarantine()
{
    ProcessCtxQuarantine& q = ProcessCtxQuarantine::instance();
    SW_CHECK(&q == &ProcessCtxQuarantine::instance());
    const std::size_t start = q.size();
    SW_CHECK(start == 0);
    std::vector<sw_ctx> shells(kCtxQuarantine + 3);
    std::size_t evicted = 0;
    for (std::size_t i = 0; i < shells.size(); ++i) {
        const QuarantinedShell out = q.push(&shells[i], sizeof(sw_ctx));
        if (i < kCtxQuarantine) {
            SW_CHECK(out.ptr == nullptr);
        } else {
            SW_CHECK(out.ptr == &shells[i - kCtxQuarantine]); // sai a mais antiga
            SW_CHECK(out.size == sizeof(sw_ctx));
            ++evicted;
        }
    }
    SW_CHECK(evicted == 3);
    SW_CHECK(q.size() == kCtxQuarantine);
    SW_CHECK(!q.contains(&shells[2]) && q.contains(&shells[3]) && q.contains(&shells.back()));
    std::puts("ok: quarentena de sw_ctx do processo (64 ultimas, vetor estatico)");
}

// (5) 16 threads abrindo, consultando, fechando (inclusive handles das outras) no registro GLOBAL.
void test_sixteen_threads()
{
    HandleRegistry& reg = HandleRegistry::instance();
    SW_CHECK(&reg == &HandleRegistry::instance());
    constexpr int kThreads = 16;
    constexpr int kPerThread = 48;
    constexpr int kRounds = 60;
    std::vector<sw_file> shells(static_cast<std::size_t>(kThreads) * kPerThread);
    std::vector<PinCore> cores(shells.size());
    std::atomic<std::uint64_t> pins_ok{0};
    std::atomic<std::uint64_t> seen_closing{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            unsigned rng = 12345u + static_cast<unsigned>(t) * 977u;
            for (int r = 0; r < kRounds; ++r) {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::size_t idx = static_cast<std::size_t>(t) * kPerThread + i;
                    SW_CHECK(reg.insert(&shells[idx], HandleKind::File, &shells, &cores[idx]) ==
                             HandleStatus::Ok);
                }
                for (int i = 0; i < kPerThread * 4; ++i) {
                    rng = rng * 1103515245u + 12345u;
                    const std::size_t other = (rng >> 8) % shells.size();
                    HandleLookup l = reg.acquire(&shells[other], HandleKind::File);
                    if (l.status == HandleStatus::Ok) {
                        SW_CHECK(l.core == &cores[other] && l.owner == &shells);
                        pins_ok.fetch_add(1, std::memory_order_relaxed);
                        l.core->unpin();
                    } else if (l.status == HandleStatus::Closing) {
                        seen_closing.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        SW_CHECK(l.status == HandleStatus::Unknown);
                    }
                }
                for (int i = 0; i < kPerThread; ++i) {
                    const std::size_t idx = static_cast<std::size_t>(t) * kPerThread + i;
                    HandleLookup l = reg.begin_close(&shells[idx], HandleKind::File);
                    SW_CHECK(l.status == HandleStatus::Ok);
                    l.core->wait_unpinned();
                    SW_CHECK(reg.remove(&shells[idx], HandleKind::File) == HandleStatus::Ok);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    SW_CHECK(reg.size() == 0);
    SW_CHECK(reg.table_bytes() == 0);
    SW_CHECK(pins_ok.load() > 0);
    for (const PinCore& c : cores) SW_CHECK(c.pins() == 0);
    std::printf("ok: (5) 16 threads no registro global (%llu pins, %llu vistos FECHANDO)\n",
                static_cast<unsigned long long>(pins_ok.load()),
                static_cast<unsigned long long>(seen_closing.load()));
}

// Limite da contagem de pins (2^31 - 1): o pin seguinte é recusado (PinOverflow), sem dar a volta
// nem ligar o bit FECHANDO por engano.
void test_pin_overflow()
{
    HandleRegistry reg;
    Shells s(1);
    PinCore core;
    int owner = 9;
    SW_CHECK(reg.insert(s.at(0), HandleKind::File, &owner, &core) == HandleStatus::Ok);
    using swarmstream::detail::testing::PinCoreTestHook;
    PinCoreTestHook::set_pins(core, PinCoreTestHook::max_pins() - 1);
    HandleLookup l = reg.acquire(s.at(0), HandleKind::File);
    SW_CHECK(l.status == HandleStatus::Ok && core.pins() == PinCoreTestHook::max_pins());
    SW_CHECK(reg.acquire(s.at(0), HandleKind::File).status == HandleStatus::PinOverflow);
    SW_CHECK(core.pins() == PinCoreTestHook::max_pins() && !core.closing());
    core.unpin();
    SW_CHECK(core.pins() == PinCoreTestHook::max_pins() - 1);
    PinCoreTestHook::set_pins(core, 0);
    SW_CHECK(reg.begin_close(s.at(0), HandleKind::File).status == HandleStatus::Ok);
    core.wait_unpinned();
    SW_CHECK(reg.remove(s.at(0), HandleKind::File) == HandleStatus::Ok);
    std::puts("ok: limite de pins (2^31 - 1): o seguinte e PinOverflow, sem dar a volta");
}

// As 64 fatias são usadas (a fatia sai só do valor do ponteiro): 4096 cascas contíguas cobrem
// todas, e nenhuma fatia fica com mais que 4x a média (sem isso, a "mesma fatia" seria todas).
void test_shards_are_spread()
{
    Shells s(4096);
    std::size_t per[HandleRegistry::kShards] = {};
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        const std::size_t k = HandleRegistry::shard_of(s.at(i));
        SW_CHECK(k < HandleRegistry::kShards);
        ++per[k];
    }
    std::size_t used = 0;
    std::size_t most = 0;
    for (std::size_t n : per) {
        if (n > 0) ++used;
        if (n > most) most = n;
    }
    SW_CHECK(used == HandleRegistry::kShards);
    SW_CHECK(most <= 4 * (s.files.size() / HandleRegistry::kShards));
    std::printf("ok: 64 fatias usadas por 4096 cascas contiguas (maior fatia: %zu)\n", most);
}

void test_shell_sizes()
{
    static_assert(sizeof(sw_file) <= swarmstream::detail::kMaxShellBytes);
    static_assert(sizeof(sw_dir) <= swarmstream::detail::kMaxShellBytes);
    static_assert(sizeof(sw_ctx) <= swarmstream::detail::kMaxShellBytes);
    std::printf("ok: (8) cascas: sw_ctx=%zu sw_file=%zu sw_dir=%zu bytes (limite 64, static_assert)\n",
                sizeof(sw_ctx), sizeof(sw_file), sizeof(sw_dir));
}

} // namespace

int main()
{
    oom::make_abort_fail_instead_of_hang();
    oom::warm_up_exceptions();
    test_shell_sizes();
    test_shards_are_spread();
    test_basic_states();
    test_cancel_close_restores_live();
    test_pin_overflow();
    test_close_waits_other_pin_without_holding_shard();
    test_many_pins_released_one_by_one();
    test_closer_frees_core_right_after_wait();
    test_lookup_and_pin_do_not_allocate();
    test_table_memory_returned_when_empty();
    test_insert_out_of_memory_sweep();
    test_shell_quarantine_order();
    test_quarantine_window_and_documented_limit();
    test_process_ctx_quarantine();
    test_sixteen_threads();
    std::puts("TODOS OS TESTES DO REGISTRO DE HANDLES (F1-67 B) PASSARAM");
    return 0;
}
