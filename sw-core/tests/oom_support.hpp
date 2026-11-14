// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Apoio das varreduras de falta de memória (F1-69): substitui o operator new global DO EXECUTÁVEL
// que inclui este cabeçalho (inclua em UM arquivo só por executável) por um que falha com
// std::bad_alloc na alocação de número K contada a partir do armamento. Também traz a contagem de
// handles/descritores abertos e o diagnóstico de std::terminate.
//
// Substitui o CONJUNTO COMPLETO de operator new/delete (normal, [], nothrow, align_val_t e
// align_val_t+nothrow, e os delete normal, dimensionado, nothrow e alinhado), todos com pares
// compatíveis malloc/free (aligned_alloc/free, ou _aligned_malloc/_aligned_free no Windows), para
// que sanitizers (alloc-dealloc-mismatch) e o contador enxerguem toda alocação do executável.
//
// IMPORTANTE (o que isto prova e o que não prova): a varredura prova o comportamento sob falha de
// alocação injetada em CADA ponto do cenário exercitado. A instrumentação de diagnóstico
// (set_terminate, _set_abort_behavior, pilha por DbgHelp, levantamento de pilhas) NÃO é evidência
// de ausência de alocação: a varredura só mostra o que o cenário exercitado faz, e o levantamento
// só informa os caminhos executados naquela passada. A evidência é o resultado por K e o contador
// de alocações (zero em mover e soltar).
#pragma once

#include "check.hpp"

// Levantamento com símbolos (DbgHelp): ligado no MSVC; pode ser ligado em outro compilador do
// Windows com -DSW_SURVEY_SYMBOLS=1 e -ldbghelp (usado para compilar e exercitar esse ramo no GCC).
#if !defined(SW_SURVEY_SYMBOLS)
#if defined(_MSC_VER)
#define SW_SURVEY_SYMBOLS 1
#else
#define SW_SURVEY_SYMBOLS 0
#endif
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#if SW_SURVEY_SYMBOLS
#include <dbghelp.h>
#endif
#else
#include <dirent.h>
#endif

