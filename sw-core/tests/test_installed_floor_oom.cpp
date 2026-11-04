// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69: varredura de falta de memória no piso anti-rollback (read_installed_floor e
// commit_installed_floor, ambos `noexcept` e ambos declarando FloorStatus::OutOfMemory).
//
// O operator new global DESTE executável (oom_support.hpp) falha com bad_alloc na K-ésima
// alocação. Para cada K = 1, 2, ... até a operação terminar sem a falha disparar, e para cada
// cenário, a varredura confere:
//   LEITURA  - o resultado NUNCA é Ok quando a falha disparou (em particular: uma falta de memória
//              jamais vira "arquivo ausente"/primeira instalação, que baixaria o piso); a pasta não
//              muda; nenhum handle fica aberto; a mesma leitura sem falha dá o resultado de base.
//   GRAVAÇÃO - o resultado nunca é Ok nem `wrote`; o arquivo do piso fica EXATAMENTE no estado
//              anterior, exceto se o resultado disser `may_have_changed` (e então fica antigo OU
//              novo, nunca outra coisa nem menor); se a troca aconteceu o resultado diz
//              `may_have_changed`; nenhum arquivo estranho aparece; nenhum handle fica aberto; e
//              uma nova gravação sem falha chega ao estado final esperado (o .tmp que sobrou é
//              truncado e consumido).
// Não há pontos excluídos: se uma alocação existe no cenário, ela é varrida. A instrumentação de
// diagnóstico (set_terminate etc.) não é evidência de ausência de alocação (ver oom_support.hpp).
//
// Com o libsodium STUB local a verificação de assinatura sempre aceita; isto não muda nenhuma
// alocação nem o que é conferido aqui.
#include "floor_test_support.hpp"
#include "oom_support.hpp"

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <swarmstream/manifest_view.hpp>
#include <vector>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace sp = swarmstream::pack;
namespace fs = std::filesystem;
using namespace swarmstream::test::floor;
using oom::arm;
using oom::disarm;
using oom::g_fired;
using oom::open_handles;

namespace {

const sp::Ed25519Seed kSeed = seed_of(0);
const std::string kGame = "jogo-oom";

std::span<const sp::Ed25519PublicKey> keys_of(const std::vector<sp::Ed25519PublicKey>& v)
{
    return std::span<const sp::Ed25519PublicKey>(v.data(), v.size());
}

using Setup = std::function<void(const fs::path&)>;

void install(const fs::path& dir, const Bytes& manifest, const std::vector<sp::Ed25519PublicKey>& k)
{
    const det::FloorWrite w = det::commit_installed_floor(dir, manifest, keys_of(k));
    SW_CHECK(w.status == det::FloorStatus::Ok && w.wrote);
}

// Estado do arquivo do piso e da pasta, para comparar antes/depois.
struct Disk
{
    bool target_exists = false;
    Bytes target;
    std::vector<std::pair<std::string, std::uintmax_t>> files;
};

Disk snapshot(const fs::path& dir)
{
    Disk d;
    const fs::path t = det::installed_floor_path(dir, kGame);
    d.target_exists = fs::exists(t);
    if (d.target_exists) d.target = read_all(t);
    d.files = listing(dir);
    return d;
}

bool same_read(const det::FloorRead& a, const det::FloorRead& b)
{
    return a.status == b.status && a.category == b.category && a.floor == b.floor &&
           a.existing_cache_without_floor == b.existing_cache_without_floor;
}

// ----------------------------------------------------------------------------------- leitura
void sweep_read(const char* name, const Setup& setup)
{
    std::cout << "[Test] varredura OOM, leitura do piso: " << name << std::endl;
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    const fs::path dir = fresh_dir("oomread");
    setup(dir);
    const det::FloorRead base = det::read_installed_floor(dir, kGame, keys_of(keys));
    const Disk before = snapshot(dir);
    oom::warm_up_exceptions();
    oom::g_progress_scenario.store(name);

    int failed_points = 0;
    int ok_at = 0;
    for (std::int64_t k = 1; k <= 2000; ++k) {
        const std::size_t handles_before = open_handles();
        oom::g_progress_k.store(k);
        arm(k);
        const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys));
        const bool fired = g_fired.load();
        disarm();
        if (!fired) {
            SW_CHECK(same_read(r, base));
            SW_CHECK(open_handles() == handles_before);
            ok_at = static_cast<int>(k);
            break;
        }
        ++failed_points;
        if (r.status == det::FloorStatus::Ok)
            std::cerr << "LEITURA devolveu Ok apos falha de alocacao em K=" << k << " (" << name
                      << ")" << std::endl;
        SW_CHECK(r.status != det::FloorStatus::Ok); // jamais "ausente" nem piso errado
        SW_CHECK(r.status == det::FloorStatus::OutOfMemory || r.status == det::FloorStatus::Io);
        if (open_handles() != handles_before)
            std::cerr << "VAZAMENTO (leitura) em K=" << k << " (" << name << ")" << std::endl;
        SW_CHECK(open_handles() == handles_before);
        const Disk after = snapshot(dir);
        SW_CHECK(after.files == before.files); // leitura não altera a pasta
        SW_CHECK(same_read(det::read_installed_floor(dir, kGame, keys_of(keys)), base));
    }
    SW_CHECK(ok_at > 0);
    SW_CHECK(failed_points > 0);
    std::cout << "  pontos de falha cobertos: " << failed_points << " (leitura completa na "
              << ok_at << "a alocacao)" << std::endl;
    cleanup(dir);
}

