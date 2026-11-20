// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-72: `CacheOwner::start()` (e a recuperação, `recover()`) sem terminar o processo e sem deixar
// nada preso por falta de memória, exceção ou falha parcial (ADR-0018: bad_alloc => OutOfMemory,
// outra exceção => Internal, erro de disco => IoError/DiskFull).
//
// O que este executável prova:
//  1. Varredura de operator new SEM pontos excluídos: para K = 1, 2, ... a K-ésima alocação feita
//     DURANTE o start() falha (std::bad_alloc), em quatro cenários (pasta vazia; 4 peças com o
//     índice consolidado; journal a reaplicar, com uma remoção e um block_count do manifesto que
//     não confere; teto reduzido, que apaga um store-NNN.bin e encurta outro). Em todo ponto que
//     dispara: nenhuma exceção sai e nenhum std::terminate; start() devolve OutOfMemory; nenhum
//     arquivo fica aberto; a memória alocada volta ao nível anterior (contador de operator new
//     menos operator delete); nenhuma thread fica viva; a pasta REABRE com exatamente as peças
//     esperadas, cada uma lida e conferida bloco a bloco. Quando o K passa do último ponto, o
//     start() termina em Success. O config é montado ANTES de armar e passado por referência: se
//     start() voltasse a receber o config por valor, a cópia do caminho (no chamador) seria a 1a
//     alocação e o bad_alloc sairia.
//  2. Exceção injetada (gancho de teste) em cada passo da inicialização (abrir o store, construir a
//     Impl, reservar índice/relógio/vítimas, ler o índice, abrir o journal, descartar entradas,
//     reduzir ao teto, checkpoint, criar a CacheOwner, criar a thread), com std::runtime_error, um
//     tipo que não é std::exception, std::bad_alloc, std::system_error e um IoError devolvido: o
//     código esperado, e a mesma liberação e reabertura do item 1.
//  3. Falhas REAIS de disco no meio do start() (sem gancho): journal.bin é uma pasta; index.bin é
//     uma pasta (a troca do checkpoint falha com o journal aberto); um store-NNN.bin além do teto é
//     uma pasta com conteúdo (a redução ao teto falha). IoError, nada preso, e a pasta reabre
//     depois de desfeito o obstáculo.
//  4. Equivalências dos nomes montados sem conversão estreito->largo (Regra R): o nome do
//     store-NNN.bin em largura nativa é o de StoreFiles::file_path, e a redução ao teto continua
//     apagando só os nomes canônicos (os mesmos que o sscanf + comparação de antes aceitava).
//
// No MSVC em Debug os construtores padrão e de movimento de std::vector/std::string/fs::path são
// `noexcept` e alocam o proxy dos iteradores: um bad_alloc ali é std::terminate ANTES de qualquer
// catch (F1-63, F1-69, F1-71). Esta varredura roda no Windows Debug e Release do CI e falha por K,
// com a pilha do terminate, se algum desses construtores ainda estiver no caminho do start().
// Limite: a varredura só intercepta o operator new DESTE executável, não alocações feitas dentro
// de uma biblioteca padrão já compilada (por exemplo a libstdc++ em DLL).
#include "cache_journal.hpp"
#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "oom_support.hpp"
#include "swarmstream/cache_owner.hpp"
#include "swarmstream/expected_block_counts.hpp"
#include "swarmstream/store_files.hpp"

#include <algorithm>
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

#if defined(_WIN32)
#include <tlhelp32.h>
#endif

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;
using oom::arm;
using oom::disarm;
using oom::g_fired;
using oom::g_live;
using oom::g_progress_k;
using oom::g_progress_scenario;
using oom::open_handles;
using sw::CacheErrorCode;
using sw::CacheOwner;
using sw::CacheOwnerTestHook;
using sw::StartFault;
using sw::StartStep;

namespace {

constexpr sw::StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};

fs::path fresh_dir(const char* tag)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_72_start_") + tag + "_" +
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

// Threads do processo (para "nenhuma thread ficou viva"). POSIX: /proc/self/task, exato. Windows:
// Toolhelp, que conta também threads do carregador e do pool do sistema, que só diminuem com o
// tempo; por isso no Windows a conferência é "não aumentou".
std::size_t thread_count()
{
#if defined(_WIN32)
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    SW_CHECK(snap != INVALID_HANDLE_VALUE);
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    std::size_t n = 0;
    const DWORD pid = ::GetCurrentProcessId();
    for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) ++n;
    SW_CHECK(::CloseHandle(snap) != 0);
    return n;
