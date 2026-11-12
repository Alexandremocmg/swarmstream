// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Relatório do LEVANTAMENTO DOS CAMINHOS EXECUTADOS (F1-69). A captura acontece dentro do
// alocador (oom_support.hpp, sem alocar, sem recursão, capacidade finita); aqui, com a captura já
// DESLIGADA e sem injeção de falha, as pilhas iguais são agrupadas e (só no MSVC) os símbolos são
// resolvidos por DbgHelp. Fora do MSVC imprime só contagens por cenário e por etapa.
//
// O que isto É: um levantamento dos caminhos que o cenário exercitado EXECUTOU nesta passada. O
// que isto NÃO É: uma lista completa das alocações do código, nem evidência de ausência de
// alocação, nem um teste (não passa nem falha pelo número de alocações; só informa).
#pragma once

#include "oom_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if SW_SURVEY_SYMBOLS // a macro vem de oom_support.hpp
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on
#endif

namespace oom::survey {

inline constexpr std::size_t kTopStacks = 6; // N: pilhas completas impressas por cenário

struct Group
{
    const char* stage = "";
    unsigned short depth = 0;
    void* frames[kFrames] = {};
    std::size_t count = 0;
};

inline std::vector<Group> group_records()
{
    std::vector<Group> groups;
    for (std::size_t i = 0; i < recorded(); ++i) {
        const Record& r = g_records[i];
        bool found = false;
        for (Group& g : groups) {
            if (std::strcmp(g.stage, r.stage) == 0 && g.depth == r.depth &&
                std::memcmp(g.frames, r.frames, sizeof(void*) * r.depth) == 0) {
                ++g.count;
                found = true;
                break;
            }
        }
        if (!found) {
            Group g;
            g.stage = r.stage;
            g.depth = r.depth;
            std::memcpy(g.frames, r.frames, sizeof(void*) * r.depth);
            g.count = 1;
            groups.push_back(g);
        }
    }
    return groups;
}

inline void print_header(const char* scenario)
{
    std::cout << "[Levantamento] cenario='" << scenario << "': " << g_seen.load()
              << " alocacoes vistas, " << recorded() << " registradas, " << dropped()
              << " DESCARTADAS por capacidade (" << kCapacity << ")" << std::endl;
    std::cout << "  por etapa:";
    for (const StageCount& s : g_stages)
        if (s.stage != nullptr) std::cout << " [" << s.stage << "]=" << s.n;
    std::cout << std::endl;
}

#if SW_SURVEY_SYMBOLS
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

// Divide "a::b<c::d>::e" nos "::" de nível zero (fora de <> e ()).
inline std::vector<std::string> top_level_components(const std::string& name)
{
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<' || c == '(') ++depth;
        if (c == '>' || c == ')') --depth;
        if (depth == 0 && c == ':' && i + 1 < name.size() && name[i + 1] == ':') {
            parts.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);
    return parts;
}

inline std::string strip_templates(const std::string& s)
{
    const std::size_t lt = s.find('<');
    return lt == std::string::npos ? s : s.substr(0, lt);
}

// Construtor: "X<...>::X<...>" (o último componente repete o anterior).
inline bool is_constructor(const std::string& name)
{
    const auto parts = top_level_components(name);
    if (parts.size() < 2) return false;
    return strip_templates(parts.back()) == strip_templates(parts[parts.size() - 2]);
}

inline bool contains(const std::string& s, const char* what) { return s.find(what) != s.npos; }

inline bool is_noise(const Frame& f)
{
    return contains(f.file, "oom_support.hpp") || f.name.rfind("oom::", 0) == 0 ||
           f.name == "operator new" || f.name == "operator delete" ||
           f.name.rfind("operator new", 0) == 0;
}

// Frame do código do projeto (sw-core, sw-pack, sw-format, testes), não da STL/CRT.
inline bool is_project(const Frame& f)
{
    return !f.file.empty() && !contains(f.file, "Microsoft Visual Studio") &&
           !contains(f.file, "vctools") && !contains(f.file, "ucrt");
}

inline bool is_sw_format(const Frame& f)
{
    return contains(f.file, "sw-format") || contains(f.name, "swarmstream::pack::");
}

inline bool is_stl_noexcept_signal(const Frame& f)
{
    return contains(f.name, "_Alloc_proxy") || contains(f.name, "_Container_base12") ||
           (contains(f.name, "std::") && is_constructor(f.name));
}

inline std::string show(const Frame& f)
{
    std::string s = f.name;
    if (!f.file.empty()) s += " (" + f.file + ":" + std::to_string(f.line) + ")";
    return s;
}

inline void report(const char* scenario)
{
    print_header(scenario);
    const std::vector<Group> groups = group_records();
    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!::SymInitialize(process, nullptr, TRUE)) {
        std::cout << "  (SymInitialize falhou; sem pilhas)" << std::endl;
        return;
    }
    std::map<void*, Frame> cache;
    auto frame_of = [&](void* a) -> const Frame& {
        auto it = cache.find(a);
        if (it == cache.end()) it = cache.emplace(a, resolve(process, a)).first;
        return it->second;
    };

    struct Summary
    {
        std::size_t index;
        std::size_t count;
        bool stl_noexcept;
        bool sw_format;
        std::string stl_ctor; // 1o construtor da STL da pilha (arquivo:linha), p/ separar noexcept
        std::string caller;
        std::string caller_caller;
        std::vector<std::string> shown;
    };
    std::vector<Summary> sums;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& g = groups[gi];
        Summary s;
        s.index = gi;
        s.count = g.count;
        s.stl_noexcept = false;
        s.sw_format = false;
        std::vector<const Frame*> kept;
        for (unsigned short k = 0; k < g.depth; ++k) {
            const Frame& f = frame_of(g.frames[k]);
            if (is_noise(f)) continue;
            kept.push_back(&f);
            if (is_stl_noexcept_signal(f)) s.stl_noexcept = true;
            if (s.stl_ctor.empty() && contains(f.name, "std::") && is_constructor(f.name))
                s.stl_ctor = show(f);
            if (is_sw_format(f)) s.sw_format = true;
        }
        // Mostra do frame mais interno até o 1o frame do projeto que chama (e mais um).
        for (std::size_t k = 0; k < kept.size() && k < 36; ++k) {
            s.shown.push_back(show(*kept[k]));
            if (is_project(*kept[k]) && s.caller.empty()) {
                s.caller = show(*kept[k]);
                if (k + 1 < kept.size()) s.caller_caller = show(*kept[k + 1]);
                if (k + 1 < kept.size()) s.shown.push_back(show(*kept[k + 1]));
                break;
            }
        }
        sums.push_back(std::move(s));
    }

