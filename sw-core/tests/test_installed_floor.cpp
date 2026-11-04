// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69: piso anti-rollback por `game_id` (F1-60 §6.2, docs/03 §8 item 10).
//
// Uso: sw-core-floor-tests <caminho do posse-tool>. O posse-tool roda como PROCESSO FILHO que se
// mata à força num ponto exato do commit (caso (e)).
//
// Mapa para os testes (a)-(h) do F1-60 §6.2:
//   (a) interface de operações de plataforma: FaultOps (floor_test_support.hpp), padrão = reais;
//   (b) falha ANTES da troca: test_replace_fails_before_swap;
//   (c) falha DEPOIS da troca: test_replace_fails_after_swap (a operação injetada faz a troca de
//       verdade, o teste prova que ela ocorreu e que o erro veio depois);
//   (d) Linux, helper real, installed/ em 0300: test_installed_floor_perm.cpp (executável à parte,
//       porque precisa saber se a permissão vale: o root a ignora);
//   (e) queda depois da troca / antes dela: test_crash_points;
//   (f) injeção em W1-W4 com a tabela como oráculo: test_failures_before_swap_leave_floor;
//       .tmp órfão: test_orphan_tmp;
//   (g) estado corrompido/vazio/truncado/chave desconhecida/outro game_id, rotação de chave, dois
//       jogos, igual com bytes diferentes, menor: test_policy_*, test_two_games, test_write_rule;
//   (h) Windows: não há teste da API real no ponto da falha posterior; (c) e (e) usam a operação
//       injetada (declarado no PR).
// Os casos que dependem de o libsodium REJEITAR uma assinatura ruim só rodam com o libsodium
// real; com o stub de revisão local são PULADOS com aviso (ver is_sodium_stub).
#include "floor_test_support.hpp"
#include "test_process.hpp"

#include <iostream>
#include <string>
#include <swarmstream/manifest_view.hpp>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace sp = swarmstream::pack;
namespace fs = std::filesystem;
using namespace swarmstream::test::floor;

namespace {

fs::path g_tool;

const sp::Ed25519Seed kSeedA = seed_of(0);
const sp::Ed25519Seed kSeedB = seed_of(100);

std::span<const sp::Ed25519PublicKey> keys_of(const std::vector<sp::Ed25519PublicKey>& v)
{
    return std::span<const sp::Ed25519PublicKey>(v.data(), v.size());
}

std::vector<sp::Ed25519PublicKey> only_a() { return {public_key_of(kSeedA)}; }

fs::path target_of(const fs::path& dir, const std::string& game)
{
    return det::installed_floor_path(dir, game);
}

fs::path tmp_of(const fs::path& dir, const std::string& game)
{
    return det::installed_tmp_path(dir, game);
}

det::FloorWrite commit(const fs::path& dir, const Bytes& m,
                       const std::vector<sp::Ed25519PublicKey>& keys, det::FloorFileOps& ops)
{
    return det::commit_installed_floor(dir, m, keys_of(keys), ops);
}

det::FloorWrite commit(const fs::path& dir, const Bytes& m,
                       const std::vector<sp::Ed25519PublicKey>& keys)
{
    return det::commit_installed_floor(dir, m, keys_of(keys));
}

det::FloorRead read_floor(const fs::path& dir, const std::string& game,
                          const std::vector<sp::Ed25519PublicKey>& keys)
{
    return det::read_installed_floor(dir, game, keys_of(keys));
}

// Abre `m` como o sw_init abrirá: com as chaves e o piso lido.
sp::ManifestStatus open_with_floor(const Bytes& m, const std::vector<sp::Ed25519PublicKey>& keys,
                                   std::optional<std::uint64_t> floor)
{
    sp::ManifestView view;
    sp::ReadOptions o;
    o.trusted_keys = keys_of(keys);
    o.installed_build_seq = floor;
    return sp::ManifestView::open(m, o, view);
}

// ---------------------------------------------------------------------------------------------

void test_file_name_is_sha256_of_game_id()
{
    std::cout << "[Test] nome do arquivo = SHA-256(game_id) em hex minúsculo\n";
    // Vetor conhecido do SHA-256: "abc".
    SW_CHECK(det::installed_hash_hex("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const fs::path dir = "pasta";
    SW_CHECK(det::installed_floor_path(dir, "abc") ==
             dir / "installed" /
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.manifest");
    SW_CHECK(det::installed_tmp_path(dir, "abc") ==
             dir / "installed" /
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.manifest.tmp");
    SW_CHECK(det::installed_hash_hex("abc") != det::installed_hash_hex("abd"));
}

void test_absent_floor_has_no_side_effects()
{
    std::cout << "[Test] piso ausente: sem piso, sem criar nada; avisa se já há índice/journal\n";
    const fs::path dir = fresh_dir("ausente");
    const auto keys = only_a();
    const auto before = listing(dir);
    det::FloorRead r = read_floor(dir, "jogo", keys);
    SW_CHECK(r.status == det::FloorStatus::Ok && !r.floor.has_value());
    SW_CHECK(!r.existing_cache_without_floor);
    SW_CHECK(listing(dir) == before); // a leitura não criou installed/ nem o arquivo
    SW_CHECK(!fs::exists(dir / "installed"));

    write_all(dir / "index.bin", Bytes(8, std::byte{1}));
    r = read_floor(dir, "jogo", keys);
    SW_CHECK(r.status == det::FloorStatus::Ok && !r.floor.has_value());
    SW_CHECK(r.existing_cache_without_floor); // "piso ausente em cache existente"
    cleanup(dir);
}

void test_first_install_and_exact_copy()
{
    std::cout << "[Test] primeira instalação: cópia exata dos bytes, piso lido de volta\n";
    const fs::path dir = fresh_dir("primeira");
    const auto keys = only_a();
    const Bytes m = make_manifest("jogo", 7, kSeedA);
    const det::FloorWrite w = commit(dir, m, keys);
    SW_CHECK(w.status == det::FloorStatus::Ok && w.wrote && !w.may_have_changed);
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), m)); // bytes exatos do envelope
    SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));                // nenhum .tmp sobrou
    // Só installed/ e o arquivo dele; nada mais na pasta.
    const auto list = listing(dir);
    SW_CHECK(list.size() == 2);
    const det::FloorRead r = read_floor(dir, "jogo", keys);
    SW_CHECK(r.status == det::FloorStatus::Ok && r.floor.has_value() && *r.floor == 7);
    cleanup(dir);
}

