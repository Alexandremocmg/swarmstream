// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Apoio das varreduras de falta de memória (F1-69): substitui o operator new global DO EXECUTÁVEL
// que inclui este cabeçalho (inclua em UM arquivo só por executável) por um que falha com
// std::bad_alloc na alocação de número K contada a partir do armamento. Também traz a contagem de
// handles/descritores abertos e o diagnóstico de std::terminate.
//
// IMPORTANTE (o que isto prova e o que não prova): a varredura prova o comportamento sob falha de
// alocação injetada em CADA ponto do cenário exercitado. A instrumentação de diagnóstico
// (set_terminate, _set_abort_behavior, pilha por DbgHelp) NÃO é evidência de ausência de alocação:
// ela só transforma um travamento em uma falha diagnosticada. A evidência é o resultado por K e o
// contador de alocações (zero em mover e soltar).
#pragma once

#include "check.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
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

// Uma exceção que escape de uma função noexcept vira std::terminate. No MSVC Debug o abort()
// abriria uma caixa de diálogo e travaria o CI até o timeout: aqui o teste termina na hora, diz
// em que ponto da varredura foi e (MSVC) imprime a pilha.
inline void on_terminate()
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

// ----------------------------------------------------------------------- operator new que falha
// Funções de substituição não podem ser `inline` ([replacement.functions]): por isso este
// cabeçalho entra em UM arquivo só por executável.
void* operator new(std::size_t n)
{
    oom::g_total.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t at = oom::g_fail_at.load(std::memory_order_relaxed);
    if (at > 0 && oom::g_seen.fetch_add(1, std::memory_order_relaxed) + 1 == at) {
        oom::g_fired.store(true, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
// O GCC (-O2) acusa "free() em ponteiro de alocação incompatível" ao inlinar o operator new
// daqui: falso positivo, new e delete aqui são o par malloc/free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