#else
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec))
        ++n;
    SW_CHECK(!ec);
    return n;
#endif
}

bool threads_not_leaked(std::size_t before)
{
#if defined(_WIN32)
    return thread_count() <= before;
#else
    return thread_count() == before;
#endif
}

// Nenhum arquivo da pasta ficou aberto? No Windows, abrindo cada arquivo SEM compartilhar nada (o
// CacheOwner abre com compartilhamento de leitura): um handle ainda aberto faz a abertura falhar.
// A contagem de handles do processo não serve no Windows (o runtime do MinGW cria handles das
// variáveis de condição e só os solta depois). No POSIX a contagem de descritores é exata.
bool nothing_open([[maybe_unused]] const fs::path& dir, [[maybe_unused]] std::size_t handles_before)
{
#if defined(_WIN32)
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        HANDLE h = ::CreateFileW(it->path().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::cerr << "  arquivo ainda aberto: " << it->path().string() << std::endl;
            return false;
        }
        SW_CHECK(::CloseHandle(h) != 0);
    }
    SW_CHECK(!ec);
    return true;
#else
    return open_handles() == handles_before;
#endif
}

// ---------------------------------------------------------------------------- cenários

enum class Scenario {
    Empty,        // pasta vazia: nada no disco (journal criado, índice ausente)
    Consolidated, // 4 peças e um stop() normal: index.bin com as peças, journal só com CHECKPOINT
    Replay, // 5 peças em 2 arquivos, 1 removida, sem checkpoint final: o journal é reaplicado;
            // o manifesto em uso dá outro block_count à peça 4 (F1-70: ela sai do índice)
    Shrunk, // 9 peças em 3 arquivos; reabre com teto 5: store-002.bin apagado, store-001.bin
            // encurtado, as peças dos slots >= 5 saem
};

constexpr Scenario kScenarios[] = {Scenario::Empty, Scenario::Consolidated, Scenario::Replay,
                                   Scenario::Shrunk};

const char* name_of(Scenario s)
{
    switch (s) {
    case Scenario::Empty: return "pasta vazia";
    case Scenario::Consolidated: return "4 pecas consolidadas";
    case Scenario::Replay: return "journal a reaplicar (remocao e block_count do manifesto)";
    case Scenario::Shrunk: return "teto reduzido (apaga e encurta store)";
    }
    return "?";
}

// O vetor do manifesto em uso (F1-70) do cenário Replay: peça 0 confere (3 blocos), peça 4 não
// (2 blocos). Ordenado por hash, como start() exige. Vive o programa todo (o config só o vê).
const std::vector<sw::PieceBlockCount>& replay_block_counts()
{
    static const std::vector<sw::PieceBlockCount> v = [] {
        std::vector<sw::PieceBlockCount> out;
        out.push_back(sw::PieceBlockCount{piece(0).hash, 3});
        out.push_back(sw::PieceBlockCount{piece(4).hash, 2});
        std::sort(out.begin(), out.end(),
                  [](const sw::PieceBlockCount& a, const sw::PieceBlockCount& b) {
                      return a.piece < b.piece;
                  });
        return out;
    }();
    return v;
}

// O config com que o start() da varredura abre a pasta.
sw::CacheOwnerConfig start_config(const fs::path& dir, Scenario s)
{
    sw::CacheOwnerConfig c{.dir = dir, .total_slots = 8, .layout = kLayout};
    if (s == Scenario::Replay) c.expected_block_counts = replay_block_counts();
    if (s == Scenario::Shrunk) c.total_slots = 5;
    return c;
}

// O config com que a pasta foi POPULADA (antes do start() sob teste).
sw::CacheOwnerConfig populate_config(const fs::path& dir, Scenario s)
{
    sw::CacheOwnerConfig c{.dir = dir, .total_slots = 8, .layout = kLayout};
    if (s == Scenario::Shrunk) c.total_slots = 12;
    return c;
}

