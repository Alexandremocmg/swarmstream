// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-71: `CacheOwner::stop()` (e `~CacheOwner()`) sem terminar o processo por falta de memória nem
// por exceção (ADR-0018, decisão 1 e 2).
//
// O que este executável prova:
//  1. Varredura de operator new SEM pontos excluídos: para K = 1, 2, ... a K-ésima alocação feita
//     DURANTE o stop() falha (std::bad_alloc), em quatro cenários (cache vazio; 4 peças; peças e um
//     FREE_SLOT em dois arquivos de store; destruição do CacheOwner em vez de stop()). Em todo
//     ponto que dispara: nenhum std::terminate; stop() devolve OutOfMemory; o journal e o store
//     foram FECHADOS (nenhum arquivo aberto); chamar stop() de novo é seguro e devolve sucesso; a
//     pasta REABRE pela recuperação com TODAS as peças confirmadas, cada uma lida e conferida bloco
//     a bloco (nada não verificado é entregue). Quando o K passa do último ponto, o stop() termina
//     em Success, com o checkpoint gravado.
//  2. Exceção injetada (gancho de teste) em cada passo do stop() (checkpoint, fechamento do
//  journal,
//     fechamento do store) com std::runtime_error, um tipo que não é std::exception e
//     std::bad_alloc: Internal (ou OutOfMemory para bad_alloc), nada termina, o resto da limpeza
//     executa (handles de volta ao nível anterior), stop() de novo é Success e a pasta reabre. Um
//     checkpoint pulado deixa o journal com os registros (a recuperação os aplica); um stop()
//     normal os consolida.
//  3. O primeiro erro vence: erro de disco no checkpoint (index.bin é uma pasta) seguido de exceção
//     depois dele devolve o erro de disco; bad_alloc no checkpoint seguido de outra exceção devolve
//     OutOfMemory.
//  4. As fronteiras noexcept do caminho de parada que NÃO alocam: Journal::close, StoreFiles::close
//     e a destruição do StoreFiles (contador de operator new igual antes e depois; controle
//     positivo no mesmo contador).
//
// No MSVC em Debug os construtores padrão e de movimento de std::vector/std::string/fs::path são
// `noexcept` e alocam o proxy dos iteradores: um bad_alloc ali é std::terminate ANTES de qualquer
// catch (F1-63, F1-69). Esta varredura roda no Windows Debug e Release do CI e falha por K, com a
// pilha do terminate, se algum desses construtores ainda estiver no caminho de parada.
#include "cache_journal.hpp"
#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "oom_support.hpp"
#include "swarmstream/block_ram_cache.hpp"
#include "swarmstream/cache_owner.hpp"
#include "swarmstream/store_files.hpp"
#include "swarmstream/telemetry.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;
using oom::arm;
using oom::disarm;
using oom::g_fired;
using oom::g_progress_k;
using oom::g_progress_scenario;
using oom::g_total;
using oom::open_handles;
using sw::CacheErrorCode;
using sw::CacheOwner;
using sw::CacheOwnerTestHook;
using sw::StopFault;
using sw::StopStep;

namespace {

constexpr sw::StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};
constexpr uint32_t kTotalSlots = 8;

fs::path fresh_dir(const char* tag)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_71_stop_") + tag + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    return dir;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    SW_CHECK(!ec); // uma pasta que não apaga (Windows) denuncia um arquivo ainda aberto
}