void test_write_rule_higher_equal_lower()
{
    std::cout << "[Test] regra de gravação: maior grava; igual com bytes diferentes grava; igual "
                 "idêntico não; menor nunca\n";
    const fs::path dir = fresh_dir("regra");
    const auto keys = only_a();
    const Bytes v5 = make_manifest("jogo", 5, kSeedA, "r1");
    SW_CHECK(commit(dir, v5, keys).wrote);

    // IGUAL e idêntico: nada a gravar (nenhuma operação de escrita é chamada).
    {
        FaultOps ops;
        const det::FloorWrite w = commit(dir, v5, keys, ops);
        SW_CHECK(w.status == det::FloorStatus::Ok && !w.wrote);
        SW_CHECK(ops.create_dir_calls == 0 && ops.write_calls == 0 && ops.replace_calls == 0);
        SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), v5));
    }
    // IGUAL com bytes diferentes (reassinatura): grava; o piso continua 5.
    const Bytes v5b = make_manifest("jogo", 5, kSeedA, "r2");
    SW_CHECK(!same_bytes(v5, v5b));
    {
        FaultOps ops;
        const det::FloorWrite w = commit(dir, v5b, keys, ops);
        SW_CHECK(w.status == det::FloorStatus::Ok && w.wrote && ops.replace_calls == 1 &&
                 ops.swapped);
        SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), v5b));
        SW_CHECK(*read_floor(dir, "jogo", keys).floor == 5);
    }
    // MAIOR: grava.
    const Bytes v6 = make_manifest("jogo", 6, kSeedA);
    SW_CHECK(commit(dir, v6, keys).wrote);
    SW_CHECK(*read_floor(dir, "jogo", keys).floor == 6);
    // MENOR: recusado, o arquivo fica como estava e nenhuma escrita foi tentada.
    {
        FaultOps ops;
        const det::FloorWrite w = commit(dir, v5, keys, ops);
        SW_CHECK(w.status == det::FloorStatus::Rollback &&
                 w.category == det::FloorCategory::LowerSeq);
        SW_CHECK(!w.wrote && !w.may_have_changed);
        SW_CHECK(ops.create_dir_calls == 0 && ops.write_calls == 0 && ops.replace_calls == 0);
        SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), v6));
        SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));
    }
    // Controle positivo do oráculo: o piso 6 faz o ManifestView recusar o v5 (a integração do
    // F1-67).
    SW_CHECK(open_with_floor(v5, keys, 6).code == sp::ManifestError::Rollback);
    SW_CHECK(open_with_floor(v6, keys, 6).ok());
    cleanup(dir);
}