std::vector<uint32_t> expected_pieces(Scenario s)
{
    switch (s) {
    case Scenario::Empty: return {};
    case Scenario::Consolidated: return {0, 1, 2, 3};
    case Scenario::Replay: return {0, 1, 3}; // 2 removida; 4 com block_count diferente
    case Scenario::Shrunk: return {0, 1, 2, 3, 4};
    }
    return {};
}

std::vector<uint32_t> absent_pieces(Scenario s)
{
    switch (s) {
    case Scenario::Empty: return {0};
    case Scenario::Consolidated: return {4};
    case Scenario::Replay: return {2, 4};
    case Scenario::Shrunk: return {5, 6, 7, 8};
    }
    return {};
}

void prepare(const fs::path& dir, Scenario s)
{
    if (s == Scenario::Empty) return;
    auto started = CacheOwner::start(populate_config(dir, s));
    SW_CHECK(started.has_value());
    std::unique_ptr<CacheOwner> o = std::move(*started);
    switch (s) {
    case Scenario::Empty: break;
    case Scenario::Consolidated:
        for (uint32_t i = 0; i < 4; ++i)
            put(*o, i);
        break;
    case Scenario::Replay:
        for (uint32_t i = 0; i < 5; ++i)
            put(*o, i);
        remove_piece(*o, 2);
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o); // o journal fica com os 6 registros
        break;
    case Scenario::Shrunk:
        for (uint32_t i = 0; i < 9; ++i)
            put(*o, i);
        break;
    }
    SW_CHECK(o->stop().has_value());
}

// A pasta do cenário é montada UMA vez (prepare) e copiada para cada ponto K: repovoar a cada K
// custaria dois fsync por peça e deixaria a varredura lenta demais no MSVC Debug do CI.
fs::path prepared_template(Scenario s)
{
    const fs::path t = fresh_dir("modelo");
    prepare(t, s);
    return t;
}

void clone(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    SW_CHECK(!ec);
}

// Reabre a pasta (a recuperação roda de novo, sem falha) e confere: exatamente as peças esperadas,
// todas verificadas, e as que saíram continuam ausentes.
void reopen_and_verify(const fs::path& dir, Scenario s)
{
    auto started = CacheOwner::start(start_config(dir, s));
    if (!started.has_value()) {
        std::cerr << "REABERTURA FALHOU (" << name_of(s) << "): " << sw::to_string(started.error())
                  << std::endl;
    }
    SW_CHECK(started.has_value());
    std::unique_ptr<CacheOwner> o = std::move(*started);
    const std::vector<uint32_t> want = expected_pieces(s);
    SW_CHECK(o->pieces() == want.size());
    for (const uint32_t i : want)
        SW_CHECK(present_and_verified(*o, i));
    for (const uint32_t i : absent_pieces(s))
        SW_CHECK(!present_and_verified(*o, i));
    SW_CHECK(o->stop().has_value());
}

// O que não pode sobrar depois de um start() que falhou.
struct Baseline
{
    std::size_t handles = 0;
    std::int64_t live = 0;
    std::size_t threads = 0;
};

Baseline baseline()
{
    Baseline b;
    b.handles = open_handles();
    b.threads = thread_count();
    b.live = g_live.load();
    return b;
}

// `what`/`detail`/`k` só entram na mensagem de erro: montar uma std::string ANTES da conferência
// alocaria e mudaria a conta.
void check_released(const fs::path& dir, const Baseline& b, const char* what, const char* detail,
                    std::int64_t k)
{
    // A memória primeiro (as conferências abaixo alocam).
    const std::int64_t live = g_live.load();
    if (live != b.live) {
        std::cerr << "VAZAMENTO DE MEMORIA (" << what << " " << detail << " K=" << k
                  << "): " << (live - b.live)
                  << " alocacao(oes) a mais depois do start() que falhou" << std::endl;
    }
    SW_CHECK(live == b.live);
    if (!nothing_open(dir, b.handles)) {
        std::cerr << "ARQUIVO PRESO (" << what << " " << detail << " K=" << k << ")" << std::endl;
        SW_CHECK(false);
    }
    if (!threads_not_leaked(b.threads)) {
        std::cerr << "THREAD VIVA (" << what << " " << detail << " K=" << k << ")" << std::endl;
        SW_CHECK(false);
    }
}

// ---------------------------------------------------------------------------- 1. varredura de new

struct SweepResult
{
    int fired_points = 0; // pontos K em que a falha disparou
    int ok_at = 0;        // K do primeiro start() que terminou sem a falha disparar
};

