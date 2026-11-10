// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Injeção de falta de memória para os testes do `sw-format` (F1-73): substitui o operator new
// global DO EXECUTÁVEL que inclui este cabeçalho (inclua em UM arquivo só por executável) por um
// que falha com std::bad_alloc na alocação de número K contada a partir do armamento.
//
// ORIGEM (cópia PARCIAL, decidida pelo dono em 2026-11-10): `sw-core/tests/oom_support.hpp` e
// `sw-core/tests/alloc_survey.hpp` do PR #37 (F1-69), commit
// 6a3556871dda932eeaeaddfbfbcd1b1dff2679b7. Aquele PR não está na `main`; este arquivo existe para
// o `sw-format` ter a varredura sem depender dele.
//   Copiado de oom_support.hpp: os contadores e arm/disarm, o aquecimento das exceções, os códigos
//     de saída do processo-filho, a captura (sem alocar) da pilha da alocação em que a falha
//     disparou e a sua impressão em RVAs, o tratamento de std::terminate, a supressão das caixas
//     de diálogo do MSVC e o conjunto completo de operator new/delete.
//   Copiado de alloc_survey.hpp: só o que resolve, NO PROCESSO PAI, a pilha que o filho imprimiu
//     (`Frame`, `resolve`, `is_noise`, `is_project`, `show`, `SymbolSession`).
//   NÃO copiado: o levantamento dos caminhos executados (`oom::survey`: registros, etapas,
//     relatório), a contagem de handles, a pilha por DbgHelp dentro do processo que falha e todo o
//     resto do F1-69.
//   Diferenças: não depende de `check.hpp` do sw-core; `g_live` (alocações vivas, para o teste
//     conferir que nada vazou em qualquer plataforma) e `kCountOnly` são daqui; o terminate sempre
//     sai pelo caminho do filho (RVAs e código próprio).
// Quando o #37 for integrado, as duas cópias devem ser reconciliadas (anotado no cartão F1-73).
//
// O que isto prova e o que não prova: a varredura prova o comportamento sob falha de alocação
// injetada em CADA alocação que o cenário exercitado faz por operator new. Não prova ausência de
// alocação em caminhos que o cenário não executa, nem cobre alocação por malloc direto.
#pragma once

// Símbolos por DbgHelp (só para o pai descrever a pilha de um filho que terminou): ligado no MSVC.
#if !defined(SW_OOM_SYMBOLS)
#if defined(_MSC_VER)
#define SW_OOM_SYMBOLS 1
#else
#define SW_OOM_SYMBOLS 0
#endif
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#if SW_OOM_SYMBOLS
// clang-format off
#include <dbghelp.h>
// clang-format on
#endif
#endif

