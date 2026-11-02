// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69: varredura de falta de memória no lock da pasta de cache.
//
// Substitui o operator new global DESTE executável por um que falha (bad_alloc) na alocação de
// número K contada a partir do armamento. Para cada K = 1, 2, ... até a aquisição passar sem que
// a falha dispare, mostra que:
//   1. uma aquisição que falha (qualquer código) NÃO deixa o lock nem o arquivo aberto: o objeto
//      fica sem posse, a contagem de handles/descritores do processo volta à de antes, e
//   2. uma NOVA aquisição na mesma pasta, no mesmo processo, funciona (o lock é por abertura:
//      um handle vazado e travado a faria receber "bloqueado").
// Cobre a pasta que já existe, a pasta que falta (create_directories) e a pasta já travada por
// outro dono (o caminho "bloqueado" também monta texto e pode faltar memória).
// Também prova as fronteiras noexcept: mover e soltar um lock não alocam nada (uma alocação
// escondida dentro de um construtor de movimento "noexcept" derrubaria o processo, como o proxy
// de iteradores do MSVC Debug).
#include "cache_dir_lock.hpp"
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <dbghelp.h>
#endif
#else
#include <dirent.h>
#endif

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;

// ----------------------------------------------------------------------- operator new que falha
namespace {
std::atomic<std::int64_t> g_fail_at{0}; // 0 = desarmado; K > 0: falha na K-ésima alocação
std::atomic<std::int64_t> g_seen{0};    // alocações vistas desde o armamento
std::atomic<bool> g_fired{false};
std::atomic<std::uint64_t> g_total{0}; // todas as alocações (para provar "não aloca")
// Onde a varredura está, para o std::terminate dizer em que ponto a exceção escapou.
std::atomic<std::int64_t> g_progress_k{0};
std::atomic<const char*> g_progress_scenario{"(fora da varredura)"};
} // namespace