namespace oom {

inline std::atomic<std::int64_t> g_fail_at{0}; // 0 = desarmado; K > 0: falha na K-ésima alocação
inline std::atomic<std::int64_t> g_seen{0};    // alocações vistas desde o armamento
inline std::atomic<bool> g_fired{false};
inline std::atomic<std::uint64_t> g_total{0}; // todas as alocações (para provar "não aloca")
// Onde a varredura está, para o std::terminate dizer em que ponto a exceção escapou.
inline std::atomic<std::int64_t> g_progress_k{0};
inline std::atomic<const char*> g_progress_scenario{"(fora da varredura)"};

// Códigos de saída do processo-filho da varredura (modo por K, Windows/MSVC Debug). O abort() do
// MSVC sai com 3 e o SW_CHECK com 1: por isso o terminate usa um código só dele.
inline constexpr int kExitFiredOk = 0; // a falha disparou em K e todas as conferências passaram
inline constexpr int kExitNotFiredOk = 10; // a operação terminou ANTES de K: fim do cenário
inline constexpr int kExitTerminate = 20; // std::terminate (exceção escapou de função noexcept)
inline constexpr int kExitUsage = 2;      // argumentos inválidos do filho

// Pilha da alocação em que a falha injetada DISPAROU (só Windows): capturada dentro do alocador,
// sem alocar, uma vez por armamento. O filho a imprime em RVAs (endereço - base do executável) e o
// PAI resolve os símbolos: o filho nunca carrega DbgHelp.
inline constexpr std::size_t kFireFrames = 32;
inline void* g_fire_frames[kFireFrames];
inline std::atomic<unsigned short> g_fire_depth{0};
// true no processo-filho: o terminate imprime RVAs (sem DbgHelp) e sai com kExitTerminate.
inline std::atomic<bool> g_child_mode{false};

// ------------------------------------------------------- levantamento dos caminhos executados
// Captura DENTRO do alocador, sem alocar e sem recursão: registros em armazenamento estático de
// capacidade finita (o que passar da capacidade é CONTADO em `dropped`, nunca perdido em silêncio).
// Sem DbgHelp aqui dentro: só RtlCaptureStackBackTrace (MSVC). Os símbolos são resolvidos DEPOIS,
// com a captura desligada (alloc_survey.hpp). Fora do MSVC só se contam alocações por etapa.
namespace survey {
inline constexpr std::size_t kCapacity = 4096;
inline constexpr std::size_t kFrames = 40;
inline constexpr std::size_t kMaxStages = 32;

struct Record
{
    const char* scenario;
    const char* stage;
    unsigned short depth;
    void* frames[kFrames];
};

struct StageCount
{
    const char* stage;
    std::size_t n;
};

inline Record g_records[kCapacity];
inline StageCount g_stages[kMaxStages];
inline std::atomic<std::size_t> g_seen{0}; // alocações vistas com a captura ligada
inline std::atomic<bool> g_on{false};
inline std::atomic<const char*> g_stage{"-"};
inline std::atomic<const char*> g_scenario{"-"};
inline thread_local bool t_busy = false; // guarda de reentrância (constante, não aloca)

inline void on_alloc() noexcept
{
    if (!g_on.load(std::memory_order_relaxed) || t_busy) return;
    t_busy = true;
    const std::size_t i = g_seen.fetch_add(1, std::memory_order_relaxed);
    const char* stage = g_stage.load(std::memory_order_relaxed);
    for (std::size_t k = 0; k < kMaxStages; ++k) {
        if (g_stages[k].stage == stage) {
            ++g_stages[k].n;
            break;
        }
        if (g_stages[k].stage == nullptr) {
            g_stages[k].stage = stage;
            g_stages[k].n = 1;
            break;
        }
    }
    if (i < kCapacity) {
        Record& r = g_records[i];
        r.scenario = g_scenario.load(std::memory_order_relaxed);
        r.stage = stage;
        r.depth = 0;
#if SW_SURVEY_SYMBOLS
        r.depth = ::RtlCaptureStackBackTrace(0, static_cast<DWORD>(kFrames), r.frames, nullptr);
#endif
    }
    t_busy = false;
}

inline void reset_and_start(const char* scenario)
{
    g_on.store(false);
    for (auto& r : g_records)
        r = Record{};
    for (auto& s : g_stages)
        s = StageCount{};
    g_seen.store(0);
    g_stage.store("inicio");
    g_scenario.store(scenario);
    g_on.store(true);
}
inline void stop() { g_on.store(false); }
inline std::size_t recorded() { return g_seen.load() < kCapacity ? g_seen.load() : kCapacity; }
inline std::size_t dropped() { return g_seen.load() > kCapacity ? g_seen.load() - kCapacity : 0; }
} // namespace survey

inline void arm(std::int64_t k)
{
    g_seen.store(0);
    g_fired.store(false);
    g_fail_at.store(k);
}
inline void disarm() { g_fail_at.store(0); }

// Handles (Windows) ou descritores (Linux) abertos no processo: detecta o arquivo vazado mesmo
// quando ele NÃO está travado.
inline std::size_t open_handles()
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

// A primeira exceção lançada no processo carrega o desenrolamento de pilha; fora da conta.
inline void warm_up_exceptions()
{
    try {
        arm(1);
        (void)::operator new(8);
    } catch (const std::bad_alloc&) {
    }
    disarm();
}

#if defined(_MSC_VER)
// Pilha do ponto onde a exceção escapou (símbolos do PDB do Debug), para o CI mostrar de onde veio.
inline void print_stack_msvc()
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

// Linha "FRAMES|<rva hex>,<rva hex>..." da pilha em que a falha disparou (Windows): endereço menos
// a base do executável. O processo PAI resolve os símbolos do MESMO executável.
inline void print_fire_frames_rva()
{
#if defined(_WIN32)
    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const unsigned short n = g_fire_depth.load();
    std::fprintf(stdout, "FRAMES|");
    for (unsigned short i = 0; i < n; ++i)
        std::fprintf(stdout, "%s%llx", i == 0 ? "" : ",",
                     static_cast<unsigned long long>(
                         reinterpret_cast<std::uintptr_t>(g_fire_frames[i]) - base));
    std::fprintf(stdout, "\n");
#endif
}

// Uma exceção que escape de uma função noexcept vira std::terminate. No MSVC Debug o abort()
// abriria uma caixa de diálogo e travaria o CI até o timeout: aqui o teste termina na hora, diz
// em que ponto da varredura foi e (MSVC) imprime a pilha. No processo-filho da varredura por K
// (g_child_mode) imprime só as RVAs da pilha da injeção e sai com kExitTerminate.
inline void on_terminate()
{
    g_fail_at.store(0); // não falhar de novo enquanto diagnostica
    std::fprintf(stderr,
                 "std::terminate: excecao escapou de uma funcao noexcept (cenario '%s', K=%lld)\n",
                 g_progress_scenario.load(), static_cast<long long>(g_progress_k.load()));
    if (g_child_mode.load()) {
        // Filho: sem DbgHelp (o pai resolve). O terminate acontece no ponto da injeção.
        print_fire_frames_rva();
        std::fflush(nullptr);
        std::_Exit(kExitTerminate);
    }
#if defined(_MSC_VER)
    print_stack_msvc();
#endif
    std::fflush(nullptr);
    std::_Exit(kExitTerminate);
}

inline void make_abort_fail_instead_of_hang()
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

} // namespace oom

