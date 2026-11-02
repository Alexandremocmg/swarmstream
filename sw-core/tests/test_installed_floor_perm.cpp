// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69, caso (d) do F1-60 §6.2: Linux, helper REAL (replace_file de platform_file.hpp), com
// installed/ em modo 0300 (escrita e travessia, sem leitura). Assim o rename(2) dá certo e o
// open(O_RDONLY | O_DIRECTORY) do sync_directory falha com EACCES: o erro vem DEPOIS da troca.
//
// O teste prova as DUAS coisas: (1) a troca ocorreu (o alvo mudou de conteúdo e o .tmp sumiu) e
// (2) o erro veio depois dela (replace_file devolveu IoError mesmo assim).
//
// Limitação declarada: o caso só vale quando a permissão vale. Como root (ou com ACL/FS que a
// ignore) o open da pasta funcionaria e o teste não reproduz a falha. Nesse caso ele imprime
// "NAO COBERTO" com o motivo e sai com 77 (SKIP_RETURN_CODE do ctest: aparece como PULADO, não
// como aprovado). No Windows não há equivalente: MoveFileExW é uma chamada só; a falha posterior
// é coberta pela operação injetada (test_installed_floor.cpp (c) e (e)), não pela API real.
#include "floor_test_support.hpp"

#include <cerrno>
#include <iostream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace sp = swarmstream::pack;
namespace fs = std::filesystem;
using namespace swarmstream::test::floor;

namespace {

constexpr int kSkipExit = 77;

int not_covered(const char* why)
{
    std::cout << "NAO COBERTO (d): " << why << "\n";
    return kSkipExit;
}

} // namespace

int main()
{
#if defined(_WIN32)
    return not_covered("Windows: MoveFileExW e uma chamada so; nao ha como reproduzir a falha "
                       "posterior com a API real (a operacao injetada cobre (c) e (e))");
#else
    SW_CHECK(sp::crypto_ready());
    const sp::Ed25519Seed seed = seed_of(0);
    const std::vector<sp::Ed25519PublicKey> keys = {public_key_of(seed)};
    const std::span<const sp::Ed25519PublicKey> kspan(keys.data(), keys.size());
    const Bytes old_m = make_manifest("jogo", 1, seed);
    const Bytes new_m = make_manifest("jogo", 2, seed);

    const fs::path dir = fresh_dir("perm");
    const fs::path installed = dir / det::kInstalledDirName;
    const fs::path target = det::installed_floor_path(dir, "jogo");
    const fs::path tmp = det::installed_tmp_path(dir, "jogo");

    // Piso antigo gravado com as operações reais.
    const det::FloorWrite first = det::commit_installed_floor(dir, old_m, kspan);
    SW_CHECK(first.status == det::FloorStatus::Ok && first.wrote);
    SW_CHECK(same_bytes(read_all(target), old_m));

    // installed/ em 0300 e a sonda: o open da pasta TEM de falhar para o caso valer.
    SW_CHECK(::chmod(installed.c_str(), 0300) == 0);
    const int probe = ::open(installed.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (probe >= 0) {
        (void)::close(probe);
        SW_CHECK(::chmod(installed.c_str(), 0700) == 0);
        cleanup(dir);
        return not_covered(::geteuid() == 0
                               ? "rodando como root: a permissao 0300 nao impede abrir a pasta"
                               : "a permissao 0300 nao impediu abrir a pasta (ACL ou sistema de arquivos)");
    }
    SW_CHECK(errno == EACCES);

    // Helper REAL de ponta a ponta.
    const det::FloorWrite w = det::commit_installed_floor(dir, new_m, kspan);

    // Devolve o acesso para poder examinar o que ficou.
    SW_CHECK(::chmod(installed.c_str(), 0700) == 0);

    // (1) A TROCA OCORREU: o conteúdo do alvo mudou e o .tmp virou o alvo.
    SW_CHECK(!same_bytes(read_all(target), old_m));
    SW_CHECK(same_bytes(read_all(target), new_m));
    SW_CHECK(!fs::exists(tmp));
    // (2) E o erro veio DEPOIS dela: o commit falhou em W5 mesmo com o alvo já novo.
    SW_CHECK(w.status == det::FloorStatus::Io);
    SW_CHECK(w.category == det::FloorCategory::Replace);
    SW_CHECK(!w.wrote && w.may_have_changed);
    // A reabertura lê o piso NOVO.
    const det::FloorRead r = det::read_installed_floor(dir, "jogo", kspan);
    SW_CHECK(r.status == det::FloorStatus::Ok && r.floor.has_value() && *r.floor == 2);

    cleanup(dir);
    std::cout << "[Test] (d) coberto: rename deu certo e o fsync da pasta falhou (EACCES) depois dele\n";
    return 0;
#endif
}