SweepResult sweep(Scenario scenario)
{
    std::cout << "[Test] varredura de falta de memoria no start(): " << name_of(scenario)
              << std::endl;
    SweepResult result;
    oom::warm_up_exceptions();
    {
        // Aquecimento: um ciclo completo antes de contar (nada preguiçoso do runtime vira alocação
        // viva ou handle novo dentro da conta).
        const fs::path warm = fresh_dir("warm");
        prepare(warm, scenario);
        reopen_and_verify(warm, scenario);
        cleanup(warm);
    }
    const fs::path model = prepared_template(scenario);
    for (std::int64_t k = 1; k <= 5000; ++k) {
        const fs::path dir = fresh_dir("sweep");
        clone(model, dir);
        const sw::CacheOwnerConfig cfg = start_config(dir, scenario); // antes de armar
        const Baseline b = baseline();
        g_progress_k.store(k);
        g_progress_scenario.store(name_of(scenario));
        arm(k);
        auto started = CacheOwner::start(cfg);
        const bool fired = g_fired.load();
        disarm();

        if (!fired) {
            // O start() terminou antes da K-ésima alocação: fim da varredura deste cenário.
            if (!started.has_value()) {
                std::cerr << "start() SEM falha injetada devolveu "
                          << sw::to_string(started.error()) << " (K=" << k << ")" << std::endl;
            }
            SW_CHECK(started.has_value());
            SW_CHECK((*started)->pieces() == expected_pieces(scenario).size());
            SW_CHECK((*started)->stop().has_value());
            started = sw::Unexpected(CacheErrorCode::Stopped); // destrói a CacheOwner
            result.ok_at = static_cast<int>(k);
        } else {
            ++result.fired_points;
            if (started.has_value() || started.error() != CacheErrorCode::OutOfMemory) {
                std::cerr << "CODIGO ERRADO no ponto K=" << k << " (" << name_of(scenario)
                          << "): esperado OutOfMemory, veio "
                          << (started.has_value() ? std::string_view("Success")
                                                  : sw::to_string(started.error()))
                          << std::endl;
            }
            SW_CHECK(!started.has_value());
            SW_CHECK(started.error() == CacheErrorCode::OutOfMemory);
            check_released(dir, b, name_of(scenario), "varredura", k);
        }
        reopen_and_verify(dir, scenario);
        cleanup(dir);
        if (!fired) break;
    }
    cleanup(model);
    SW_CHECK(result.ok_at > 0);        // a varredura terminou: o start() completo foi alcançado
    SW_CHECK(result.fired_points > 0); // e houve pontos de falha de verdade (controle)
    std::cout << "  pontos de falha cobertos: " << result.fired_points << " (start() completo na "
              << result.ok_at << "a alocacao)" << std::endl;
    // Resumo para o log do CI (o ctest só mostra a saída de quem falha; o passo do ci.yml copia as
    // linhas "[F1-72]" de LastTest.log): uma queda silenciosa dos pontos fica visível, inclusive no
    // MSVC Debug.
    std::cout << "[F1-72] cenario='" << name_of(scenario)
              << "' pontos_de_falha=" << result.fired_points
              << " start_completo_na_alocacao=" << result.ok_at << std::endl;
    return result;
}

// ---------------------------------------------------------------------------- 2. exceção injetada

constexpr StartStep kSteps[] = {
    StartStep::StoreOpen,   StartStep::ImplCreate,   StartStep::Reserve, StartStep::ReadIndex,
    StartStep::JournalOpen, StartStep::DropEntries,  StartStep::Shrink,  StartStep::Checkpoint,
    StartStep::OwnerCreate, StartStep::ThreadCreate,
};

const char* step_name(StartStep s)
{
    switch (s) {
    case StartStep::StoreOpen: return "abrir o store";
    case StartStep::ImplCreate: return "construir a Impl";
    case StartStep::Reserve: return "reservar indice/relogio/vitimas";
    case StartStep::ReadIndex: return "ler o index.bin";
    case StartStep::JournalOpen: return "abrir o journal";
    case StartStep::DropEntries: return "descartar entradas";
    case StartStep::Shrink: return "reduzir ao teto";
    case StartStep::Checkpoint: return "checkpoint";
    case StartStep::OwnerCreate: return "criar a CacheOwner";
    case StartStep::ThreadCreate: return "criar a thread";
    case StartStep::Count: break;
    }
    return "?";
}

