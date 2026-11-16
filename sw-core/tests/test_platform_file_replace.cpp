// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-74: `replace_file` (platform_file.hpp) sem terminar o processo por falta de memória.
//
// O que este executável prova:
//  1. CacheErrorCode::OutOfMemory e Internal existem, com to_string, e os valores antigos não
//  mudaram.
//  2. Varredura de operator new SEM pontos excluídos: a K-ésima alocação de `replace_file` falha
//     (K = 1, 2, ... até a chamada passar sem a falha disparar). Em todo ponto que dispara: nenhuma
//     exceção sai, o código é OutOfMemory (não IoError), o destino está INTACTO e a origem foi
//     preservada (a falha veio ANTES da troca). Sem falha disparada: Success, destino novo, origem
//     consumida. Uma alocação DEPOIS da troca apareceria aqui como terminate (a função é noexcept)
//     ou como destino novo com código de erro.
//  3. Falha do rename (origem ausente; pasta de destino ausente): IoError, destino intacto.
//  4. Controles positivos: implementações ERRADAS (mutantes) escritas neste arquivo têm de ser
//     reprovadas pelo MESMO auditor; e uma implementação CERTA com a forma da correção tem de ser
//     aprovada e ter pontos de falha de verdade (prova que a injeção dispara e que o auditor sabe
//     ver a falha). Os mutantes lançam em vez de terminar para o auditor poder observá-los no mesmo
//     processo; a alocação do "caminho da pasta" é simulada por um operator new explícito, de modo
//     que o controle não depende de a STL alocar ou não neste compilador (MSVC Debug incluso).
//
// No Windows o helper é só MoveFileExW (nada aloca): a varredura termina no K = 1 sem falha
// disparada, e isso é o resultado esperado. No POSIX o helper calcula o caminho da pasta (aloca)
// ANTES do rename; a varredura dispara nesse ponto. A falha da sincronização DEPOIS do rename
// (Linux, não-root) está em test_platform_file_replace_perm.cpp.
#include "check.hpp"
#include "oom_support.hpp"
#include "platform_file.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;
using oom::arm;
using oom::disarm;
using oom::g_fired;
using oom::g_progress_k;
using oom::g_progress_scenario;
using oom::open_handles;
using sw::CacheErrorCode;

// A assinatura do helper é a do F1-11 (o F1-74 não a muda): noexcept, devolve CacheErrorCode.
static_assert(std::is_same_v<decltype(det::replace_file(std::declval<const fs::path&>(),
                                                        std::declval<const fs::path&>())),
                             CacheErrorCode>);
static_assert(noexcept(det::replace_file(std::declval<const fs::path&>(),
                                         std::declval<const fs::path&>())));

namespace {

// ---------------------------------------------------------------------------- 1. enum
void test_error_code_enum()
{
    std::cout << "[Test] CacheErrorCode: valores antigos inalterados, OutOfMemory e Internal novos"
              << std::endl;
    // Os valores existentes não mudam (o enum é interno, mas o journal e os testes dependem da
    // ordem).
    static_assert(static_cast<int>(CacheErrorCode::Success) == 0);
    static_assert(static_cast<int>(CacheErrorCode::OutOfSlots) == 1);
    static_assert(static_cast<int>(CacheErrorCode::SlotNotFound) == 2);
    static_assert(static_cast<int>(CacheErrorCode::SlotBusy) == 3);
    static_assert(static_cast<int>(CacheErrorCode::InvalidOffset) == 4);
    static_assert(static_cast<int>(CacheErrorCode::InvalidSize) == 5);
    static_assert(static_cast<int>(CacheErrorCode::PieceNotFound) == 6);
    static_assert(static_cast<int>(CacheErrorCode::CorruptedChecksum) == 7);
    static_assert(static_cast<int>(CacheErrorCode::IoError) == 8);
    static_assert(static_cast<int>(CacheErrorCode::DiskFull) == 9);
    static_assert(static_cast<int>(CacheErrorCode::Stopped) == 10);
    // Os novos entram DEPOIS do último valor antigo.
    static_assert(static_cast<int>(CacheErrorCode::OutOfMemory) == 11);
    static_assert(static_cast<int>(CacheErrorCode::Internal) == 12);

    SW_CHECK(sw::to_string(CacheErrorCode::Success) == "Success");
    SW_CHECK(sw::to_string(CacheErrorCode::IoError) == "IoError");
    SW_CHECK(sw::to_string(CacheErrorCode::DiskFull) == "DiskFull");
    SW_CHECK(sw::to_string(CacheErrorCode::Stopped) == "Stopped");
    SW_CHECK(sw::to_string(CacheErrorCode::OutOfMemory) == "OutOfMemory");
    SW_CHECK(sw::to_string(CacheErrorCode::Internal) == "Internal");
    // Um valor fora do enum continua caindo no texto de desconhecido (o switch não tem default).
    SW_CHECK(sw::to_string(static_cast<CacheErrorCode>(99)) == "UnknownCacheError");
    // Os dois novos são distintos entre si e dos antigos.
    SW_CHECK(CacheErrorCode::OutOfMemory != CacheErrorCode::IoError);
    SW_CHECK(CacheErrorCode::Internal != CacheErrorCode::IoError);
    SW_CHECK(CacheErrorCode::OutOfMemory != CacheErrorCode::Internal);
}

// ---------------------------------------------------------------------------- infraestrutura
const char* const kOld = "conteudo ANTIGO do destino";
const char* const kNew = "conteudo NOVO da origem";

std::optional<std::string> slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& s)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    SW_CHECK(out.good());
    out << s;
    out.flush();
    SW_CHECK(out.good());
}