void test_two_games_keep_independent_floors()
{
    std::cout << "[Test] dois jogos: X(5) -> Y(1) -> X(4) é rollback e o arquivo de X não muda\n";
    const fs::path dir = fresh_dir("doisjogos");
    const auto keys = only_a();
    const Bytes x5 = make_manifest("jogo.x", 5, kSeedA);
    const Bytes y1 = make_manifest("jogo.y", 1, kSeedA);
    const Bytes x4 = make_manifest("jogo.x", 4, kSeedA);
    SW_CHECK(commit(dir, x5, keys).wrote);
    SW_CHECK(commit(dir, y1, keys).wrote); // outro arquivo; o de X fica
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo.x")), x5));
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo.y")), y1));
    const auto rx = read_floor(dir, "jogo.x", keys);
    const auto ry = read_floor(dir, "jogo.y", keys);
    SW_CHECK(*rx.floor == 5 && *ry.floor == 1);
    // X(4): a leitura dá piso 5 e o ManifestView devolve Rollback; o commit também recusa.
    SW_CHECK(open_with_floor(x4, keys, rx.floor).code == sp::ManifestError::Rollback);
    SW_CHECK(commit(dir, x4, keys).status == det::FloorStatus::Rollback);
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo.x")), x5)); // byte a byte igual
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo.y")), y1)); // o de Y também
    SW_CHECK(listing(dir).size() == 3); // installed/ + 2 arquivos, sem .tmp
    cleanup(dir);
}

void test_replace_fails_before_swap()
{
    std::cout << "[Test] (b) replace falha ANTES da troca: alvo antigo, SW_E_IO\n";
    const fs::path dir = fresh_dir("antes");
    const auto keys = only_a();
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);
    SW_CHECK(commit(dir, old_m, keys).wrote);

    FaultOps ops;
    ops.replace_fault = FaultOps::ReplaceFault::FailBeforeSwap;
    const det::FloorWrite w = commit(dir, new_m, keys, ops);
    SW_CHECK(w.status == det::FloorStatus::Io && w.category == det::FloorCategory::Replace);
    SW_CHECK(!w.wrote && w.may_have_changed); // o chamador NÃO sabe: antigo ou novo
    SW_CHECK(w.detail.find("antigo ou novo") != std::string::npos);
    SW_CHECK(ops.replace_calls == 1 && !ops.swapped);
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), old_m)); // de fato antigo
    SW_CHECK(*read_floor(dir, "jogo", keys).floor == 1);
    cleanup(dir);
}

// (a) do replace real: a origem não existe => IoError, e o alvo continua como estava (a troca não
// ocorreu). Vale no Windows (MoveFileExW) e no POSIX (rename).
void test_real_replace_missing_source()
{
    std::cout << "[Test] replace real com origem inexistente: IoError e alvo intacto\n";
    const fs::path dir = fresh_dir("realrepl");
    const fs::path target = dir / "alvo.bin";
    write_all(target, Bytes(5, std::byte{7}));
    const sw::CacheErrorCode rc =
        det::real_floor_file_ops().replace(dir / "nao-existe.tmp", target);
    SW_CHECK(rc == sw::CacheErrorCode::IoError);
    SW_CHECK(same_bytes(read_all(target), Bytes(5, std::byte{7})));
    cleanup(dir);
}

void test_replace_fails_after_swap()
{
    std::cout << "[Test] (c) replace falha DEPOIS da troca: alvo NOVO em disco, SW_E_IO\n";
    const fs::path dir = fresh_dir("depois");
    const auto keys = only_a();
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);
    SW_CHECK(commit(dir, old_m, keys).wrote);
    SW_CHECK(fs::exists(target_of(dir, "jogo")) && !fs::exists(tmp_of(dir, "jogo")));

    FaultOps ops;
    ops.replace_fault = FaultOps::ReplaceFault::FailAfterSwap;
    const det::FloorWrite w = commit(dir, new_m, keys, ops);

    // O erro foi devolvido...
    SW_CHECK(w.status == det::FloorStatus::Io && w.category == det::FloorCategory::Replace);
    SW_CHECK(!w.wrote && w.may_have_changed);
    SW_CHECK(w.detail.find("antigo ou novo") != std::string::npos);
    // ...mas a troca OCORREU antes dele (controle positivo, para o teste não passar vazio):
    SW_CHECK(ops.replace_calls == 1 && ops.swapped); // o replace real devolveu sucesso
    SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));      // o .tmp virou o alvo
    const Bytes on_disk = read_all(target_of(dir, "jogo"));
    SW_CHECK(!same_bytes(on_disk, old_m) && same_bytes(on_disk, new_m)); // o conteúdo mudou
    // O alvo novo verifica e a reabertura lê o piso NOVO e rejeita um manifesto menor que ele.
    const auto r = read_floor(dir, "jogo", keys);
    SW_CHECK(r.status == det::FloorStatus::Ok && r.floor.has_value() && *r.floor == 2);
    SW_CHECK(open_with_floor(old_m, keys, r.floor).code == sp::ManifestError::Rollback);
    SW_CHECK(open_with_floor(new_m, keys, r.floor).ok());
    // Primeira instalação com a mesma falha: o alvo, que não existia, passa a existir.
    const fs::path dir2 = fresh_dir("depois2");
    FaultOps ops2;
    ops2.replace_fault = FaultOps::ReplaceFault::FailAfterSwap;
    SW_CHECK(!fs::exists(target_of(dir2, "jogo")));
    const det::FloorWrite w2 = commit(dir2, new_m, keys, ops2);
    SW_CHECK(w2.status == det::FloorStatus::Io && w2.may_have_changed && ops2.swapped);
    SW_CHECK(same_bytes(read_all(target_of(dir2, "jogo")), new_m));
    SW_CHECK(*read_floor(dir2, "jogo", keys).floor == 2);
    cleanup(dir);
    cleanup(dir2);
}