// ----------------------------------------------------------------------------------- gravação
struct CommitCase
{
    const char* name;
    Setup setup;
    Bytes new_manifest;
    std::uint64_t new_seq;
    // resultado esperado SEM falha
    det::FloorStatus expect_status;
    bool expect_wrote;
    // o arquivo final esperado SEM falha: o novo (se grava) ou o que já estava
    bool inject_replace_error_after_swap = false;
};

// Uma gravação (com as operações reais ou com a falha de replace DEPOIS da troca).
det::FloorWrite run_commit(const CommitCase& c, const fs::path& dir,
                           const std::vector<sp::Ed25519PublicKey>& keys, FaultOps& ops)
{
    return det::commit_installed_floor(dir, c.new_manifest, keys_of(keys), ops);
}

void sweep_commit(const CommitCase& c)
{
    std::cout << "[Test] varredura OOM, gravacao do piso: " << c.name << std::endl;
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    const fs::path dir = fresh_dir("oomwrite");
    const fs::path tmp = det::installed_tmp_path(dir, kGame);
    oom::warm_up_exceptions();
    oom::g_progress_scenario.store(c.name);

    int failed_points = 0;
    int ok_at = 0;
    for (std::int64_t k = 1; k <= 2000; ++k) {
        // Estado inicial limpo a cada ponto.
        cleanup(dir);
        fs::create_directories(dir);
        c.setup(dir);
        const Disk pre = snapshot(dir);
        std::optional<std::uint64_t> pre_floor;
        {
            const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys));
            SW_CHECK(r.status == det::FloorStatus::Ok);
            pre_floor = r.floor;
        }
        const std::size_t handles_before = open_handles();

        FaultOps ops;
        if (c.inject_replace_error_after_swap)
            ops.replace_fault = FaultOps::ReplaceFault::FailAfterSwap;
        oom::g_progress_k.store(k);
        arm(k);
        const det::FloorWrite w = run_commit(c, dir, keys, ops);
        const bool fired = g_fired.load();
        disarm();

        const Disk post = snapshot(dir);
        if (!fired) {
            SW_CHECK(w.status == c.expect_status);
            SW_CHECK(w.wrote == c.expect_wrote);
            SW_CHECK(open_handles() == handles_before);
            ok_at = static_cast<int>(k);
            break;
        }

        ++failed_points;
        if (w.status == det::FloorStatus::Ok || w.wrote)
            std::cerr << "GRAVACAO devolveu sucesso apos falha de alocacao em K=" << k << " ("
                      << c.name << ")" << std::endl;
        SW_CHECK(w.status != det::FloorStatus::Ok);
        SW_CHECK(!w.wrote);
        SW_CHECK(open_handles() == handles_before);

        // Se a troca aconteceu, o resultado TEM de dizer que o piso pode ter mudado.
        if (ops.swapped) SW_CHECK(w.may_have_changed);
        // O arquivo do piso: antigo OU novo (só se may_have_changed); nunca outra coisa, nunca
        // menor.
        const Bytes new_bytes = c.new_manifest;
        if (!w.may_have_changed) {
            SW_CHECK(post.target_exists == pre.target_exists);
            if (pre.target_exists) SW_CHECK(same_bytes(post.target, pre.target));
        } else {
            SW_CHECK(post.target_exists);
            SW_CHECK((pre.target_exists && same_bytes(post.target, pre.target)) ||
                     same_bytes(post.target, new_bytes));
        }
        // Piso nunca menor (leitura real, sem falha).
        {
            const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys));
            if (pre_floor.has_value()) {
                SW_CHECK(r.status == det::FloorStatus::Ok && r.floor.has_value());
                SW_CHECK(*r.floor >= *pre_floor);
            }
        }
        // Nenhum arquivo estranho: só o que já havia, a pasta installed/ e o .tmp do piso.
        for (const auto& f : post.files) {
            bool known = f.first == "installed" || f.first.ends_with(".tmp");
            for (const auto& p : pre.files)
                if (p.first == f.first) known = true;
            if (!known) std::cerr << "arquivo estranho: " << f.first << " em K=" << k << std::endl;
            SW_CHECK(known);
        }

        // Uma NOVA gravação sem falha chega ao estado final esperado.
        FaultOps again_ops;
        if (c.inject_replace_error_after_swap)
            again_ops.replace_fault = FaultOps::ReplaceFault::FailAfterSwap;
        const det::FloorWrite again = run_commit(c, dir, keys, again_ops);
        // Com o erro de replace injetado, o resultado depende de a troca já ter ocorrido (cópia
        // idêntica => Ok); o que vale é o estado final.
        if (!c.inject_replace_error_after_swap) {
            SW_CHECK(again.status == c.expect_status);
            SW_CHECK(again.wrote == c.expect_wrote);
        }
        const Disk fin = snapshot(dir);
        if (c.expect_wrote || c.inject_replace_error_after_swap) {
            SW_CHECK(fin.target_exists && same_bytes(fin.target, new_bytes));
            SW_CHECK(!fs::exists(tmp)); // o .tmp que sobrou foi consumido
        } else {
            SW_CHECK(fin.target_exists == pre.target_exists);
            if (pre.target_exists) SW_CHECK(same_bytes(fin.target, pre.target));
        }
    }
    SW_CHECK(ok_at > 0);
    SW_CHECK(failed_points > 0);
    std::cout << "  pontos de falha cobertos: " << failed_points << " (gravacao completa na "
              << ok_at << "a alocacao)" << std::endl;
    cleanup(dir);
}

} // namespace