void* operator new(std::size_t n)
{
    g_total.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t at = g_fail_at.load(std::memory_order_relaxed);
    if (at > 0 && g_seen.fetch_add(1, std::memory_order_relaxed) + 1 == at) {
        g_fired.store(true, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

void arm(std::int64_t k)
{
    g_seen.store(0);
    g_fired.store(false);
    g_fail_at.store(k);
}
void disarm() { g_fail_at.store(0); }

// Handles (Windows) ou descritores (Linux) abertos no processo: detecta o arquivo vazado mesmo
// quando ele NÃO está travado (o caminho "bloqueado" fecha o handle antes de montar o texto).
std::size_t open_handles()
{
#if defined(_WIN32)
    DWORD n = 0;
    SW_CHECK(::GetProcessHandleCount(::GetCurrentProcess(), &n) != 0);
    return static_cast<std::size_t>(n);
#else
    std::size_t n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    SW_CHECK(d != nullptr);
    while (::readdir(d) != nullptr)
        ++n;
    SW_CHECK(::closedir(d) == 0);
    return n;
#endif
}

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_69oom_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

enum class Scenario { ExistingDir, MissingDir, LockedByOther };

const char* name_of(Scenario s)
{
    switch (s) {
    case Scenario::ExistingDir: return "pasta existente";
    case Scenario::MissingDir: return "pasta que falta";
    case Scenario::LockedByOther: return "pasta travada por outro dono";
    }
    return "?";
}

// Roda a varredura de um cenário. Devolve quantas alocações a aquisição faz (N).
int sweep(Scenario scenario)
{
    std::cout << "[Test] varredura de falta de memoria: " << name_of(scenario) << std::endl;
    const fs::path dir = fresh_dir("sweep");
    if (scenario != Scenario::MissingDir) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        SW_CHECK(!ec);
    }
    det::CacheDirLock other;
    if (scenario == Scenario::LockedByOther) {
        SW_CHECK(det::CacheDirLock::acquire(dir, other).ok());
        SW_CHECK(other.held());
    }

    // Aquecimento: uma aquisição completa (ou "bloqueado") antes de contar, para que nenhuma
    // inicialização preguiçosa do runtime apareça como handle novo.
    {
        det::CacheDirLock warm;
        const det::LockStatus w = det::CacheDirLock::acquire(dir, warm);
        SW_CHECK(w.ok() || scenario == Scenario::LockedByOther);
        if (warm.held()) SW_CHECK(warm.release().ok());
        if (scenario == Scenario::MissingDir) cleanup(dir);
    }

    // A primeira exceção lançada no processo carrega o desenrolamento de pilha; fora da conta.
    try {
        arm(1);
        (void)::operator new(8);
    } catch (const std::bad_alloc&) {
    }
    disarm();

    int failed_points = 0;
    int ok_at = 0;
    int oom_points = 0;
    for (std::int64_t k = 1; k <= 400; ++k) {
        // Cenário da pasta que falta: recria o estado inicial a cada ponto.
        if (scenario == Scenario::MissingDir) cleanup(dir);

        // Aquecimento fora da medida: nenhuma alocação preguiçosa do runtime entra na conta.
        const std::size_t handles_before = open_handles();

        det::CacheDirLock victim;
        g_progress_k.store(k);
        g_progress_scenario.store(name_of(scenario));
        arm(k);
        const det::LockStatus st = det::CacheDirLock::acquire(dir, victim);
        const bool fired = g_fired.load();
        disarm();

        if (!fired) {
            // A aquisição terminou antes da K-ésima alocação: acabou a varredura.
            if (scenario == Scenario::LockedByOther) {
                SW_CHECK(st.code == det::LockCode::Locked);
                SW_CHECK(!victim.held());
            } else {
                SW_CHECK(st.ok());
                SW_CHECK(victim.held());
                SW_CHECK(victim.release().ok());
            }
            SW_CHECK(open_handles() == handles_before);
            ok_at = static_cast<int>(k);
            break;
        }

        ++failed_points;
        if (st.code == det::LockCode::OutOfMemory) ++oom_points;
        // 1. Falhou: não há posse e nenhum arquivo ficou aberto.
        SW_CHECK(!st.ok());
        SW_CHECK(!victim.held());
        if (open_handles() != handles_before) {
            std::cerr << "VAZAMENTO no ponto K=" << k << " (" << name_of(scenario)
                      << "): handles antes=" << handles_before << " depois=" << open_handles()
                      << " codigo=" << static_cast<int>(st.code) << std::endl;
        }
        SW_CHECK(open_handles() == handles_before);

        // 2. Uma NOVA aquisição, no mesmo processo, funciona (ou, se outro dono segura de
        //    propósito, recebe "bloqueado" e não E/S nem sucesso).
        det::CacheDirLock again;
        const det::LockStatus st2 = det::CacheDirLock::acquire(dir, again);
        if (scenario == Scenario::LockedByOther) {
            SW_CHECK(st2.code == det::LockCode::Locked);
            SW_CHECK(!again.held());
        } else {
            if (!st2.ok()) {
                std::cerr << "NOVA AQUISICAO FALHOU depois da falha no ponto K=" << k << " ("
                          << name_of(scenario) << "): codigo=" << static_cast<int>(st2.code) << " "
                          << st2.detail << std::endl;
            }
            SW_CHECK(st2.ok());
            SW_CHECK(again.held());
            SW_CHECK(again.release().ok());
        }
    }
    SW_CHECK(ok_at > 0); // a varredura terminou: N alocações cobertas
    SW_CHECK(failed_points >
             0); // e houve pontos de falha de verdade (controle de que sabemos falhar)
    SW_CHECK(oom_points > 0);
    std::cout << "  pontos de falha cobertos: " << failed_points << " (aquisicao completa na "
              << ok_at << "a alocacao)" << std::endl;

    if (scenario == Scenario::LockedByOther) SW_CHECK(other.release().ok());
    cleanup(dir);
    return ok_at;
}

// Mover e soltar não alocam: nada que possa lançar dentro de uma função noexcept.
void test_move_and_release_do_not_allocate()
{
    std::cout << "[Test] mover e soltar o lock nao alocam (fronteiras noexcept)" << std::endl;
    const fs::path dir = fresh_dir("move");
    det::CacheDirLock a;
    SW_CHECK(det::CacheDirLock::acquire(dir, a).ok());

    const std::uint64_t before = g_total.load();
    det::CacheDirLock b(std::move(a)); // construtor de movimento
    det::CacheDirLock c;
    c = std::move(b); // atribuição de movimento
    const std::uint64_t after_moves = g_total.load();
    SW_CHECK(after_moves == before);
    SW_CHECK(!a.held() && !b.held() && c.held());

    const det::LockStatus rel = c.release();
    const std::uint64_t after_release = g_total.load();
    SW_CHECK(rel.ok());
    SW_CHECK(after_release == after_moves);

    // Controle positivo: o contador enxerga uma alocação de verdade.
    const std::uint64_t pre = g_total.load();
    void* volatile probe = ::operator new(16);
    SW_CHECK(g_total.load() == pre + 1);
    ::operator delete(probe);
    cleanup(dir);
}

#if defined(_MSC_VER)
// Pilha do ponto onde a exceção escapou (símbolos do PDB do Debug), para o CI mostrar de onde veio.
void print_stack_msvc()
{
    void* frames[48] = {};
    const USHORT n = ::CaptureStackBackTrace(0, 48, frames, nullptr);
    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!::SymInitialize(process, nullptr, TRUE)) {
        std::fputs("(SymInitialize falhou; sem pilha)\n", stderr);
        return;
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    for (USHORT i = 0; i < n; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 disp = 0;
        const char* name = ::SymFromAddr(process, addr, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        if (::SymGetLineFromAddr64(process, addr, &ldisp, &line))
            std::fprintf(stderr, "  #%u %s (%s:%lu)\n", static_cast<unsigned>(i), name,
                         line.FileName, static_cast<unsigned long>(line.LineNumber));
        else
            std::fprintf(stderr, "  #%u %s\n", static_cast<unsigned>(i), name);
    }
}
#endif

// Uma exceção que escape de uma função noexcept vira std::terminate. No MSVC Debug o abort()
// abriria uma caixa de diálogo e travaria o CI até o timeout: aqui o teste termina na hora, diz
// em que ponto da varredura foi e (MSVC) imprime a pilha.
void on_terminate()
{
    g_fail_at.store(0); // não falhar de novo enquanto diagnostica
    std::fprintf(stderr,
                 "std::terminate: excecao escapou de uma funcao noexcept (cenario '%s', K=%lld)\n",
                 g_progress_scenario.load(), static_cast<long long>(g_progress_k.load()));
#if defined(_MSC_VER)
    print_stack_msvc();
#endif
    std::fflush(nullptr);
    std::_Exit(1);
}

void make_abort_fail_instead_of_hang()
{
    std::set_terminate(on_terminate);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
}

} // namespace

int main()
{
    make_abort_fail_instead_of_hang();
    const int n_existing = sweep(Scenario::ExistingDir);
    const int n_missing = sweep(Scenario::MissingDir);
    const int n_locked = sweep(Scenario::LockedByOther);
    SW_CHECK(n_missing >= n_existing); // criar a pasta só soma alocações
    SW_CHECK(n_locked > 1);
    test_move_and_release_do_not_allocate();
    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA DO LOCK PASSARAM" << std::endl;
    return 0;
}