constexpr StartFault kFaults[] = {StartFault::StdException, StartFault::NonStdException,
                                  StartFault::BadAlloc, StartFault::SystemError,
                                  StartFault::IoError};

const char* fault_name(StartFault f)
{
    switch (f) {
    case StartFault::None: return "nenhuma";
    case StartFault::StdException: return "std::runtime_error";
    case StartFault::NonStdException: return "int (nao e std::exception)";
    case StartFault::BadAlloc: return "std::bad_alloc";
    case StartFault::SystemError: return "std::system_error";
    case StartFault::IoError: return "IoError devolvido";
    }
    return "?";
}

CacheErrorCode expected_for(StartFault f)
{
    switch (f) {
    case StartFault::BadAlloc: return CacheErrorCode::OutOfMemory;
    case StartFault::IoError: return CacheErrorCode::IoError;
    default: return CacheErrorCode::Internal;
    }
}

void test_injected_fault_in_each_step()
{
    std::cout << "[Test] falha injetada em cada passo do start(): codigo certo, nada preso, a "
                 "pasta reabre"
              << std::endl;
    oom::warm_up_exceptions();
    for (const Scenario scenario : {Scenario::Replay, Scenario::Shrunk}) {
        const fs::path model = prepared_template(scenario);
        for (const StartStep step : kSteps) {
            for (const StartFault fault : kFaults) {
                std::cout << "  " << name_of(scenario) << ": passo '" << step_name(step) << "' com "
                          << fault_name(fault) << std::endl;
                const fs::path dir = fresh_dir("inject");
                clone(model, dir);
                const sw::CacheOwnerConfig cfg = start_config(dir, scenario);
                const Baseline b = baseline();
                CacheOwnerTestHook::inject_start_fault(step, fault);
                auto started = CacheOwner::start(cfg);
                if (started.has_value() || started.error() != expected_for(fault)) {
                    std::cerr << "CODIGO ERRADO: esperado " << sw::to_string(expected_for(fault))
                              << ", veio "
                              << (started.has_value() ? std::string_view("Success")
                                                      : sw::to_string(started.error()))
                              << std::endl;
                }
                SW_CHECK(!started.has_value());
                SW_CHECK(started.error() == expected_for(fault));
                check_released(dir, b, step_name(step), fault_name(fault), 0);
                // A falha foi consumida pelo start() que a viu: a reabertura abre normalmente.
                reopen_and_verify(dir, scenario);
                cleanup(dir);
            }
        }
        cleanup(model);
    }
}

// Controle: sem falha injetada, todos os passos rodam e o start() dá certo (sem isto o teste acima
// passaria com um gancho que falhasse sempre). E uma falha num passo que o start() não alcança
// (o start() parou antes) não fica pendurada para o start() seguinte.
void test_control_and_fault_not_left_pending()
{
    std::cout << "[Test] controle: sem falha o start() da certo; falha nao alcancada nao fica "
                 "pendente"
              << std::endl;
    const fs::path dir = fresh_dir("control");
    prepare(dir, Scenario::Replay);
    reopen_and_verify(dir, Scenario::Replay);
    // Duas falhas: a do store aborta o start() antes de a da thread ser alcançada.
    CacheOwnerTestHook::inject_start_fault(StartStep::StoreOpen, StartFault::IoError);
    CacheOwnerTestHook::inject_start_fault(StartStep::ThreadCreate, StartFault::StdException);
    auto failed = CacheOwner::start(start_config(dir, Scenario::Replay));
    SW_CHECK(!failed.has_value() && failed.error() == CacheErrorCode::IoError);
    reopen_and_verify(dir, Scenario::Replay); // a da thread NÃO ficou para este start()
    cleanup(dir);
}

// A thread dona não pôde ser criada (std::system_error): a CacheOwner já existia, marcada como
// parada, e é destruída SEM o caminho normal do stop() (que gravaria um checkpoint de novo). Conta
// os checkpoints pelo gancho de queda: exatamente um, o do fim da recuperação.
int g_checkpoints = 0;
void count_checkpoints(sw::detail::CrashPoint point, void*)
{
    if (point == sw::detail::CrashPoint::CheckpointIndexTmp) ++g_checkpoints;
}