void test_failures_before_swap_leave_floor()
{
    std::cout
        << "[Test] (f) falhas em W1-W4: piso inalterado (tabela do F1-60 §6.2 como oráculo)\n";
    const auto keys = only_a();
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);

    struct Case
    {
        const char* name;
        void (*arm)(FaultOps&);
        det::FloorStatus status;
        det::FloorCategory category;
        bool installed_exists_after_first_install; // W1 falhou: installed/ não existe
    };
    const Case cases[] = {
        {"W1 criar installed/", [](FaultOps& o) { o.fail_create_dir = true; }, det::FloorStatus::Io,
         det::FloorCategory::CreateDir, false},
        {"W2/W3 erro sem escrever",
         [](FaultOps& o) { o.write_fault = FaultOps::WriteFault::FailBeforeWriting; },
         det::FloorStatus::Io, det::FloorCategory::WriteTmp, true},
        {"W2/W3 disco cheio no meio",
         [](FaultOps& o) { o.write_fault = FaultOps::WriteFault::FailAfterPartialWrite; },
         det::FloorStatus::Io, det::FloorCategory::WriteTmp, true},
        {"W4 releitura falha", [](FaultOps& o) { o.fail_readback_io = true; }, det::FloorStatus::Io,
         det::FloorCategory::VerifyTmp, true},
        {"W4 releitura sem memória", [](FaultOps& o) { o.fail_readback_oom = true; },
         det::FloorStatus::OutOfMemory, det::FloorCategory::VerifyTmp, true},
        {"W4 releitura diverge (byte trocado)", [](FaultOps& o) { o.corrupt_readback = true; },
         det::FloorStatus::Io, det::FloorCategory::VerifyTmp, true},
        {"W4 releitura curta", [](FaultOps& o) { o.truncate_readback = true; },
         det::FloorStatus::Io, det::FloorCategory::VerifyTmp, true},
    };

    for (const Case& c : cases) {
        std::cout << "  - " << c.name << "\n";
        // (1) já havia piso antigo (1): continua 1 e o alvo é byte a byte o antigo.
        {
            const fs::path dir = fresh_dir("w_antigo");
            SW_CHECK(commit(dir, old_m, keys).wrote);
            FaultOps ops;
            c.arm(ops);
            const det::FloorWrite w = commit(dir, new_m, keys, ops);
            SW_CHECK(w.status == c.status && w.category == c.category);
            SW_CHECK(!w.wrote && !w.may_have_changed);
            SW_CHECK(ops.replace_calls == 0 || c.category == det::FloorCategory::Replace);
            SW_CHECK(ops.replace_calls == 0 && !ops.swapped); // nunca chegou à troca
            SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), old_m));
            const auto r = read_floor(dir, "jogo", keys);
            SW_CHECK(r.status == det::FloorStatus::Ok && *r.floor == 1);
            // O .tmp (se ficou) nunca é lido: o piso lido é o do alvo, e a próxima gravação o
            // substitui.
            SW_CHECK(commit(dir, new_m, keys).wrote);
            SW_CHECK(*read_floor(dir, "jogo", keys).floor == 2);
            SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));
            cleanup(dir);
        }
        // (2) primeira instalação: o alvo continua ausente, o piso continua "nenhum".
        {
            const fs::path dir = fresh_dir("w_novo");
            FaultOps ops;
            c.arm(ops);
            const det::FloorWrite w = commit(dir, new_m, keys, ops);
            SW_CHECK(w.status == c.status && w.category == c.category && !w.may_have_changed);
            SW_CHECK(!fs::exists(target_of(dir, "jogo")));
            SW_CHECK(fs::exists(dir / "installed") == c.installed_exists_after_first_install);
            const auto r = read_floor(dir, "jogo", keys);
            SW_CHECK(r.status == det::FloorStatus::Ok && !r.floor.has_value());
            cleanup(dir);
        }
    }
}