sw::PieceId piece(uint32_t i)
{
    sw::PieceHash h{};
    for (size_t b = 0; b < h.bytes.size(); ++b)
        h.bytes[b] = static_cast<uint8_t>(i * 13 + b + 1);
    return sw::PieceId(i, h);
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

sw::CacheOwnerConfig config_for(const fs::path& dir)
{
    return sw::CacheOwnerConfig{.dir = dir, .total_slots = kTotalSlots, .layout = kLayout};
}

std::unique_ptr<CacheOwner> open_owner(const fs::path& dir)
{
    auto o = CacheOwner::start(config_for(dir));
    SW_CHECK(o.has_value());
    return std::move(*o);
}

void put(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    auto r = sw::CacheRequest::put(piece(i), c.bytes, c.blocks);
    auto res = o.call(r);
    SW_CHECK(res.has_value());
    SW_CHECK(*res == c.bytes.size());
}

void remove_piece(CacheOwner& o, uint32_t i)
{
    auto r = sw::CacheRequest::remove(piece(i));
    auto res = o.call(r);
    SW_CHECK(res.has_value());
}

// Lê a peça i com os blocos do "manifesto" e confere o conteúdo byte a byte: o que a recuperação
// entrega foi VERIFICADO (XXH3 por bloco, na leitura) e é o que foi gravado.
bool present_and_verified(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    std::vector<std::byte> out(kLayout.slot_size);
    auto r = sw::CacheRequest::get(piece(i), out, c.blocks);
    auto res = o.call(r);
    if (!res.has_value()) {
        SW_CHECK(res.error() == CacheErrorCode::PieceNotFound);
        return false;
    }
    SW_CHECK(*res == c.bytes.size());
    SW_CHECK(std::memcmp(out.data(), c.bytes.data(), c.bytes.size()) == 0);
    return true;
}

// O journal e os arquivos do store foram FECHADOS? No Windows, abrindo cada arquivo SEM
// compartilhar nada (o CacheOwner abre com FILE_SHARE_READ): se ainda houver um handle aberto, a
// abertura falha com violação de compartilhamento. A contagem de handles do processo NÃO serve
// aqui: o runtime do MinGW (winpthreads) cria os handles das variáveis de condição do Impl e só os
// solta ao destruí-lo. No POSIX a contagem de descritores (/proc/self/fd) é exata e basta.
bool journal_and_store_closed([[maybe_unused]] const fs::path& dir,
                              [[maybe_unused]] std::size_t handles_before)
{
#if defined(_WIN32)
    std::vector<fs::path> files;
    files.push_back(dir / "journal.bin");
    for (uint32_t i = 0; i < kTotalSlots / kLayout.slots_per_file; ++i)
        files.push_back(sw::StoreFiles::file_path(dir, i));
    for (const fs::path& f : files) {
        std::error_code ec;
        if (!fs::exists(f, ec)) continue;
        HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        SW_CHECK(::CloseHandle(h) != 0);
    }
    return true;
#else
    return open_handles() == handles_before;
#endif
}

enum class Scenario {
    Empty,       // nada gravado: índice vazio
    Pieces,      // 4 peças (um arquivo de store)
    PiecesFreed, // 5 peças (dois arquivos de store) e uma removida (FREE_SLOT no journal)
};

const char* name_of(Scenario s)
{
    switch (s) {
    case Scenario::Empty: return "cache vazio";
    case Scenario::Pieces: return "4 pecas";
    case Scenario::PiecesFreed: return "5 pecas e 1 removida (2 arquivos de store)";
    }
    return "?";
}

// As peças que têm de existir depois de reabrir, para cada cenário.
std::vector<uint32_t> expected_pieces(Scenario s)
{
    switch (s) {
    case Scenario::Empty: return {};
    case Scenario::Pieces: return {0, 1, 2, 3};
    case Scenario::PiecesFreed: return {0, 1, 3, 4}; // a 2 foi removida
    }
    return {};
}

void populate(CacheOwner& o, Scenario s)
{
    switch (s) {
    case Scenario::Empty: return;
    case Scenario::Pieces:
        for (uint32_t i = 0; i < 4; ++i)
            put(o, i);
        return;
    case Scenario::PiecesFreed:
        for (uint32_t i = 0; i < 5; ++i)
            put(o, i);
        remove_piece(o, 2);
        return;
    }
}

// Reabre a pasta (a recuperação roda) e confere: exatamente as peças esperadas, todas verificadas,
// e as removidas continuam ausentes. Devolve quantos registros do journal a recuperação aplicou.
uint64_t reopen_and_verify(const fs::path& dir, Scenario s)
{
    auto o = open_owner(dir);
    const std::vector<uint32_t> want = expected_pieces(s);
    SW_CHECK(o->pieces() == want.size());
    for (const uint32_t i : want)
        SW_CHECK(present_and_verified(*o, i));
    if (s == Scenario::PiecesFreed) SW_CHECK(!present_and_verified(*o, 2));
    const uint64_t applied = o->recovery().journal_records_applied;
    SW_CHECK(o->stop().has_value());
    return applied;
}

// ---------------------------------------------------------------------------- 1. varredura de new

struct SweepResult
{
    int fired_points = 0; // pontos K em que a falha disparou
    int ok_at = 0;        // K do primeiro stop() que terminou sem a falha disparar
};

// `via_destructor`: a parada é a destruição do CacheOwner (que chama stop()), não stop() direto.
SweepResult sweep(Scenario scenario, bool via_destructor)
{
    const std::string label =
        std::string(name_of(scenario)) + (via_destructor ? " [destrutor]" : " [stop() explicito]");
    std::cout << "[Test] varredura de falta de memoria no stop(): " << label << std::endl;
    SweepResult result;
    oom::warm_up_exceptions();
    // Aquecimento: um ciclo completo antes de contar (nada preguiçoso do runtime vira handle novo).
    {
        const fs::path warm = fresh_dir("warm");
        {
            auto o = open_owner(warm);
            populate(*o, scenario);
            SW_CHECK(o->stop().has_value());
        }
        cleanup(warm);
    }

    int detail_oom = 0;
    for (std::int64_t k = 1; k <= 2000; ++k) {
        const fs::path dir = fresh_dir("sweep");
        const std::size_t handles_before = open_handles();
        auto owner = open_owner(dir);
        populate(*owner, scenario);

        g_progress_k.store(k);
        g_progress_scenario.store(name_of(scenario));
        CacheErrorCode got = CacheErrorCode::Success;
        arm(k);
        if (via_destructor) {
            owner.reset();
        } else {
            const auto r = owner->stop();
            if (!r.has_value()) got = r.error();
        }
        const bool fired = g_fired.load();
        disarm();

        // Journal e store FECHADOS pelo próprio stop() (antes de destruir o objeto).
        if (!via_destructor && !journal_and_store_closed(dir, handles_before)) {
            std::cerr << "VAZAMENTO no ponto K=" << k << " (" << label
                      << "): journal ou store ficou aberto depois do stop(); codigo="
                      << sw::to_string(got) << std::endl;
            SW_CHECK(false);
        }

        if (!fired) {
            // O stop() terminou antes da K-ésima alocação: fim da varredura deste cenário.
            SW_CHECK(got == CacheErrorCode::Success);
            result.ok_at = static_cast<int>(k);
        } else {
            ++result.fired_points;
            if (!via_destructor) {
                if (got != CacheErrorCode::OutOfMemory) {
                    std::cerr << "CODIGO ERRADO no ponto K=" << k << " (" << label
                              << "): esperado OutOfMemory, veio " << sw::to_string(got)
                              << std::endl;
                }
                SW_CHECK(got == CacheErrorCode::OutOfMemory);
                ++detail_oom;
            }
        }

        if (!via_destructor) {
            // Chamar de novo é seguro e devolve sucesso (idempotente), inclusive depois da falha.
            SW_CHECK(owner->stop().has_value());
            owner.reset();
        }
        // Tudo solto depois de destruir: nenhum arquivo aberto (no Windows a contagem global de
        // handles oscila entre execuções, então só os arquivos são conferidos; no POSIX, os
        // descritores).
        SW_CHECK(journal_and_store_closed(dir, handles_before));

        // A pasta reabre pela recuperação com tudo o que foi confirmado, tudo verificado.
        const uint64_t applied = reopen_and_verify(dir, scenario);
        if (!fired) {
            // Stop completo: o checkpoint consolidou o journal; nada a reaplicar.
            SW_CHECK(applied == 0);
        }
        cleanup(dir);
        if (!fired) break;
    }
    SW_CHECK(result.ok_at > 0);        // a varredura terminou: o stop() completo foi alcançado
    SW_CHECK(result.fired_points > 0); // e houve pontos de falha de verdade (controle)
    if (!via_destructor) SW_CHECK(detail_oom == result.fired_points);
    std::cout << "  pontos de falha cobertos: " << result.fired_points << " (stop() completo na "
              << result.ok_at << "a alocacao)" << std::endl;
    // Resumo para o log do CI (o ctest só mostra a saída de quem falha; o passo do ci.yml copia as
    // linhas "[F1-71]" de LastTest.log): uma queda silenciosa dos pontos, inclusive no MSVC Debug,
    // fica visível.
    std::cout << "[F1-71] cenario='" << label << "' pontos_de_falha=" << result.fired_points
              << " stop_completo_na_alocacao=" << result.ok_at << std::endl;
    return result;
}

// ---------------------------------------------------------------------------- 2. exceção injetada

const char* step_name(StopStep s)
{
    switch (s) {
    case StopStep::Checkpoint: return "checkpoint";
    case StopStep::JournalClose: return "fechar o journal";
    case StopStep::StoreClose: return "fechar o store";
    }
    return "?";
}

const char* fault_name(StopFault f)
{
    switch (f) {
    case StopFault::None: return "nenhuma";
    case StopFault::StdException: return "std::runtime_error";
    case StopFault::NonStdException: return "int (nao e std::exception)";
    case StopFault::BadAlloc: return "std::bad_alloc";
    case StopFault::IoError: return "IoError devolvido";
    }
    return "?";
}

CacheErrorCode expected_for(StopFault f)
{
    return f == StopFault::BadAlloc ? CacheErrorCode::OutOfMemory : CacheErrorCode::Internal;
}

void test_injected_exception_in_each_step()
{
    std::cout
        << "[Test] excecao injetada em cada passo do stop(): Internal/OutOfMemory, o resto da "
           "limpeza executa"
        << std::endl;
    oom::warm_up_exceptions();
    const StopStep steps[] = {StopStep::Checkpoint, StopStep::JournalClose, StopStep::StoreClose};
    const StopFault faults[] = {StopFault::StdException, StopFault::NonStdException,
                                StopFault::BadAlloc};
    for (const StopStep step : steps) {
        for (const StopFault fault : faults) {
            for (const bool via_destructor : {false, true}) {
                std::cout << "  passo '" << step_name(step) << "' com " << fault_name(fault)
                          << (via_destructor ? " [destrutor]" : "") << std::endl;
                const fs::path dir = fresh_dir("inject");
                const std::size_t handles_before = open_handles();
                auto owner = open_owner(dir);
                populate(*owner, Scenario::Pieces);
                CacheOwnerTestHook::inject_stop_fault(*owner, step, fault);
                if (via_destructor) {
                    owner.reset(); // não pode terminar o processo
                } else {
                    const auto r = owner->stop();
                    SW_CHECK(!r.has_value());
                    SW_CHECK(r.error() == expected_for(fault));
                    // O resto da limpeza executou: journal e store fechados.
                    SW_CHECK(journal_and_store_closed(dir, handles_before));
                    SW_CHECK(owner->stop().has_value()); // idempotente
                    owner.reset();
                }
                SW_CHECK(journal_and_store_closed(dir, handles_before));
                const uint64_t applied = reopen_and_verify(dir, Scenario::Pieces);
                if (step == StopStep::Checkpoint) {
                    // O checkpoint foi PULADO: o journal ficou com os 4 registros e a recuperação
                    // os aplicou (nada foi perdido).
                    SW_CHECK(applied == 4);
                } else {
                    // O checkpoint rodou antes: consolidou o journal.
                    SW_CHECK(applied == 0);
                }
                cleanup(dir);
            }
        }
    }
}

// Controle: o stop() sem falha consolida (applied == 0) e devolve sucesso; e a falha injetada de
// verdade deixa o journal com os registros (applied > 0). Sem isto, o teste acima poderia passar
// sem que o gancho funcionasse.
void test_control_without_fault()
{
    std::cout << "[Test] controle: sem falha, o stop() consolida o journal e devolve Success"
              << std::endl;
    const fs::path dir = fresh_dir("control");
    {
        auto owner = open_owner(dir);
        populate(*owner, Scenario::Pieces);
        const auto r = owner->stop();
        SW_CHECK(r.has_value());
    }
    SW_CHECK(reopen_and_verify(dir, Scenario::Pieces) == 0);
    cleanup(dir);
}

// ---------------------------------------------------------------------------- 3. o primeiro erro
// vence

void test_first_error_wins()
{
    std::cout << "[Test] o primeiro erro vence (erro de disco no checkpoint; OutOfMemory depois "
                 "de outra excecao)"
              << std::endl;
    oom::warm_up_exceptions();
    {
        // index.bin vira uma PASTA: a troca do índice falha (IoError, erro de disco), e as demais
        // etapas ainda executam, mesmo com exceção injetada depois.
        const fs::path dir = fresh_dir("first_disk");
        const std::size_t handles_before = open_handles();
        auto owner = open_owner(dir);
        populate(*owner, Scenario::Pieces);
        std::error_code ec;
        fs::remove(dir / "index.bin", ec);
        SW_CHECK(!ec);
        fs::create_directory(dir / "index.bin", ec);
        SW_CHECK(!ec);
        CacheOwnerTestHook::inject_stop_fault(*owner, StopStep::JournalClose,
                                              StopFault::StdException);
        const auto r = owner->stop();
        SW_CHECK(!r.has_value());
        SW_CHECK(r.error() == CacheErrorCode::IoError); // o erro de disco veio primeiro
        SW_CHECK(journal_and_store_closed(dir, handles_before));
        SW_CHECK(owner->stop().has_value());
        owner.reset();
        SW_CHECK(journal_and_store_closed(dir, handles_before));
        cleanup(dir);
    }
    {
        // bad_alloc no checkpoint e outra exceção no fechamento do journal: vence o OutOfMemory.
        const fs::path dir = fresh_dir("first_oom");
        const std::size_t handles_before = open_handles();
        auto owner = open_owner(dir);
        populate(*owner, Scenario::Pieces);
        CacheOwnerTestHook::inject_stop_fault(*owner, StopStep::Checkpoint, StopFault::BadAlloc);
        CacheOwnerTestHook::inject_stop_fault(*owner, StopStep::JournalClose,
                                              StopFault::StdException);
        CacheOwnerTestHook::inject_stop_fault(*owner, StopStep::StoreClose,
                                              StopFault::NonStdException);
        const auto r = owner->stop();
        SW_CHECK(!r.has_value());
        SW_CHECK(r.error() == CacheErrorCode::OutOfMemory);
        SW_CHECK(journal_and_store_closed(dir, handles_before));
        owner.reset();
        SW_CHECK(journal_and_store_closed(dir, handles_before));
        SW_CHECK(reopen_and_verify(dir, Scenario::Pieces) == 4);
        cleanup(dir);
    }
}

// R1 da revisão: o ERRO de journal.close() e de store.close() é guardado, devolvido e o primeiro
// vence. O gancho faz o passo devolver IoError (nos fechamentos, depois de fechar de verdade).
struct StopCase
{
    const char* name;
    StopStep step1;
    StopFault fault1;
    StopStep step2; // StopFault::None = só uma falha
    StopFault fault2;
    CacheErrorCode expected;
    uint64_t expected_applied; // registros do journal que a recuperação aplica depois
};

void test_close_errors_are_returned_and_first_wins()
{
    std::cout
        << "[Test] erro de journal.close()/store.close(): devolvido, o passo seguinte executa, "
           "o primeiro erro vence"
        << std::endl;
    const StopCase cases[] = {
        {"journal.close() devolve IoError", StopStep::JournalClose, StopFault::IoError,
         StopStep::StoreClose, StopFault::None, CacheErrorCode::IoError, 0},
        {"store.close() devolve IoError", StopStep::StoreClose, StopFault::IoError,
         StopStep::JournalClose, StopFault::None, CacheErrorCode::IoError, 0},
        {"checkpoint devolve IoError (nem roda)", StopStep::Checkpoint, StopFault::IoError,
         StopStep::JournalClose, StopFault::None, CacheErrorCode::IoError, 4},
        {"journal IoError e store bad_alloc: vence o journal", StopStep::JournalClose,
         StopFault::IoError, StopStep::StoreClose, StopFault::BadAlloc, CacheErrorCode::IoError, 0},
        {"journal bad_alloc e store IoError: vence o journal", StopStep::JournalClose,
         StopFault::BadAlloc, StopStep::StoreClose, StopFault::IoError, CacheErrorCode::OutOfMemory,
         0},
        {"journal IoError e store IoError", StopStep::JournalClose, StopFault::IoError,
         StopStep::StoreClose, StopFault::IoError, CacheErrorCode::IoError, 0},
    };
    oom::warm_up_exceptions();
    for (const StopCase& c : cases) {
        for (const bool via_destructor : {false, true}) {
            std::cout << "  " << c.name << (via_destructor ? " [destrutor]" : "") << std::endl;
            const fs::path dir = fresh_dir("close_err");
            const std::size_t handles_before = open_handles();
            auto owner = open_owner(dir);
            populate(*owner, Scenario::Pieces);
            CacheOwnerTestHook::inject_stop_fault(*owner, c.step1, c.fault1);
            CacheOwnerTestHook::inject_stop_fault(*owner, c.step2, c.fault2);
            if (via_destructor) {
                owner.reset(); // não pode terminar o processo
            } else {
                const auto r = owner->stop();
                SW_CHECK(!r.has_value());
                SW_CHECK(r.error() == c.expected);
                // O passo seguinte ainda executou: journal e store fechados.
                SW_CHECK(journal_and_store_closed(dir, handles_before));
                SW_CHECK(owner->stop().has_value());
                owner.reset();
            }
            SW_CHECK(journal_and_store_closed(dir, handles_before));
            SW_CHECK(reopen_and_verify(dir, Scenario::Pieces) == c.expected_applied);
            cleanup(dir);
        }
    }
}

// R3 da revisão: cache `broken` => stop() PULA o checkpoint (a recuperação decide na próxima
// abertura). O checkpoint, se rodasse, gravaria o índice sem a remoção que falhou e consolidaria o
// journal (applied == 0); pulado, o journal fica com os registros (applied > 0).
void test_broken_cache_skips_checkpoint_on_stop()
{
    std::cout << "[Test] cache quebrado (sync do journal falhou): stop() pula o checkpoint"
              << std::endl;
    const fs::path dir = fresh_dir("broken");
    {
        auto owner = open_owner(dir);
        populate(*owner, Scenario::Pieces);
        CacheOwnerTestHook::fail_next_journal_sync(*owner);
        auto rm = sw::CacheRequest::remove(piece(1));
        const auto res = owner->call(rm);
        SW_CHECK(!res.has_value() && res.error() == CacheErrorCode::IoError);
        SW_CHECK(owner->broken());
        SW_CHECK(owner->stop().has_value()); // fecha sem erro; o checkpoint não rodou
    }
    {
        auto o = open_owner(dir);
        SW_CHECK(o->recovery().journal_records_applied > 0); // o journal NÃO foi consolidado
        for (const uint32_t i : {0u, 2u, 3u})
            SW_CHECK(present_and_verified(*o, i));
        (void)present_and_verified(*o, 1); // presente ou não, se vier tem de vir certa
        SW_CHECK(o->stop().has_value());
    }
    cleanup(dir);
}

// ---------------------------------------------------------------------------- 4. fronteiras sem
// alocação

void test_close_boundaries_do_not_allocate()
{
    std::cout << "[Test] Journal::close, StoreFiles::close e ~StoreFiles nao alocam (fronteiras "
                 "noexcept do caminho de parada)"
              << std::endl;
    const fs::path dir = fresh_dir("close");
    {
        // Journal::close
        det::JournalReplay replay;
        auto opened =
            det::Journal::open(dir / "journal.bin", 0, [](const det::JournalRecord&) {}, replay);
        SW_CHECK(opened.has_value());
        det::Journal j = std::move(*opened);
        const std::uint64_t before = g_total.load();
        const CacheErrorCode rc = j.close();
        const CacheErrorCode again = j.close(); // idempotente
        SW_CHECK(g_total.load() == before);
        SW_CHECK(rc == CacheErrorCode::Success && again == CacheErrorCode::Success);
    }
    {
        // StoreFiles::close (com um arquivo de store aberto) e a destruição.
        auto store = sw::StoreFiles::open(dir, kTotalSlots, kLayout);
        SW_CHECK(store.has_value());
        SW_CHECK(store->ensure_slot(0).has_value());
        const std::uint64_t before = g_total.load();
        const auto rc = store->close();
        const auto again = store->close();
        SW_CHECK(g_total.load() == before);
        SW_CHECK(rc.has_value() && again.has_value());
        const std::uint64_t before_dtor = g_total.load();
        {
            sw::StoreFiles dead(std::move(*store));
        }
        // A destruição libera memória (delete) mas não aloca nada.
        SW_CHECK(g_total.load() == before_dtor);
    }
    {
        // Controle positivo: o contador enxerga uma alocação de verdade.
        const std::uint64_t pre = g_total.load();
        void* volatile probe = ::operator new(16);
        SW_CHECK(g_total.load() == pre + 1);
        ::operator delete(probe);
    }
    cleanup(dir);
}

// Auditoria do caminho de parada, itens que NÃO são do CacheOwner (ADR-0018 decisão 1): o destrutor
// da telemetria (resumo, fflush, fclose, join) e o BlockRamCache não alocam. O `close()` explícito
// da telemetria é entrega do F1-67 (item 4 do cartão do F1-71): aqui só o destrutor de hoje.
void test_other_stop_path_destructors_do_not_allocate()
{
    std::cout << "[Test] ~TelemetryLog e ~BlockRamCache nao alocam (auditoria do caminho de parada)"
              << std::endl;
    const fs::path dir = fresh_dir("telemetry");
    {
        auto log = sw::TelemetryLog::open(dir / "telemetria.jsonl", 64);
        SW_CHECK(log != nullptr);
        SW_CHECK(log->emit_eviction(3, 4096));
        SW_CHECK(log->emit_frame_stutter(1500, "teste"));
        log->flush();
        SW_CHECK(log->written() == 2 && log->write_errors() == 0);
        const std::uint64_t before = g_total.load();
        log.reset(); // resumo + fflush + fclose + join
        SW_CHECK(g_total.load() == before);
    }
    {
        auto cache = std::make_unique<sw::BlockRamCache>(4 * sw::kMaxBlockSize);
        sw::BlockKey key{};
        key.piece = piece(2);
        key.block = 0;
        auto fill = cache->reserve(key, 100);
        SW_CHECK(fill.has_value());
        auto lease = fill->commit();
        SW_CHECK(lease.valid());
        lease.reset();
        const std::uint64_t before = g_total.load();
        cache.reset();
        SW_CHECK(g_total.load() == before);
    }
    cleanup(dir);
}

// O nome do checkpoint na largura nativa (append_path_leaf) dá o MESMO caminho de `dir / "nome"`.
void test_native_leaf_matches_operator_slash()
{
    std::cout << "[Test] append_path_leaf == operator/ para os caminhos do checkpoint" << std::endl;
    const fs::path tmpdir = fs::temp_directory_path();
    std::vector<fs::path> dirs;
    dirs.push_back(tmpdir);
    dirs.push_back(tmpdir / "sub");
    dirs.push_back(tmpdir / "sub" / ""); // termina em separador
    dirs.push_back(fs::path("relativo"));
    dirs.push_back(fs::path("relativo") / "");
    for (const fs::path& d : dirs) {
        fs::path index(d);
        det::append_path_leaf(index, det::kIndexLeaf);
        fs::path journal(d);
        det::append_path_leaf(journal, det::kJournalLeaf);
        fs::path tmp(index);
        tmp += det::kTmpSuffix;
        SW_CHECK(index.native() == (d / "index.bin").native());
        SW_CHECK(journal.native() == (d / "journal.bin").native());
        SW_CHECK(tmp.native() == (d / "index.bin.tmp").native());
    }
}

// Journal::open_in_place reabre DENTRO de um Journal já usado e o deixa como um recém-aberto
// (contadores e falhas injetadas zerados), com o mesmo conteúdo que open() leria.
void test_open_in_place_resets_state()
{
    std::cout << "[Test] Journal::open_in_place deixa o Journal como um recem-aberto" << std::endl;
    const fs::path dir = fresh_dir("inplace");
    const fs::path file = dir / "journal.bin";
    det::JournalReplay first_info;
    auto opened = det::Journal::open(file, 0, [](const det::JournalRecord&) {}, first_info);
    SW_CHECK(opened.has_value());
    det::Journal j = std::move(*opened);
    det::JournalRecord rec;
    rec.type = det::JournalType::FreeSlot;
    rec.lsn = 5;
    rec.piece = piece(1).hash;
    SW_CHECK(j.append(rec, nullptr, nullptr) == CacheErrorCode::Success);
    SW_CHECK(j.sync() == CacheErrorCode::Success);
    j.fail_next_sync();
    j.fail_next_append(CacheErrorCode::DiskFull);
    SW_CHECK(j.syncs() == 1);
    const uint64_t size_with_record = j.size_bytes();
    SW_CHECK(j.close() == CacheErrorCode::Success);

    uint64_t applied = 0;
    det::JournalReplay info;
    SW_CHECK(det::Journal::open_in_place(
                 j, file, 0, [&applied](const det::JournalRecord&) { ++applied; }, info) ==
             CacheErrorCode::Success);
    SW_CHECK(applied == 1 && info.applied == 1 && info.last_lsn == 5);
    SW_CHECK(j.syncs() == 0);                      // contador zerado
    SW_CHECK(j.size_bytes() == size_with_record);  // releu o registro
    SW_CHECK(j.sync() == CacheErrorCode::Success); // a falha injetada de sync foi apagada
    SW_CHECK(j.append(rec, nullptr, nullptr) == CacheErrorCode::Success); // e a de append
    SW_CHECK(j.close() == CacheErrorCode::Success);
    cleanup(dir);
}

// Linha "[F1-71] STL:" para o log do CI (compilador, depuração de iteradores e NDEBUG): diz em que
// configuração a varredura rodou (no MSVC Debug o proxy dos iteradores é alocado por construtores
// `noexcept`, e é só ali que uma regressão da Regra R aparece).
void print_stl_facts()
{
    std::cout << "[F1-71] STL:";
#if defined(_MSC_FULL_VER)
    std::cout << " _MSC_FULL_VER=" << _MSC_FULL_VER;
#endif
#if defined(__GNUC__) && !defined(__clang__)
    std::cout << " __GNUC__=" << __GNUC__;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    std::cout << " _ITERATOR_DEBUG_LEVEL=" << _ITERATOR_DEBUG_LEVEL;
#endif
#if defined(NDEBUG)
    std::cout << " NDEBUG=1";
#else
    std::cout << " NDEBUG=0";
#endif
    std::cout << std::endl;
}

} // namespace