void test_thread_failure_does_not_checkpoint_again()
{
    std::cout << "[Test] thread que nao pode ser criada: Internal, sem um segundo checkpoint"
              << std::endl;
    const fs::path dir = fresh_dir("thread");
    prepare(dir, Scenario::Replay);
    g_checkpoints = 0;
    CacheOwnerTestHook::inject_start_fault(StartStep::ThreadCreate, StartFault::SystemError);
    auto failed = CacheOwnerTestHook::start_with_crash_hook(start_config(dir, Scenario::Replay),
                                                            count_checkpoints, nullptr);
    SW_CHECK(!failed.has_value() && failed.error() == CacheErrorCode::Internal);
    SW_CHECK(g_checkpoints == 1);
    // Controle: um start() que dá certo e um stop() normal gravam dois (recuperação e parada).
    g_checkpoints = 0;
    auto ok = CacheOwnerTestHook::start_with_crash_hook(start_config(dir, Scenario::Replay),
                                                        count_checkpoints, nullptr);
    SW_CHECK(ok.has_value());
    SW_CHECK((*ok)->stop().has_value());
    SW_CHECK(g_checkpoints == 2);
    ok = sw::Unexpected(CacheErrorCode::Stopped);
    reopen_and_verify(dir, Scenario::Replay);
    cleanup(dir);
}

// ---------------------------------------------------------------------------- 3. falhas reais

void test_real_disk_failures_release_everything()
{
    std::cout << "[Test] falhas reais de disco no meio do start(): IoError, nada preso, reabre "
                 "depois"
              << std::endl;
    {
        // journal.bin é uma pasta: abrir o journal falha (o store já foi aberto).
        std::cout << "  journal.bin e uma pasta" << std::endl;
        const fs::path dir = fresh_dir("real_journal");
        prepare(dir, Scenario::Consolidated);
        std::error_code ec;
        fs::remove(dir / "journal.bin", ec);
        SW_CHECK(!ec);
        fs::create_directory(dir / "journal.bin", ec);
        SW_CHECK(!ec);
        const sw::CacheOwnerConfig cfg = start_config(dir, Scenario::Consolidated);
        const Baseline b = baseline();
        auto started = CacheOwner::start(cfg);
        SW_CHECK(!started.has_value() && started.error() == CacheErrorCode::IoError);
        check_released(dir, b, "falha real", "journal.bin pasta", 0);
        fs::remove(dir / "journal.bin", ec);
        SW_CHECK(!ec);
        // Sem journal, o índice consolidado basta: as 4 peças voltam.
        reopen_and_verify(dir, Scenario::Consolidated);
        cleanup(dir);
    }
    {
        // index.bin é uma pasta: o índice é descartado (inválido) e a troca do checkpoint no fim
        // da recuperação falha, com o journal ABERTO e já reaplicado.
        std::cout << "  index.bin e uma pasta (checkpoint falha com o journal aberto)" << std::endl;
        const fs::path dir = fresh_dir("real_index");
        prepare(dir, Scenario::Replay);
        std::error_code ec;
        fs::remove(dir / "index.bin", ec);
        SW_CHECK(!ec);
        fs::create_directories(dir / "index.bin" / "ocupado", ec);
        SW_CHECK(!ec);
        const sw::CacheOwnerConfig cfg = start_config(dir, Scenario::Replay);
        const Baseline b = baseline();
        auto started = CacheOwner::start(cfg);
        SW_CHECK(!started.has_value() && started.error() == CacheErrorCode::IoError);
        check_released(dir, b, "falha real", "index.bin pasta", 0);
        fs::remove_all(dir / "index.bin", ec);
        SW_CHECK(!ec);
        reopen_and_verify(dir, Scenario::Replay); // o journal tem tudo
        cleanup(dir);
    }
    {
        // store-002.bin além do novo teto é uma pasta com conteúdo: a redução ao teto não consegue
        // apagá-lo (journal aberto).
        std::cout << "  store-002.bin alem do teto e uma pasta" << std::endl;
        const fs::path dir = fresh_dir("real_shrink");
        prepare(dir, Scenario::Shrunk);
        const fs::path beyond = sw::StoreFiles::file_path(dir, 2);
        std::error_code ec;
        fs::remove(beyond, ec);
        SW_CHECK(!ec);
        fs::create_directories(beyond / "ocupado", ec);
        SW_CHECK(!ec);
        const sw::CacheOwnerConfig cfg = start_config(dir, Scenario::Shrunk);
        const Baseline b = baseline();
        auto started = CacheOwner::start(cfg);
        SW_CHECK(!started.has_value() && started.error() == CacheErrorCode::IoError);
        check_released(dir, b, "falha real", "store-002.bin pasta", 0);
        fs::remove_all(beyond, ec);
        SW_CHECK(!ec);
        reopen_and_verify(dir, Scenario::Shrunk);
        cleanup(dir);
    }
}