void test_orphan_tmp()
{
    std::cout << "[Test] .tmp órfão é ignorado na leitura e sobrescrito (e truncado) na gravação\n";
    const fs::path dir = fresh_dir("orfao");
    const auto keys = only_a();
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);
    SW_CHECK(commit(dir, old_m, keys).wrote);
    // Lixo MAIOR que o manifesto novo: se a gravação não truncasse, a cauda ficaria.
    write_all(tmp_of(dir, "jogo"), Bytes(new_m.size() + 4096, std::byte{0x5A}));
    const auto r = read_floor(dir, "jogo", keys);
    SW_CHECK(r.status == det::FloorStatus::Ok && *r.floor == 1); // o .tmp não é lido
    SW_CHECK(commit(dir, new_m, keys).wrote);
    SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), new_m));
    SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));
    // Também sem alvo: .tmp órfão de uma primeira instalação que caiu.
    const fs::path dir2 = fresh_dir("orfao2");
    fs::create_directories(dir2 / "installed");
    write_all(tmp_of(dir2, "jogo"), Bytes(10, std::byte{0x11}));
    SW_CHECK(!read_floor(dir2, "jogo", keys).floor.has_value()); // ausente, apesar do .tmp
    SW_CHECK(commit(dir2, new_m, keys).wrote);
    SW_CHECK(same_bytes(read_all(target_of(dir2, "jogo")), new_m));
    cleanup(dir);
    cleanup(dir2);
}

// Estado existente ruim => status Io, categoria certa, arquivo intacto, nunca "primeira
// instalação".
void expect_policy(const fs::path& dir, const std::string& game,
                   const std::vector<sp::Ed25519PublicKey>& keys, det::FloorCategory category,
                   const Bytes& expected_file)
{
    const auto before = listing(dir);
    const det::FloorRead r = read_floor(dir, game, keys);
    SW_CHECK(r.status == det::FloorStatus::Io);
    SW_CHECK(r.category == category);
    SW_CHECK(!r.floor.has_value());
    SW_CHECK(!r.detail.empty() && r.detail.find(det::to_string(category)) != std::string::npos);
    SW_CHECK(r.detail.find(".manifest") != std::string::npos); // o diagnóstico traz o caminho
    SW_CHECK(same_bytes(read_all(target_of(dir, game)), expected_file)); // não foi tocado
    SW_CHECK(listing(dir) == before);                                    // nem apagado nem criado
    // O commit também recusa e NÃO reescreve (política conservadora).
    const Bytes newer = make_manifest(game, 99, kSeedA);
    const det::FloorWrite w = commit(dir, newer, keys);
    SW_CHECK(w.status == det::FloorStatus::Io && w.category == category && !w.wrote);
    SW_CHECK(same_bytes(read_all(target_of(dir, game)), expected_file));
    SW_CHECK(listing(dir) == before);
}