enum class Dest { Existing, Absent };

const char* name_of(Dest d) { return d == Dest::Existing ? "destino existe" : "destino ausente"; }

struct Fixture
{
    fs::path dir;
    fs::path from;
    fs::path to;
    std::optional<std::string> to_before; // conteúdo do destino antes da chamada
};

// Pasta nova com nome longo (o caminho da pasta passa do SSO: calculá-lo ALOCA de verdade).
fs::path fresh_dir(const char* tag)
{
    return fs::temp_directory_path() /
           (std::string("swarmstream_f1_74_replace_") + tag + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

Fixture make_fixture(const fs::path& dir, Dest dest)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    Fixture fx;
    fx.dir = dir;
    fx.from = dir / "indice-novo.bin.tmp";
    fx.to = dir / "indice-do-cache.bin";
    spit(fx.from, kNew);
    if (dest == Dest::Existing) spit(fx.to, kOld);
    fx.to_before = slurp(fx.to);
    SW_CHECK(fx.to_before.has_value() == (dest == Dest::Existing));
    return fx;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

using ReplaceFn = CacheErrorCode (*)(const fs::path&, const fs::path&);

// A troca em si, sem alocar e sem std::filesystem (usada pelas implementações de controle).
bool raw_swap(const fs::path& from, const fs::path& to) noexcept
{
#if defined(_WIN32)
    return ::MoveFileExW(from.c_str(), to.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// ---------------------------------------------------------------------------- o helper e controles
CacheErrorCode real_replace(const fs::path& from, const fs::path& to)
{
    return det::replace_file(from, to);
}

// A alocação do "caminho da pasta" é simulada por um operator new explícito (ver o cabeçalho).
struct SimulatedAlloc
{
    void* p;
    SimulatedAlloc() : p(::operator new(48)) {} // lança bad_alloc na K-ésima alocação
    ~SimulatedAlloc() { ::operator delete(p); }
    SimulatedAlloc(const SimulatedAlloc&) = delete;
    SimulatedAlloc& operator=(const SimulatedAlloc&) = delete;
};

// CERTA: aloca ANTES do rename; falta de memória => OutOfMemory sem tocar o disco; nada aloca
// depois.
CacheErrorCode good_alloc_before_swap(const fs::path& from, const fs::path& to)
{
    try {
        const SimulatedAlloc dir;
        if (!raw_swap(from, to)) return CacheErrorCode::IoError;
        return CacheErrorCode::Success;
    } catch (const std::bad_alloc&) {
        return CacheErrorCode::OutOfMemory;
    }
}

// ERRADA (o defeito original): troca primeiro e DEPOIS aloca. No helper noexcept isto é
// std::terminate; aqui a exceção sai para o auditor poder vê-la.
CacheErrorCode mutant_alloc_after_swap(const fs::path& from, const fs::path& to)
{
    if (!raw_swap(from, to)) return CacheErrorCode::IoError;
    const SimulatedAlloc dir; // pode lançar DEPOIS da troca
    return CacheErrorCode::Success;
}

// ERRADA: aloca antes da troca, mas devolve IoError (o chamador não distingue memória de disco).
CacheErrorCode mutant_oom_reported_as_io(const fs::path& from, const fs::path& to)
{
    try {
        const SimulatedAlloc dir;
        if (!raw_swap(from, to)) return CacheErrorCode::IoError;
        return CacheErrorCode::Success;
    } catch (const std::bad_alloc&) {
        return CacheErrorCode::IoError;
    }
}

// ERRADA: troca primeiro e só depois captura a falta de memória e diz OutOfMemory: o código afirma
// "nada foi trocado" com a troca já feita.
CacheErrorCode mutant_oom_after_swap_claims_untouched(const fs::path& from, const fs::path& to)
{
    if (!raw_swap(from, to)) return CacheErrorCode::IoError;
    try {
        const SimulatedAlloc dir;
        return CacheErrorCode::Success;
    } catch (const std::bad_alloc&) {
        return CacheErrorCode::OutOfMemory;
    }
}

// ERRADA: ignora o resultado da troca (falha silenciosa).
CacheErrorCode mutant_ignores_swap_failure(const fs::path& from, const fs::path& to)
{
    (void)raw_swap(from, to);
    return CacheErrorCode::Success;
}

// ---------------------------------------------------------------------------- auditor
// Vazio quando o resultado é coerente com o ponto da falha; senão, o motivo.
std::string audit(const Fixture& fx, bool fired, bool threw, CacheErrorCode rc)
{
    if (threw) return "excecao saiu da funcao (std::terminate no helper noexcept)";
    const std::optional<std::string> to_now = slurp(fx.to);
    const std::optional<std::string> from_now = slurp(fx.from);
    if (fired) {
        // A falha de memória veio ANTES da troca: nada mudou e o chamador sabe que é memória.
        if (rc != CacheErrorCode::OutOfMemory)
            return std::string("falta de memoria devolveu '") + std::string(sw::to_string(rc)) +
                   "' em vez de OutOfMemory";
        if (to_now != fx.to_before)
            return "OutOfMemory com o destino ALTERADO (a troca ja tinha ocorrido)";
        if (!from_now.has_value() || *from_now != kNew)
            return "OutOfMemory sem a origem preservada";
        return {};
    }
    if (rc != CacheErrorCode::Success)
        return std::string("sem falha injetada devolveu '") + std::string(sw::to_string(rc)) + "'";
    if (!to_now.has_value() || *to_now != kNew) return "Success sem o destino novo";
    if (from_now.has_value()) return "Success com a origem ainda no lugar";
    return {};
}

struct SweepResult
{
    int fired_points = 0;
    int clean_at = 0; // K da primeira chamada que passou sem a falha disparar (0 = não terminou)
    std::vector<std::string> violations;
};

constexpr std::int64_t kMaxK = 64;

SweepResult sweep(const char* label, ReplaceFn fn, Dest dest)
{
    const fs::path dir = fresh_dir("sweep");
    SweepResult res;
    oom::warm_up_exceptions();
    for (std::int64_t k = 1; k <= kMaxK; ++k) {
        const Fixture fx = make_fixture(dir, dest);
        const std::size_t handles_before = open_handles();
        g_progress_k.store(k);
        g_progress_scenario.store(label);
        CacheErrorCode rc = CacheErrorCode::Success;
        bool threw = false;
        arm(k);
        try {
            rc = fn(fx.from, fx.to);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool fired = g_fired.load();
        disarm();

        std::string why = audit(fx, fired, threw, rc);
        if (open_handles() != handles_before) why += " [vazou handle/descritor]";
        if (fired) ++res.fired_points;
        if (!why.empty()) res.violations.push_back("K=" + std::to_string(k) + ": " + why);
        if (!fired) {
            res.clean_at = static_cast<int>(k);
            break;
        }
    }
    cleanup(dir);
    return res;
}

void expect_clean(const char* label, ReplaceFn fn, Dest dest, bool must_have_failure_points)
{
    std::cout << "[Test] varredura de falta de memoria: " << label << " (" << name_of(dest) << ")"
              << std::endl;
    const SweepResult r = sweep(label, fn, dest);
    for (const std::string& v : r.violations)
        std::cerr << "  VIOLACAO " << label << " " << v << std::endl;
    SW_CHECK(r.violations.empty());
    SW_CHECK(r.clean_at > 0); // a varredura terminou: a chamada completa passou sem falha injetada
    if (must_have_failure_points) SW_CHECK(r.fired_points > 0);
    std::cout << "  pontos de falha cobertos: " << r.fired_points << " (chamada completa na "
              << r.clean_at << "a alocacao; sem pontos excluidos)" << std::endl;
}

// Controle positivo: o auditor TEM de reprovar a implementação errada (e dizer por quê).
void expect_caught(const char* label, ReplaceFn fn, Dest dest, const char* reason_fragment)
{
    std::cout << "[Test] controle (mutante tem de ser reprovado): " << label << " ("
              << name_of(dest) << ")" << std::endl;
    const SweepResult r = sweep(label, fn, dest);
    SW_CHECK(!r.violations.empty());
    bool found = false;
    for (const std::string& v : r.violations)
        if (v.find(reason_fragment) != std::string::npos) found = true;
    if (!found) {
        for (const std::string& v : r.violations)
            std::cerr << "  motivo visto: " << v << std::endl;
        std::cerr << "  motivo esperado: " << reason_fragment << std::endl;
    }
    SW_CHECK(found);
}

// ---------------------------------------------------------------------------- falha do rename
// Sem injeção de memória: o rename falha (origem ausente; pasta de destino ausente).
std::string audit_rename_failure(ReplaceFn fn, bool missing_source)
{
    const fs::path dir = fresh_dir(missing_source ? "nosrc" : "nodir");
    Fixture fx = make_fixture(dir, Dest::Existing);
    fs::path from = fx.from;
    fs::path to = fx.to;
    if (missing_source) {
        std::error_code ec;
        fs::remove(from, ec);
        SW_CHECK(!ec);
    } else {
        to = dir / "pasta-que-nao-existe" / "destino.bin";
    }
    const std::size_t handles_before = open_handles();
    CacheErrorCode rc = CacheErrorCode::Success;
    bool threw = false;
    try {
        rc = fn(from, to);
    } catch (...) {
        threw = true;
    }
    std::string why;
    if (threw)
        why = "excecao inesperada";
    else if (rc != CacheErrorCode::IoError)
        why = std::string("rename que falhou devolveu '") + std::string(sw::to_string(rc)) +
              "' em vez de IoError";
    else if (slurp(fx.to) != fx.to_before)
        why = "destino alterado por um rename que falhou";
    else if (!missing_source && slurp(from) != std::optional<std::string>(kNew))
        why = "origem nao preservada";
    if (open_handles() != handles_before) why += " [vazou handle/descritor]";
    cleanup(dir);
    return why;
}

void test_rename_failure()
{
    std::cout << "[Test] rename que falha: IoError, destino intacto, origem preservada"
              << std::endl;
    const std::string a = audit_rename_failure(&real_replace, /*missing_source=*/true);
    const std::string b = audit_rename_failure(&real_replace, /*missing_source=*/false);
    if (!a.empty()) std::cerr << "  origem ausente: " << a << std::endl;
    if (!b.empty()) std::cerr << "  pasta de destino ausente: " << b << std::endl;
    SW_CHECK(a.empty());
    SW_CHECK(b.empty());
    // Controles: quem ignora o erro da troca é reprovado nos dois casos.
    std::cout << "[Test] controle (mutante tem de ser reprovado): ignora a falha do rename"
              << std::endl;
    SW_CHECK(!audit_rename_failure(&mutant_ignores_swap_failure, true).empty());
    SW_CHECK(!audit_rename_failure(&mutant_ignores_swap_failure, false).empty());
    // E a implementação certa passa pelo mesmo auditor (o auditor não reprova tudo).
    SW_CHECK(audit_rename_failure(&good_alloc_before_swap, true).empty());
}

// ---------------------------------------------------------------------------- caminho feliz
void test_success_path()
{
    std::cout << "[Test] troca bem-sucedida (destino existente e ausente), sem vazar descritor"
              << std::endl;
    const fs::path dir = fresh_dir("ok");
    for (Dest d : {Dest::Existing, Dest::Absent}) {
        const Fixture fx = make_fixture(dir, d);
        const std::size_t handles_before = open_handles();
        const CacheErrorCode rc = det::replace_file(fx.from, fx.to);
        SW_CHECK(rc == CacheErrorCode::Success);
        SW_CHECK(slurp(fx.to) == std::optional<std::string>(kNew));
        SW_CHECK(!slurp(fx.from).has_value());
        SW_CHECK(open_handles() == handles_before);
    }
    cleanup(dir);
}

} // namespace

int main()
{
    oom::make_abort_fail_instead_of_hang();
    test_error_code_enum();
    test_success_path();
    test_rename_failure();

    // Controle positivo da própria varredura: a forma CERTA passa e tem pontos de falha de verdade.
    for (Dest d : {Dest::Existing, Dest::Absent})
        expect_clean("implementacao certa de controle", &good_alloc_before_swap, d, true);

    // O helper de verdade. No POSIX o caminho da pasta é alocado (antes do rename): há pontos de
    // falha. No Windows o helper não aloca: a varredura termina na primeira chamada.
    for (Dest d : {Dest::Existing, Dest::Absent}) {
#if defined(_WIN32)
        expect_clean("replace_file", &real_replace, d, /*must_have_failure_points=*/false);
#else
        expect_clean("replace_file", &real_replace, d, /*must_have_failure_points=*/true);
#endif
    }

    // Mutantes: cada um viola uma regra diferente e o auditor tem de dizer qual.
    for (Dest d : {Dest::Existing, Dest::Absent}) {
        expect_caught("mutante: aloca depois do rename", &mutant_alloc_after_swap, d,
                      "excecao saiu da funcao");
        expect_caught("mutante: memoria vira IoError", &mutant_oom_reported_as_io, d,
                      "em vez de OutOfMemory");
        expect_caught("mutante: OutOfMemory com a troca feita",
                      &mutant_oom_after_swap_claims_untouched, d, "ALTERADO");
    }
    std::cout << "TODOS OS TESTES DO replace_file (F1-74) PASSARAM" << std::endl;
    return 0;
}