// O journal existe, mas não abre (Windows: outro handle sem compartilhar escrita, mas com
// FILE_SHARE_DELETE; POSIX: permissão 000). A troca do checkpoint por cima dele CONSEGUIRIA ser
// feita (rename), então um start() que ignorasse a falha de abertura seguiria sem reaplicar o
// journal e consolidaria um índice SEM as peças dele (perda de indexação). Tem de dar IoError, não
// tocar no journal, e a pasta reabre com as peças do journal depois de desfeito o obstáculo.
void test_journal_that_does_not_open_is_not_ignored()
{
    std::cout << "[Test] journal que nao abre (sem ser pasta): IoError, o journal fica intacto"
              << std::endl;
    const fs::path dir = fresh_dir("real_locked");
    prepare(dir, Scenario::Replay); // índice vazio; as peças só estão no journal
    const fs::path journal = dir / "journal.bin";
    std::error_code ec;
    const auto size_before = fs::file_size(journal, ec);
    SW_CHECK(!ec);
#if defined(_WIN32)
    HANDLE blocker =
        ::CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SW_CHECK(blocker != INVALID_HANDLE_VALUE);
#else
    fs::permissions(journal, fs::perms::none, ec);
    SW_CHECK(!ec);
    if (std::FILE* probe = std::fopen(journal.c_str(), "rb")) {
        // root (ou um sistema de arquivos sem permissões): o obstáculo não vale aqui.
        SW_CHECK(std::fclose(probe) == 0);
        fs::permissions(journal, fs::perms::owner_read | fs::perms::owner_write, ec);
        SW_CHECK(!ec);
        std::cout << "  PULADO: a permissao 000 nao impede abrir (root?)" << std::endl;
        cleanup(dir);
        return;
    }
#endif
    const sw::CacheOwnerConfig cfg = start_config(dir, Scenario::Replay);
    const std::size_t handles_before = open_handles();
    const std::int64_t live_before = g_live.load();
    auto started = CacheOwner::start(cfg);
    const std::int64_t live_after = g_live.load();
#if defined(_WIN32)
    SW_CHECK(::CloseHandle(blocker) != 0);
#else
    fs::permissions(journal, fs::perms::owner_read | fs::perms::owner_write, ec);
    SW_CHECK(!ec);
#endif
    if (started.has_value()) std::cerr << "  start() IGNOROU o journal que nao abre" << std::endl;
    SW_CHECK(!started.has_value() && started.error() == CacheErrorCode::IoError);
    SW_CHECK(live_after == live_before);
    SW_CHECK(nothing_open(dir, handles_before));
    SW_CHECK(fs::file_size(journal, ec) == size_before && !ec); // intacto: não foi trocado
    reopen_and_verify(dir, Scenario::Replay);                   // as peças do journal voltam
    cleanup(dir);
}

// ---------------------------------------------------------------------------- 4. equivalências

void test_store_file_leaf_matches_file_path()
{
    std::cout << "[Test] nome nativo do store-NNN.bin == StoreFiles::file_path" << std::endl;
    const fs::path tmpdir = fs::temp_directory_path();
    for (const fs::path& d : {tmpdir, tmpdir / "sub", tmpdir / "sub" / "", fs::path("relativo")}) {
        for (const uint32_t i : {0u, 1u, 7u, 99u, 999u, 1000u, 123456u, 4294967295u}) {
            fs::path p(d);
            SW_CHECK(det::append_store_file_leaf(p, i));
            SW_CHECK(p.native() == sw::StoreFiles::file_path(d, i).native());
        }
    }
}