// ------------------------------------------------------- operator new/delete (conjunto completo)
// Funções de substituição não podem ser `inline` ([replacement.functions]): por isso este
// cabeçalho entra em UM arquivo só por executável.
namespace oom {
// true => esta alocação deve FALHAR (injeção K-ésima). Também conta e alimenta o levantamento.
inline bool should_fail() noexcept
{
    g_total.fetch_add(1, std::memory_order_relaxed);
    survey::on_alloc();
    const std::int64_t at = g_fail_at.load(std::memory_order_relaxed);
    if (at > 0 && g_seen.fetch_add(1, std::memory_order_relaxed) + 1 == at) {
#if defined(_WIN32)
        g_fire_depth.store(
            ::RtlCaptureStackBackTrace(0, static_cast<DWORD>(kFireFrames), g_fire_frames, nullptr),
            std::memory_order_relaxed);
#endif
        g_fired.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}
inline void* raw_alloc(std::size_t n) noexcept
{
    if (should_fail()) return nullptr;
    return std::malloc(n == 0 ? 1 : n);
}
inline void* raw_alloc_aligned(std::size_t n, std::size_t align) noexcept
{
    if (should_fail()) return nullptr;
    if (n == 0) n = 1;
#if defined(_WIN32)
    return ::_aligned_malloc(n, align);
#else
    return std::aligned_alloc(align, (n + align - 1) / align * align);
#endif
}
inline void raw_free(void* p) noexcept { std::free(p); }
inline void raw_free_aligned(void* p) noexcept
{
#if defined(_WIN32)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace oom

// Lançam bad_alloc (a falha injetada também).
void* operator new(std::size_t n)
{
    if (void* p = oom::raw_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
    if (void* p = oom::raw_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al)
{
    if (void* p = oom::raw_alloc_aligned(n, static_cast<std::size_t>(al))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t al)
{
    if (void* p = oom::raw_alloc_aligned(n, static_cast<std::size_t>(al))) return p;
    throw std::bad_alloc();
}
// nothrow: a falha injetada devolve nullptr (e conta).
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return oom::raw_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return oom::raw_alloc(n); }
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    return oom::raw_alloc_aligned(n, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    return oom::raw_alloc_aligned(n, static_cast<std::size_t>(al));
}

// O GCC (-O2) acusa "free() em ponteiro de alocação incompatível" ao inlinar o operator new
// daqui: falso positivo, new e delete aqui são o par malloc/free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { oom::raw_free(p); }
void operator delete[](void* p) noexcept { oom::raw_free(p); }
void operator delete(void* p, std::size_t) noexcept { oom::raw_free(p); }
void operator delete[](void* p, std::size_t) noexcept { oom::raw_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { oom::raw_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { oom::raw_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { oom::raw_free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { oom::raw_free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { oom::raw_free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    oom::raw_free_aligned(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    oom::raw_free_aligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    oom::raw_free_aligned(p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
