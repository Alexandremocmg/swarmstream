// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-74: falha da sincronização da pasta DEPOIS do rename em `replace_file` (POSIX).
//
// A pasta do destino fica em 0300 (-wx): o rename funciona (precisa de escrita e busca), mas o
// open(O_RDONLY | O_DIRECTORY) do fsync da pasta é negado. O helper devolve IoError com a troca
// JÁ FEITA: o chamador deve tratá-lo como "antigo OU novo" (é o que o piso faz com
// `may_have_changed`). Este teste fixa exatamente isso: erro E destino novo. O helper não tem como
// dizer ao chamador que a troca ocorreu (a assinatura não muda; decisão do F1-74): a semântica
// está no comentário de `replace_file`.
//
// Sai com 77 (o ctest mostra PULADO, SKIP_RETURN_CODE 77) quando a permissão não vale: Windows
// (o helper não sincroniza pasta), root ou capacidade que ignora permissões de pasta.
#include "check.hpp"
#include "platform_file.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;
using sw::CacheErrorCode;

#if defined(_WIN32)

int main()
{
    std::cout << "[PULADO] sync_directory depois do rename so existe no POSIX: o Windows usa "
                 "MoveFileExW com WRITE_THROUGH (nao ha fsync de pasta)"
              << std::endl;
    return 77;
}

#else

#include <sys/stat.h>
#include <unistd.h>

namespace {

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

// ERRADA: faz o rename e ignora o erro da sincronização da pasta (falha silenciosa).
CacheErrorCode mutant_ignores_sync_failure(const fs::path& from, const fs::path& to)
{
    if (::rename(from.c_str(), to.c_str()) != 0) return CacheErrorCode::IoError;
    (void)det::sync_directory(to.parent_path());
    return CacheErrorCode::Success;
}

} // namespace

int main()
{
    if (::geteuid() == 0) {
        std::cout << "[PULADO] executando como root: a permissao 0300 da pasta nao vale"
                  << std::endl;
        return 77;
    }
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_74_perm_") +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    const fs::path from = dir / "indice-novo.bin.tmp";
    const fs::path to = dir / "indice-do-cache.bin";

    // Sem leitura na pasta: o open do fsync é negado, o rename (escrita + busca) continua valendo.
    SW_CHECK(::chmod(dir.c_str(), 0300) == 0);
    if (::access(dir.c_str(), R_OK) == 0) {
        SW_CHECK(::chmod(dir.c_str(), 0700) == 0);
        fs::remove_all(dir, ec);
        std::cout << "[PULADO] a pasta em 0300 continua legivel (capacidade que ignora permissoes)"
                  << std::endl;
        return 77;
    }
    SW_CHECK(::chmod(dir.c_str(), 0700) == 0);

    // Controle: o próprio sync_directory falha na pasta em 0300 (a falha injetada é real).
    SW_CHECK(det::sync_directory(dir) == CacheErrorCode::Success);
    SW_CHECK(::chmod(dir.c_str(), 0300) == 0);
    SW_CHECK(det::sync_directory(dir) == CacheErrorCode::IoError);
    SW_CHECK(::chmod(dir.c_str(), 0700) == 0);

    struct Run
    {
        const char* name;
        CacheErrorCode (*fn)(const fs::path&, const fs::path&);
    };
    const Run runs[] = {
        {"replace_file",
         [](const fs::path& a, const fs::path& b) { return det::replace_file(a, b); }},
        {"mutante: ignora a falha do sync", &mutant_ignores_sync_failure},
    };
    bool real_ok = false;
    bool mutant_caught = false;
    for (const Run& run : runs) {
        std::cout << "[Test] sync da pasta falha DEPOIS do rename: " << run.name << std::endl;
        spit(from, "conteudo NOVO da origem");
        spit(to, "conteudo ANTIGO do destino");
        SW_CHECK(::chmod(dir.c_str(), 0300) == 0);
        const CacheErrorCode rc = run.fn(from, to);
        SW_CHECK(::chmod(dir.c_str(), 0700) == 0);
        const std::optional<std::string> to_now = slurp(to);
        const bool swapped = to_now == std::optional<std::string>("conteudo NOVO da origem") &&
                             !slurp(from).has_value();
        // O que o helper promete: erro (IoError) E a troca JÁ FEITA (antigo OU novo; aqui, novo).
        const bool ok = rc == CacheErrorCode::IoError && swapped;
        if (run.fn == &mutant_ignores_sync_failure)
            mutant_caught = !ok; // o auditor tem de reprovar quem devolve Success
        else
            real_ok = ok;
        if (!ok)
            std::cerr << "  " << run.name << ": codigo=" << sw::to_string(rc)
                      << " trocado=" << swapped << std::endl;
    }
    SW_CHECK(real_ok);
    SW_CHECK(mutant_caught);

    fs::remove_all(dir, ec);
    std::cout << "TESTE DA FALHA DE SYNC DEPOIS DO RENAME (F1-74) PASSOU" << std::endl;
    return 0;
}

#endif