int main()
{
    oom::make_abort_fail_instead_of_hang();
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    const Bytes m5 = make_manifest(kGame, 5, kSeed);
    const Bytes m7 = make_manifest(kGame, 7, kSeed);
    const Bytes m9 = make_manifest(kGame, 9, kSeed);
    const Bytes m5_reissue = make_manifest(kGame, 5, kSeed, "reassinado");
    SW_CHECK(!same_bytes(m5, m5_reissue));

    // ---- leitura
    sweep_read("piso presente", [&](const fs::path& d) { install(d, m7, keys); });
    sweep_read("primeira instalacao (sem installed/)", [&](const fs::path&) {});
    sweep_read("cache existente sem piso (index.bin presente)",
               [&](const fs::path& d) { write_all(d / "index.bin", Bytes(4, std::byte{1})); });
    sweep_read("installed e um arquivo comum",
               [&](const fs::path& d) { write_all(d / "installed", Bytes(3, std::byte{2})); });

    // ---- gravação
    sweep_commit(
        {"primeira instalacao", [&](const fs::path&) {}, m5, 5, det::FloorStatus::Ok, true});
    sweep_commit({"atualizacao 5 -> 7", [&](const fs::path& d) { install(d, m5, keys); }, m7, 7,
                  det::FloorStatus::Ok, true});
    sweep_commit({"mesmo build_seq com bytes diferentes (reassinatura)",
                  [&](const fs::path& d) { install(d, m5, keys); }, m5_reissue, 5,
                  det::FloorStatus::Ok, true});
    sweep_commit({"copia identica (nada a gravar)",
                  [&](const fs::path& d) { install(d, m5, keys); }, m5, 5, det::FloorStatus::Ok,
                  false});
    sweep_commit({"downgrade recusado (9 -> 7)", [&](const fs::path& d) { install(d, m9, keys); },
                  m7, 7, det::FloorStatus::Rollback, false});
    CommitCase after_swap{"erro de replace DEPOIS da troca (5 -> 7)",
                          [&](const fs::path& d) { install(d, m5, keys); },
                          m7,
                          7,
                          det::FloorStatus::Io,
                          false};
    after_swap.inject_replace_error_after_swap = true;
    sweep_commit(after_swap);

    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA DO PISO PASSARAM" << std::endl;
    return 0;
}