void test_policy_bad_state()
{
    std::cout << "[Test] (g) política conservadora: estado ruim => SW_E_IO com a categoria, nunca "
                 "primeira instalação\n";
    const auto keys = only_a();
    const Bytes good = make_manifest("jogo", 3, kSeedA);

    auto fresh_with = [&](const char* name, const Bytes& content) {
        const fs::path dir = fresh_dir(name);
        fs::create_directories(dir / "installed");
        write_all(target_of(dir, "jogo"), content);
        return dir;
    };

    { // vazio
        const fs::path dir = fresh_with("p_vazio", Bytes{});
        expect_policy(dir, "jogo", keys, det::FloorCategory::Format, Bytes{});
        cleanup(dir);
    }
    { // lixo
        const Bytes junk(300, std::byte{0x41});
        const fs::path dir = fresh_with("p_lixo", junk);
        expect_policy(dir, "jogo", keys, det::FloorCategory::Format, junk);
        cleanup(dir);
    }
    { // truncado (metade do envelope válido)
        const Bytes half(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
        const fs::path dir = fresh_with("p_trunc", half);
        expect_policy(dir, "jogo", keys, det::FloorCategory::Format, half);
        cleanup(dir);
    }
    { // truncado só no envelope (menos de 104 bytes)
        const Bytes tiny(good.begin(), good.begin() + 50);
        const fs::path dir = fresh_with("p_tiny", tiny);
        expect_policy(dir, "jogo", keys, det::FloorCategory::Format, tiny);
        cleanup(dir);
    }
    { // assinatura inválida (um bit da assinatura trocado): só com o libsodium real
        if (is_sodium_stub()) {
            std::cout << "  PULADO (stub): assinatura inválida precisa do libsodium real\n";
        } else {
            Bytes bad = good;
            bad[40] ^= std::byte{0x01}; // envelope: assinatura em [40, 104)
            const fs::path dir = fresh_with("p_assin", bad);
            expect_policy(dir, "jogo", keys, det::FloorCategory::Signature, bad);
            cleanup(dir);
            std::cout << "  assinatura inválida: coberto (libsodium real)\n";
        }
    }
    { // payload alterado com a assinatura antiga: idem
        if (!is_sodium_stub()) {
            Bytes bad = good;
            bad[good.size() - 1] ^= std::byte{0x80};
            const fs::path dir = fresh_with("p_payload", bad);
            expect_policy(dir, "jogo", keys, det::FloorCategory::Signature, bad);
            cleanup(dir);
        }
    }
    { // chave que saiu da lista
        const fs::path dir = fresh_with("p_chave", make_manifest("jogo", 3, kSeedB));
        expect_policy(dir, "jogo", keys, det::FloorCategory::UnknownKey,
                      make_manifest("jogo", 3, kSeedB));
        const det::FloorRead r = read_floor(dir, "jogo", keys);
        // O diagnóstico traz o key_id (16 hex) de quem assinou.
        const std::string kid = sp::to_hex(sp::key_id_of(public_key_of(kSeedB)));
        SW_CHECK(r.detail.find(kid) != std::string::npos);
        cleanup(dir);
    }
    { // manifesto de OUTRO jogo guardado com o nome deste
        const Bytes other = make_manifest("outro.jogo", 3, kSeedA);
        const fs::path dir = fresh_with("p_gameid", other);
        expect_policy(dir, "jogo", keys, det::FloorCategory::GameIdMismatch, other);
        cleanup(dir);
    }
    { // min_reader que este leitor não suporta (assinatura VÁLIDA)
        sp::ManifestModel m = model_of("jogo", 3);
        m.min_reader = 1;
        const Bytes unsupported = sp::encode_manifest(m, kSeedA);
        const fs::path dir = fresh_with("p_minreader", unsupported);
        expect_policy(dir, "jogo", keys, det::FloorCategory::Unsupported, unsupported);
        cleanup(dir);
    }
    { // o "arquivo" é uma pasta
        const fs::path dir = fresh_dir("p_pasta");
        fs::create_directories(target_of(dir, "jogo"));
        const det::FloorRead r = read_floor(dir, "jogo", keys);
        SW_CHECK(r.status == det::FloorStatus::Io && r.category == det::FloorCategory::Read);
        SW_CHECK(fs::is_directory(target_of(dir, "jogo")));
        cleanup(dir);
    }
    { // `installed` é um arquivo comum (ENOTDIR não é "ausente")
        const fs::path dir = fresh_dir("p_installed_arquivo");
        write_all(dir / "installed", Bytes(3, std::byte{1}));
        const det::FloorRead r = read_floor(dir, "jogo", keys);
        SW_CHECK(r.status == det::FloorStatus::Io && r.category == det::FloorCategory::Read);
        SW_CHECK(commit(dir, good, keys).status == det::FloorStatus::Io);
        SW_CHECK(fs::is_regular_file(dir / "installed"));
        cleanup(dir);
    }
    { // erros de leitura reportados pela operação: grande demais / E/S / memória
        const fs::path dir = fresh_dir("p_ops");
        struct S
        {
            det::FileReadStatus st;
            det::FloorStatus status;
            det::FloorCategory cat;
        };
        const S sts[] = {
            {det::FileReadStatus::TooLarge, det::FloorStatus::Io, det::FloorCategory::Format},
            {det::FileReadStatus::Io, det::FloorStatus::Io, det::FloorCategory::Read},
            {det::FileReadStatus::OutOfMemory, det::FloorStatus::OutOfMemory,
             det::FloorCategory::Read}};
        for (const S& s : sts) {
            FaultOps ops;
            ops.read_status_for_target = s.st;
            const det::FloorRead r = det::read_installed_floor(dir, "jogo", keys_of(keys), ops);
            SW_CHECK(r.status == s.status && !r.floor.has_value());
            if (s.status == det::FloorStatus::Io) SW_CHECK(r.category == s.cat);
            const det::FloorWrite w = commit(dir, good, keys, ops);
            SW_CHECK(w.status == s.status && !w.wrote && ops.write_calls == 0);
        }
        SW_CHECK(!fs::exists(dir / "installed"));
        cleanup(dir);
    }
}

void test_policy_new_manifest_must_verify()
{
    std::cout << "[Test] só se grava um manifesto que passa na verificação completa\n";
    const fs::path dir = fresh_dir("novo_invalido");
    const auto keys = only_a();
    const Bytes junk(200, std::byte{0x41});
    const det::FloorWrite w1 = commit(dir, junk, keys);
    SW_CHECK(w1.status == det::FloorStatus::Io && w1.category == det::FloorCategory::NewManifest);
    // Assinado por chave que NÃO é confiável.
    const det::FloorWrite w2 = commit(dir, make_manifest("jogo", 1, kSeedB), keys);
    SW_CHECK(w2.status == det::FloorStatus::Io && w2.category == det::FloorCategory::NewManifest);
    // min_reader não suportado.
    sp::ManifestModel m = model_of("jogo", 1);
    m.min_reader = 1;
    const det::FloorWrite w3 = commit(dir, sp::encode_manifest(m, kSeedA), keys);
    SW_CHECK(w3.status == det::FloorStatus::Io && w3.category == det::FloorCategory::NewManifest);
    if (!is_sodium_stub()) {
        Bytes bad = make_manifest("jogo", 1, kSeedA);
        bad[41] ^= std::byte{0x02};
        const det::FloorWrite w4 = commit(dir, bad, keys);
        SW_CHECK(w4.status == det::FloorStatus::Io &&
                 w4.category == det::FloorCategory::NewManifest);
    }
    SW_CHECK(!fs::exists(dir / "installed")); // nenhuma tentativa chegou a tocar na pasta
    cleanup(dir);
}

void test_key_rotation()
{
    std::cout << "[Test] (g) rotação de chave: com sobreposição funciona; sem ela, SW_E_IO e o "
                 "piso não cai\n";
    const fs::path dir = fresh_dir("rotacao");
    const std::vector<sp::Ed25519PublicKey> only_old = {public_key_of(kSeedA)};
    const std::vector<sp::Ed25519PublicKey> only_new = {public_key_of(kSeedB)};
    const std::vector<sp::Ed25519PublicKey> both = {public_key_of(kSeedB), public_key_of(kSeedA)};

    const Bytes v5_old = make_manifest("jogo", 5, kSeedA);
    SW_CHECK(commit(dir, v5_old, only_old).wrote);

    // Sem sobreposição: a cópia (assinada pela chave antiga) não verifica => SW_E_IO, e NADA mudou.
    const Bytes v6_new = make_manifest("jogo", 6, kSeedB);
    {
        const auto r = read_floor(dir, "jogo", only_new);
        SW_CHECK(r.status == det::FloorStatus::Io && r.category == det::FloorCategory::UnknownKey);
        const auto w = commit(dir, v6_new, only_new);
        SW_CHECK(w.status == det::FloorStatus::Io && w.category == det::FloorCategory::UnknownKey &&
                 !w.wrote);
        SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), v5_old));
    }
    // Com sobreposição (lista nova + chave antiga): lê o piso 5, aceita o v6 e REESCREVE a cópia
    // já assinada pela chave nova.
    {
        const auto r = read_floor(dir, "jogo", both);
        SW_CHECK(r.status == det::FloorStatus::Ok && *r.floor == 5);
        SW_CHECK(open_with_floor(v6_new, both, r.floor).ok());
        SW_CHECK(commit(dir, v6_new, both).wrote);
        SW_CHECK(same_bytes(read_all(target_of(dir, "jogo")), v6_new));
    }
    // Agora a chave antiga pode sair da lista.
    {
        const auto r = read_floor(dir, "jogo", only_new);
        SW_CHECK(r.status == det::FloorStatus::Ok && *r.floor == 6);
    }
    // Reassinatura sem mudar o build_seq (igual, bytes diferentes): grava, piso inalterado.
    const Bytes v6_resigned = make_manifest("jogo", 6, kSeedA, "outro-build-id");
    SW_CHECK(commit(dir, v6_resigned, both).wrote);
    SW_CHECK(*read_floor(dir, "jogo", both).floor == 6);
    cleanup(dir);
}