    std::cout << "  pilhas distintas: " << groups.size() << std::endl;
    // 1) Pilhas que passam por construtor/funcao noexcept da STL (proxy de iteradores) e o
    //    PRIMEIRO frame do codigo do projeto que as chama.
    std::map<std::string, std::pair<std::size_t, std::size_t>>
        by_caller; // caller -> (pilhas, alocs)
    for (const Summary& s : sums)
        if (s.stl_noexcept) {
            auto& e = by_caller[s.caller.empty() ? "(sem frame do projeto)" : s.caller];
            ++e.first;
            e.second += s.count;
        }
    std::cout
        << "  [STL-NOEXCEPT?] pilhas que passam por _Alloc_proxy/_Container_base12/construtor "
           "da STL: "
        << by_caller.size() << " chamadores distintos do codigo do projeto:" << std::endl;
    for (const auto& [caller, e] : by_caller)
        std::cout << "    " << e.second << " aloc em " << e.first << " pilha(s) <- " << caller
                  << std::endl;
    // 1b) T6: TODAS as pilhas sinalizadas (não só as top-N), com o construtor da STL
    // (arquivo:linha)
    //     e o 1o frame do projeto. O sinal é por nome de frame: serve para separar construtor
    //     noexcept de construtor comum, mas só a varredura (por K) mostra se há terminate.
    std::cout
        << "  [STL-NOEXCEPT?] detalhe de cada pilha sinalizada (construtor da STL <- projeto):"
        << std::endl;
    for (const Summary& s : sums)
        if (s.stl_noexcept)
            std::cout << "    x" << s.count << " etapa=[" << groups[s.index].stage << "] ctor="
                      << (s.stl_ctor.empty() ? "(sem construtor std:: na pilha)" : s.stl_ctor)
                      << " <- " << (s.caller.empty() ? "(sem frame do projeto)" : s.caller)
                      << std::endl;
    // 2) Pilhas que passam pelo sw-format.
    std::map<std::string, std::pair<std::size_t, std::size_t>> by_format;
    for (const Summary& s : sums)
        if (s.sw_format) {
            auto& e = by_format[s.caller.empty() ? "(sem frame do projeto)" : s.caller];
            ++e.first;
            e.second += s.count;
        }
    std::cout << "  [SW-FORMAT] pilhas que passam por funcoes do sw-format: " << by_format.size()
              << " chamadores distintos:" << std::endl;
    for (const auto& [caller, e] : by_format)
        std::cout << "    " << e.second << " aloc em " << e.first << " pilha(s) <- " << caller
                  << std::endl;
    // 3) Top-N pilhas por contagem, com a pilha ate o frame do projeto.
    std::vector<const Summary*> order;
    for (const Summary& s : sums)
        order.push_back(&s);
    std::stable_sort(order.begin(), order.end(),
                     [](const Summary* a, const Summary* b) { return a->count > b->count; });
    std::cout << "  top-" << kTopStacks << " pilhas por contagem (de " << sums.size()
              << "; pilha mostrada do frame mais interno ate o 1o frame do projeto + 1):"
              << std::endl;
    for (std::size_t i = 0; i < order.size() && i < kTopStacks; ++i) {
        const Summary& s = *order[i];
        std::cout << "   #" << (i + 1) << " x" << s.count << " etapa=[" << groups[s.index].stage
                  << "]" << (s.stl_noexcept ? " [STL-NOEXCEPT?]" : "")
                  << (s.sw_format ? " [SW-FORMAT]" : "") << std::endl;
        for (const std::string& line : s.shown)
            std::cout << "      " << line << std::endl;
    }
    ::SymCleanup(process); // o handler de std::terminate chama SymInitialize de novo
    std::cout << std::flush;
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
    [[nodiscard]] bool ok() const { return ok_; }

    // Do frame mais interno até o 1o frame do projeto (inclusive), sem o ruído do alocador, no
    // máximo 10 frames. Vazio se nada resolveu.
    [[nodiscard]] std::string describe(const std::vector<std::uint64_t>& rvas)
    {
        if (!ok_) return "(DbgHelp indisponivel)";
        std::string out;
        int shown = 0;
        for (const std::uint64_t rva : rvas) {
            if (rva >= size_) continue; // frame fora do executável (ntdll, ucrt, ...)
            const Frame f = resolve(process_, reinterpret_cast<void*>(base_ + rva));
            if (is_noise(f)) continue;
            if (!out.empty()) out += " <- ";
            out += show(f);
            if (is_project(f) || ++shown >= 10) break;
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
inline void report(const char* scenario)
{
    print_header(scenario);
    std::cout << "  (sem simbolos neste compilador: so contagens por cenario e etapa)" << std::endl;
}

class SymbolSession
{
public:
    [[nodiscard]] bool ok() const { return false; }
    [[nodiscard]] std::string describe(const std::vector<std::uint64_t>&) const
    {
        return "(sem simbolos neste compilador)";
    }
};
#endif

} // namespace oom::survey