// A redução ao teto apaga só os store-NNN.bin de nome canônico com NNN além do teto, como antes
// (sscanf "store-%u.bin%c" e depois comparação com o nome canônico).
void test_shrink_removes_only_canonical_names()
{
    std::cout << "[Test] reduzir ao teto apaga so os nomes canonicos alem do teto" << std::endl;
    const fs::path dir = fresh_dir("names");
    const char* removed[] = {"store-002.bin", "store-010.bin", "store-1000.bin",
                             "store-4294967295.bin"};
    // ("Store-002.bin" só no POSIX: no NTFS ele é o MESMO arquivo que "store-002.bin".)
    std::vector<const char*> kept = {"store-000.bin",  "store-001.bin",        "store-2.bin",
                                     "store-02.bin",   "store-0010.bin",       "store-002.bin.bak",
                                     "store-002.binx", "store-+02.bin",        "store- 02.bin",
                                     "store-.bin",     "store-4294967296.bin", "xstore-002.bin",
                                     "store-00a.bin",  "store-99999999999.bin"};
#if !defined(_WIN32)
    kept.push_back("Store-002.bin");
#endif
    for (const char* n : removed) {
        std::FILE* f = std::fopen((dir / n).string().c_str(), "wb");
        SW_CHECK(f != nullptr);
        SW_CHECK(std::fclose(f) == 0);
    }
    for (const char* n : kept) {
        std::FILE* f = std::fopen((dir / n).string().c_str(), "wb");
        SW_CHECK(f != nullptr);
        SW_CHECK(std::fclose(f) == 0);
    }
    {
        // Teto de 8 slots com 4 por arquivo: store-000 e store-001 ficam; >= 2 sai.
        auto store = sw::StoreFiles::open(dir, 8, kLayout);
        SW_CHECK(store.has_value());
        SW_CHECK(store->shrink_to_ceiling().has_value());
        SW_CHECK(store->close().has_value());
    }
    for (const char* n : removed) {
        if (fs::exists(dir / n)) std::cerr << "  deveria ter sido apagado: " << n << std::endl;
        SW_CHECK(!fs::exists(dir / n));
    }
    for (const char* n : kept) {
        if (!fs::exists(dir / n)) std::cerr << "  nao deveria ter sido apagado: " << n << std::endl;
        SW_CHECK(fs::exists(dir / n));
    }
    cleanup(dir);
}

// Linha "[F1-72] STL:" para o log do CI: em que configuração a varredura rodou.
void print_stl_facts()
{
    std::cout << "[F1-72] STL:";
#if defined(_MSC_FULL_VER)
    std::cout << " _MSC_FULL_VER=" << _MSC_FULL_VER;
#endif
#if defined(__GNUC__) && !defined(__clang__)
    std::cout << " __GNUC__=" << __GNUC__;
#endif
#if defined(__clang__)
    std::cout << " __clang_major__=" << __clang_major__;
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
    test_store_file_leaf_matches_file_path();
    test_shrink_removes_only_canonical_names();
    test_control_and_fault_not_left_pending();
    test_thread_failure_does_not_checkpoint_again();
    test_injected_fault_in_each_step();
    test_real_disk_failures_release_everything();
    test_journal_that_does_not_open_is_not_ignored();
    SweepResult r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = sweep(kScenarios[i]);
    // Mais trabalho na recuperação => mais pontos de alocação. Se isto falhasse, a varredura não
    // estaria vendo a recuperação (o que o teste existe para cobrir).
    SW_CHECK(r[1].fired_points > r[0].fired_points); // índice com peças > pasta vazia
    SW_CHECK(r[2].fired_points > r[0].fired_points); // journal reaplicado > pasta vazia
    SW_CHECK(r[3].fired_points > r[1].fired_points); // teto reduzido (mais peças, apaga) > 4 peças
    // Piso: a contagem real depende da STL (e é maior no MSVC Debug, pelo proxy dos iteradores).
    // Os pontos estruturais do start() (store, arquivos, Impl, tabela de leases, contagens, índice,
    // relógio, vítimas, slots, journal, checkpoint, CacheOwner, thread) passam de 20 em qualquer
    // STL; uma varredura que deixasse de ver a recuperação cairia abaixo disso.
    SW_CHECK(r[0].fired_points >= 20);
    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA E EXCECAO DO start() (F1-72) PASSARAM"
              << std::endl;
    return 0;
}