int main()
{
    oom::make_abort_fail_instead_of_hang();
    print_stl_facts();
    const SweepResult empty = sweep(Scenario::Empty, false);
    const SweepResult pieces = sweep(Scenario::Pieces, false);
    const SweepResult freed = sweep(Scenario::PiecesFreed, false);
    const SweepResult destructor = sweep(Scenario::Pieces, true);
    // Mais peças => mais cópias de checksums no checkpoint => mais pontos de alocação. Se isto
    // falhasse, a varredura não estaria vendo a cópia do índice (o que o teste existe para cobrir).
    SW_CHECK(pieces.fired_points > empty.fired_points);
    SW_CHECK(freed.fired_points >= pieces.fired_points);
    SW_CHECK(destructor.fired_points == pieces.fired_points);
    // Pisos baixos (a contagem real depende da STL: 28/34 no libstdc++, mais no MSVC Debug): uma
    // varredura que deixasse de ver o checkpoint cairia abaixo deles.
    SW_CHECK(empty.fired_points >= 5);
    SW_CHECK(pieces.fired_points >= 10);
    test_native_leaf_matches_operator_slash();
    test_open_in_place_resets_state();
    test_control_without_fault();
    test_injected_exception_in_each_step();
    test_first_error_wins();
    test_close_errors_are_returned_and_first_wins();
    test_broken_cache_skips_checkpoint_on_stop();
    test_close_boundaries_do_not_allocate();
    test_other_stop_path_destructors_do_not_allocate();
    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA E EXCECAO DO stop() (F1-71) PASSARAM"
              << std::endl;
    return 0;
}