namespace oom {

inline std::atomic<std::int64_t> g_fail_at{0}; // 0 = desarmado; K > 0: falha na K-ésima alocação
inline std::atomic<std::int64_t> g_seen{0};    // alocações vistas desde o armamento
inline std::atomic<bool> g_fired{false};
inline std::atomic<std::int64_t> g_live{0}; // alocações vivas (new sem o delete correspondente)
// Onde a varredura está, para o std::terminate dizer em que ponto a exceção escapou.
inline std::atomic<std::int64_t> g_progress_k{0};
inline std::atomic<const char*> g_progress_scenario{"(fora da varredura)"};

// Armar com este K só CONTA as alocações da operação (nunca dispara).
inline constexpr std::int64_t kCountOnly = std::numeric_limits<std::int64_t>::max();

// Códigos de saída do processo-filho da varredura. O abort() do MSVC sai com 3 e a asserção do
// teste com 1: por isso o terminate usa um código só dele.
inline constexpr int kExitFiredOk = 0; // a falha disparou em K e todas as conferências passaram
inline constexpr int kExitCheckFailed = 1; // uma conferência do teste falhou
inline constexpr int kExitUsage = 2;       // argumentos inválidos do filho
inline constexpr int kExitNotFiredOk = 10; // a operação terminou ANTES de K: fim do cenário
inline constexpr int kExitTerminate = 20; // std::terminate (exceção escapou de função noexcept)

// Pilha da alocação em que a falha injetada DISPAROU (só Windows): capturada dentro do alocador,
// sem alocar, uma vez por armamento. O filho a imprime em RVAs (endereço - base do executável) e o
// PAI resolve os símbolos: o filho nunca carrega DbgHelp.
inline constexpr std::size_t kFireFrames = 32;
inline void* g_fire_frames[kFireFrames];
inline std::atomic<unsigned short> g_fire_depth{0};

inline void arm(std::int64_t k)
{
    g_seen.store(0);
    g_fired.store(false);
    g_fire_depth.store(0);
    g_fail_at.store(k);
}
inline void disarm() { g_fail_at.store(0); }

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
// abriria uma caixa de diálogo e travaria o CI até o timeout: aqui o processo termina na hora, diz
// em que ponto da varredura foi, imprime as RVAs da pilha da injeção (o terminate acontece no
// ponto da injeção) e sai com kExitTerminate.
inline void on_terminate()
{
    g_fail_at.store(0); // não falhar de novo enquanto diagnostica
    std::fprintf(stderr,
                 "std::terminate: excecao escapou de uma funcao noexcept (cenario '%s', K=%lld)\n",
                 g_progress_scenario.load(), static_cast<long long>(g_progress_k.load()));
    print_fire_frames_rva();
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

// ------------------------------------------------------- símbolos no processo PAI (só MSVC)
#if SW_OOM_SYMBOLS
struct Frame
{
    std::string name;
    std::string file;
    unsigned line = 0;
};

inline Frame resolve(HANDLE process, void* addr)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    Frame f;
    const DWORD64 a = reinterpret_cast<DWORD64>(addr);
    DWORD64 disp = 0;
    f.name = ::SymFromAddr(process, a, &disp, sym) ? sym->Name : "?";
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    if (::SymGetLineFromAddr64(process, a, &ldisp, &line)) {
        f.file = line.FileName;
        f.line = line.LineNumber;
    }
    return f;
}

inline bool contains(const std::string& s, const char* what) { return s.find(what) != s.npos; }

inline bool is_noise(const Frame& f)
{
    return contains(f.file, "oom_inject.hpp") || f.name.rfind("oom::", 0) == 0 ||
           f.name == "operator new" || f.name == "operator delete" ||
           f.name.rfind("operator new", 0) == 0;
}

// Frame do código do projeto (sw-format, testes), não da STL/CRT.
inline bool is_project(const Frame& f)
{
    return !f.file.empty() && !contains(f.file, "Microsoft Visual Studio") &&
           !contains(f.file, "vctools") && !contains(f.file, "ucrt");
}

inline std::string show(const Frame& f)
{
    std::string s = f.name;
    if (!f.file.empty()) s += " (" + f.file + ":" + std::to_string(f.line) + ")";
    return s;
}

// Resolve, NO PROCESSO PAI, as pilhas que o filho imprimiu em RVAs (endereço - base do executável):
// o executável é o MESMO arquivo, então base_do_pai + RVA é o mesmo código. Um DbgHelp por sessão.
class SymbolSession
{
public:
    SymbolSession()
    {
        process_ = ::GetCurrentProcess();
        ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        ok_ = ::SymInitialize(process_, nullptr, TRUE) != FALSE;
        base_ = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base_ + dos->e_lfanew);
        size_ = nt->OptionalHeader.SizeOfImage;
    }
    SymbolSession(const SymbolSession&) = delete;
    SymbolSession& operator=(const SymbolSession&) = delete;
    ~SymbolSession()
    {
        if (ok_) ::SymCleanup(process_);
    }

    // Do frame mais interno até o 2o frame do projeto (inclusive), sem o ruído do alocador, no
    // máximo 12 frames.
    [[nodiscard]] std::string describe(const std::vector<std::uint64_t>& rvas)
    {
        if (!ok_) return "(DbgHelp indisponivel)";
        std::string out;
        int shown = 0;
        int project = 0;
        for (const std::uint64_t rva : rvas) {
            if (rva >= size_) continue; // frame fora do executável (ntdll, ucrt, ...)
            const Frame f = resolve(process_, reinterpret_cast<void*>(base_ + rva));
            if (is_noise(f)) continue;
            if (!out.empty()) out += " <- ";
            out += show(f);
            if (is_project(f)) ++project;
            if (project >= 2 || ++shown >= 12) break;
        }
        return out.empty() ? std::string("(sem simbolos)") : out;
    }

private:
    HANDLE process_ = nullptr;
    bool ok_ = false;
    std::uintptr_t base_ = 0;
    std::uint64_t size_ = 0;
};
#else
class SymbolSession
{
public:
    [[nodiscard]] std::string describe(const std::vector<std::uint64_t>&) const
    {
        return "(sem simbolos neste compilador)";
    }
};
#endif

// ------------------------------------------------------- operator new/delete (conjunto completo)
// true => esta alocação deve FALHAR (injeção K-ésima).
inline bool should_fail() noexcept
{
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
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p != nullptr) g_live.fetch_add(1, std::memory_order_relaxed);
    return p;
}
inline void* raw_alloc_aligned(std::size_t n, std::size_t align) noexcept
{
    if (should_fail()) return nullptr;
    if (n == 0) n = 1;
#if defined(_WIN32)
    void* p = ::_aligned_malloc(n, align);
#else
    void* p = std::aligned_alloc(align, (n + align - 1) / align * align);
#endif
    if (p != nullptr) g_live.fetch_add(1, std::memory_order_relaxed);
    return p;
}
inline void raw_free(void* p) noexcept
{
    if (p != nullptr) g_live.fetch_sub(1, std::memory_order_relaxed);
    std::free(p);
}
inline void raw_free_aligned(void* p) noexcept
{
    if (p != nullptr) g_live.fetch_sub(1, std::memory_order_relaxed);
#if defined(_WIN32)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace oom

// Funções de substituição não podem ser `inline` ([replacement.functions]): por isso este
// cabeçalho entra em UM arquivo só por executável. Lançam bad_alloc (a falha injetada também).
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