// (e) queda DE VERDADE do processo filho num ponto exato do commit.
void test_crash_points()
{
    std::cout << "[Test] (e) queda do processo no meio do commit: antes da troca = piso antigo; "
                 "depois = novo\n";
    const auto keys = only_a();
    const sp::Ed25519PublicKey key = keys[0];
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);

    struct Case
    {
        const char* mode;
        bool floor_is_new;
        bool tmp_survives;
    };
    const Case cases[] = {
        {"die-before-write", false, false},  // nada foi gravado
        {"die-after-write", false, true},    // .tmp completo e sincronizado, alvo intocado
        {"die-before-replace", false, true}, // idem, depois da releitura W4
        {"die-after-replace", true, false},  // a troca (rename + fsync da pasta) já ocorreu
    };
    for (const Case& c : cases) {
        std::cout << "  - " << c.mode << "\n";
        const fs::path dir = fresh_dir("queda");
        const fs::path aux = fresh_dir("queda_aux");
        SW_CHECK(commit(dir, old_m, keys).wrote);
        write_all(aux / "novo.manifest", new_m);

        auto child =
            sw::test::Child::spawn(g_tool, {"floor-commit", dir.string(),
                                            (aux / "novo.manifest").string(), hex_of(key), c.mode});
        SW_CHECK(child.has_value());
        const sw::test::ChildExit e = child->wait(std::chrono::seconds(60));
        SW_CHECK(e.exited && e.by_kill); // morreu à força, no ponto combinado (não saiu sozinho)

        const Bytes on_disk = read_all(target_of(dir, "jogo"));
        if (c.floor_is_new) {
            SW_CHECK(
                same_bytes(on_disk, new_m)); // controle positivo: a troca ocorreu antes da queda
        } else {
            SW_CHECK(same_bytes(on_disk, old_m)); // e aqui NÃO ocorreu
        }
        SW_CHECK(fs::exists(tmp_of(dir, "jogo")) == c.tmp_survives);
        const auto r = read_floor(dir, "jogo", keys);
        SW_CHECK(r.status == det::FloorStatus::Ok && r.floor.has_value());
        SW_CHECK(*r.floor == (c.floor_is_new ? 2u : 1u));
        // A cópia que ficou verifica e rejeita um manifesto com build_seq menor que o piso.
        const Bytes lower = make_manifest("jogo", 1, kSeedA, "inferior");
        if (c.floor_is_new)
            SW_CHECK(open_with_floor(lower, keys, r.floor).code == sp::ManifestError::Rollback);
        // Depois da queda, a próxima gravação funciona (substitui o .tmp órfão).
        SW_CHECK(commit(dir, new_m, keys).wrote == !c.floor_is_new);
        SW_CHECK(*read_floor(dir, "jogo", keys).floor == 2);
        SW_CHECK(!fs::exists(tmp_of(dir, "jogo")));
        cleanup(dir);
        cleanup(aux);
    }
    // Controle positivo do roteiro: o modo "ok" do filho completa o commit e grava o piso novo.
    {
        const fs::path dir = fresh_dir("queda_ok");
        const fs::path aux = fresh_dir("queda_ok_aux");
        SW_CHECK(commit(dir, old_m, keys).wrote);
        write_all(aux / "novo.manifest", new_m);
        auto child =
            sw::test::Child::spawn(g_tool, {"floor-commit", dir.string(),
                                            (aux / "novo.manifest").string(), hex_of(key), "ok"});
        SW_CHECK(child.has_value());
        const sw::test::ChildExit e = child->wait(std::chrono::seconds(60));
        SW_CHECK(e.exited && !e.by_kill && e.code == 0);
        SW_CHECK(*read_floor(dir, "jogo", keys).floor == 2);
        cleanup(dir);
        cleanup(aux);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "uso: sw-core-floor-tests <posse-tool>\n");
        return 2;
    }
    g_tool = argv[1];
    SW_CHECK(fs::exists(g_tool));
    SW_CHECK(sp::crypto_ready());
    std::cout << "libsodium: " << sp::crypto_version()
              << (is_sodium_stub() ? " (STUB: casos de assinatura ruim PULADOS)" : " (real)")
              << "\n";
    test_file_name_is_sha256_of_game_id();
    test_absent_floor_has_no_side_effects();
    test_first_install_and_exact_copy();
    test_write_rule_higher_equal_lower();
    test_two_games_keep_independent_floors();
    test_replace_fails_before_swap();
    test_real_replace_missing_source();
    test_replace_fails_after_swap();
    test_failures_before_swap_leave_floor();
    test_orphan_tmp();
    test_policy_bad_state();
    test_policy_new_manifest_must_verify();
    test_key_rotation();
    test_crash_points();
    std::cout << "TODOS OS TESTES DO PISO PASSARAM\n";
    return 0;
}
