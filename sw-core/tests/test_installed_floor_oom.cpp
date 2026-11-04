// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69: falta de memória no piso anti-rollback (read_installed_floor e commit_installed_floor,
// ambos `noexcept` e ambos declarando FloorStatus::OutOfMemory). Duas partes:
//
// 1) LEVANTAMENTO DOS CAMINHOS EXECUTADOS (primeiro, passada SEPARADA, SEM injeção de falha):
//    registra as pilhas das alocações que cada cenário executa (alloc_survey.hpp). Só informa:
//    não passa nem falha pelo número de alocações, não é uma lista completa das alocações do
//    código e NÃO é evidência de ausência de alocação.
// 2) VARREDURA DE FALHAS: o operator new global DESTE executável (oom_support.hpp) falha com
//    bad_alloc na K-ésima alocação. Para cada K = 1, 2, ... até a operação terminar sem a falha
//    disparar, e para cada cenário, confere:
//   LEITURA  - o resultado NUNCA é Ok quando a falha disparou (em particular: uma falta de memória
//              jamais vira "arquivo ausente"/primeira instalação, que baixaria o piso); a pasta não
//              muda; nenhum handle fica aberto; a mesma leitura sem falha dá o resultado de base.
//   GRAVAÇÃO - o resultado nunca é Ok nem `wrote`; o arquivo do piso fica EXATAMENTE no estado
//              anterior, exceto se o resultado disser `may_have_changed` (e então o arquivo tem o
//              nome esperado e os bytes EXATOS do manifesto antigo ou do novo, e o piso lido é o
//              antigo ou o novo, nunca menor); se a troca aconteceu o resultado diz
//              `may_have_changed`; o .tmp, se existir, tem exatamente os bytes do manifesto novo;
//              nenhum outro arquivo aparece ou muda; nenhum handle fica aberto; e uma nova
//              gravação sem falha chega ao estado final esperado (o .tmp que sobrou é consumido).
// Não há pontos excluídos: se uma alocação existe no cenário, ela é varrida. A instrumentação de
// diagnóstico (set_terminate, DbgHelp, levantamento) não é evidência de ausência de alocação.
//
// Com o libsodium STUB local a verificação de assinatura sempre aceita; isto não muda nenhuma
// alocação nem o que é conferido aqui.
#include "alloc_survey.hpp"
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

// Estado do arquivo do piso, do .tmp e da pasta, para comparar antes/depois.
struct Disk
{
    bool target_exists = false;
    Bytes target;
    bool tmp_exists = false;
    Bytes tmp;
    std::vector<std::pair<std::string, std::uintmax_t>> files;
};

Disk snapshot(const fs::path& dir)
{
    Disk d;
    const fs::path t = det::installed_floor_path(dir, kGame);
    const fs::path m = det::installed_tmp_path(dir, kGame);
    d.target_exists = fs::exists(t);
    if (d.target_exists) d.target = read_all(t);
    d.tmp_exists = fs::exists(m);
    if (d.tmp_exists) d.tmp = read_all(m);
    d.files = listing(dir);
    return d;
}

std::string rel_of(const fs::path& dir, const fs::path& p)
{
    return fs::relative(p, dir).generic_string();
}

bool same_read(const det::FloorRead& a, const det::FloorRead& b)
{
    return a.status == b.status && a.category == b.category && a.floor == b.floor &&
           a.existing_cache_without_floor == b.existing_cache_without_floor;
}

// ------------------------------------------------------------ auditoria do estado após a falha
struct AuditIn
{
    bool may_have_changed = false;
    const Bytes* new_bytes = nullptr;
    std::optional<std::uint64_t> pre_floor;
    std::uint64_t new_seq = 0;
    std::optional<std::uint64_t> post_floor; // piso lido depois da falha (nullopt: sem piso)
    std::string target_rel;                  // caminho relativo EXATO esperado do alvo
    std::string tmp_rel;                     // idem, do .tmp
};

const std::pair<std::string, std::uintmax_t>* find_file(const Disk& d, const std::string& name)
{
    for (const auto& f : d.files)
        if (f.first == name) return &f;
    return nullptr;
}

// "" se o estado depois da falha é aceitável; senão, o motivo. NÃO libera "qualquer .manifest": o
// alvo só é aceito com o NOME exato, com os bytes EXATOS do antigo ou do novo e com o piso lido
// sendo o antigo ou o novo (nunca menor). Qualquer outro arquivo novo ou alterado reprova.
std::string audit_after_failure(const Disk& pre, const Disk& post, const AuditIn& in)
{
    if (!in.may_have_changed) {
        if (post.target_exists != pre.target_exists)
            return "o alvo apareceu/sumiu sem may_have_changed";
        if (pre.target_exists && !same_bytes(post.target, pre.target))
            return "o alvo mudou sem may_have_changed";
        if (in.post_floor != in.pre_floor) return "o piso lido mudou sem may_have_changed";
    } else {
        if (!post.target_exists) return "may_have_changed mas o alvo nao existe";
        const bool is_old = pre.target_exists && same_bytes(post.target, pre.target);
        const bool is_new = same_bytes(post.target, *in.new_bytes);
        if (!is_old && !is_new) return "o alvo nao tem os bytes do manifesto antigo nem do novo";
        if (!in.post_floor.has_value()) return "o piso nao e legivel depois de may_have_changed";
        const bool floor_old = in.pre_floor.has_value() && *in.post_floor == *in.pre_floor;
        const bool floor_new = *in.post_floor == in.new_seq;
        if (!floor_old && !floor_new) return "o piso lido nao e o antigo nem o novo";
        if (in.pre_floor.has_value() && *in.post_floor < *in.pre_floor) return "piso menor";
    }
    // Arquivos: o alvo (nome exato), o .tmp (nome exato) e a pasta installed/ são os únicos que
    // podem aparecer; todo o resto tem de estar idêntico (mesmo nome e tamanho).
    for (const auto& f : post.files) {
        if (f.first == "installed" || f.first == in.target_rel || f.first == in.tmp_rel) continue;
        const auto* before = find_file(pre, f.first);
        if (before == nullptr) return "arquivo estranho: " + f.first;
        if (before->second != f.second) return "arquivo alterado: " + f.first;
    }
    for (const auto& f : pre.files) {
        if (f.first == in.target_rel) continue;
        const auto* after = find_file(post, f.first);
        if (after == nullptr) return "arquivo sumiu: " + f.first;
    }
    // O .tmp só pode existir completo: exatamente os bytes do manifesto novo.
    if (post.tmp_exists && !same_bytes(post.tmp, *in.new_bytes))
        return "o .tmp esta parcial, truncado ou corrompido";
    return "";
}

// Controles positivos do próprio auditor: estados bons passam e cada defeito reprova.
void test_audit_self_check()
{
    std::cout << "[Test] auditor do estado pos-falha: estados bons passam, defeitos reprovam\n";
    const Bytes novo(10, std::byte{0xAA});
    Bytes outro = novo;
    outro[3] ^= std::byte{1};
    const std::string alvo = "installed/aaaa.manifest";
    const std::string tmp = "installed/aaaa.manifest.tmp";

    AuditIn in;
    in.may_have_changed = true;
    in.new_bytes = &novo;
    in.pre_floor = std::nullopt;
    in.new_seq = 5;
    in.post_floor = 5;
    in.target_rel = alvo;
    in.tmp_rel = tmp;

    Disk pre; // primeira instalação: nada
    Disk bom;
    bom.target_exists = true;
    bom.target = novo;
    bom.files = {{"installed", 0}, {alvo, 10}};
    SW_CHECK(audit_after_failure(pre, bom, in).empty()); // alvo novo com may_have_changed: bom

    Disk com_tmp = bom;
    com_tmp.tmp_exists = true;
    com_tmp.tmp = novo;
    com_tmp.files.push_back({tmp, 10});
    SW_CHECK(audit_after_failure(pre, com_tmp, in).empty()); // .tmp completo: bom

    Disk outro_hash = bom; // arquivo com nome diferente (outro hash)
    outro_hash.files = {{"installed", 0}, {"installed/bbbb.manifest", 10}};
    SW_CHECK(!audit_after_failure(pre, outro_hash, in).empty());

    Disk corrompido = bom; // conteúdo corrompido (um byte)
    corrompido.target = outro;
    SW_CHECK(!audit_after_failure(pre, corrompido, in).empty());

    Disk tmp_truncado = com_tmp; // .tmp truncado
    tmp_truncado.tmp.pop_back();
    SW_CHECK(!audit_after_failure(pre, tmp_truncado, in).empty());

    Disk tmp_ruim = com_tmp; // .tmp com outro conteúdo
    tmp_ruim.tmp = outro;
    SW_CHECK(!audit_after_failure(pre, tmp_ruim, in).empty());

    Disk extra = bom; // arquivo estranho a mais
    extra.files.push_back({"installed/lixo.bin", 1});
    SW_CHECK(!audit_after_failure(pre, extra, in).empty());

    AuditIn sem_flag = in; // alvo apareceu mas o resultado não diz may_have_changed
    sem_flag.may_have_changed = false;
    SW_CHECK(!audit_after_failure(pre, bom, sem_flag).empty());

    AuditIn piso_errado = in; // piso lido nem antigo nem novo
    piso_errado.post_floor = 4;
    SW_CHECK(!audit_after_failure(pre, bom, piso_errado).empty());

    AuditIn menor = in; // piso menor que o antigo
    menor.pre_floor = 9;
    Disk pre9;
    pre9.target_exists = true;
    pre9.target = Bytes(10, std::byte{0x11});
    pre9.files = {{"installed", 0}, {alvo, 10}};
    menor.post_floor = 5; // o novo (5) é menor que o antigo (9)
    SW_CHECK(!audit_after_failure(pre9, bom, menor).empty());
}

// ------------------------------------------------------------------------- ops com etapas
// Operações reais que só marcam a ETAPA (W1..W5) para o levantamento; opcionalmente fazem a troca
// de verdade e devolvem erro (o cenário "erro de replace depois da troca").
class StageOps final : public det::FloorFileOps
{
public:
    bool error_after_swap = false;

    sw::CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        stage("W1 create_directory");
        const sw::CacheErrorCode rc = det::real_floor_file_ops().create_directory(dir);
        stage("entre W1 e W2");
        return rc;
    }
    sw::CacheErrorCode write_file_durable(const fs::path& file,
                                          std::span<const std::byte> data) noexcept override
    {
        stage("W2+W3 write_file_durable");
        const sw::CacheErrorCode rc = det::real_floor_file_ops().write_file_durable(file, data);
        stage("entre W3 e W4");
        return rc;
    }
    det::FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                                  Bytes& out) noexcept override
    {
        const bool is_tmp = has_tmp_suffix(file);
        stage(is_tmp ? "W4 read_file(.tmp)" : "leitura read_file(alvo)");
        const det::FileReadStatus rc = det::real_floor_file_ops().read_file(file, max_bytes, out);
        stage(is_tmp ? "entre W4 e W5" : "apos leitura do alvo (verificacao)");
        return rc;
    }
    sw::CacheErrorCode replace(const fs::path& from, const fs::path& to) noexcept override
    {
        stage("W5 replace");
        sw::CacheErrorCode rc = det::real_floor_file_ops().replace(from, to);
        if (error_after_swap && rc == sw::CacheErrorCode::Success) rc = sw::CacheErrorCode::IoError;
        stage("apos W5");
        return rc;
    }

private:
    static void stage(const char* s) noexcept { oom::survey::g_stage.store(s); }
};

// ----------------------------------------------------------------------------------- casos
struct ReadCase
{
    const char* name;
    Setup setup;
};

struct CommitCase
{
    const char* name;
    Setup setup;
    Bytes new_manifest;
    std::uint64_t new_seq;
    // resultado esperado SEM falha
    det::FloorStatus expect_status;
    bool expect_wrote;
    // a operação de replace faz a troca de verdade e só então devolve erro
    bool inject_replace_error_after_swap = false;
};

// ------------------------------------------------------------------ levantamento (sem falha)
void survey_read(const ReadCase& c)
{
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    const fs::path dir = fresh_dir("svread");
    c.setup(dir);
    StageOps ops;
    (void)det::read_installed_floor(dir, kGame, keys_of(keys), ops); // aquecimento, sem captura
    oom::survey::reset_and_start(c.name);
    (void)det::read_installed_floor(dir, kGame, keys_of(keys), ops);
    oom::survey::stop();
    oom::survey::report(c.name);
    cleanup(dir);
}

void survey_commit(const CommitCase& c)
{
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    {
        const fs::path warm = fresh_dir("svwarm"); // aquecimento em outra pasta, sem captura
        c.setup(warm);
        StageOps ops;
        ops.error_after_swap = c.inject_replace_error_after_swap;
        (void)det::commit_installed_floor(warm, c.new_manifest, keys_of(keys), ops);
        cleanup(warm);
    }
    const fs::path dir = fresh_dir("svwrite");
    c.setup(dir);
    StageOps ops;
    ops.error_after_swap = c.inject_replace_error_after_swap;
    oom::survey::reset_and_start(c.name);
    (void)det::commit_installed_floor(dir, c.new_manifest, keys_of(keys), ops);
    oom::survey::stop();
    oom::survey::report(c.name);
    cleanup(dir);
}

// ----------------------------------------------------------------------------------- leitura
void sweep_read(const ReadCase& c)
{
    const char* name = c.name;
    std::cout << "[Test] varredura OOM, leitura do piso: " << name << std::endl;
    const auto keys = std::vector<sp::Ed25519PublicKey>{public_key_of(kSeed)};
    const fs::path dir = fresh_dir("oomread");
    c.setup(dir);
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
    const std::string target_rel = rel_of(dir, det::installed_floor_path(dir, kGame));
    const std::string tmp_rel = rel_of(dir, tmp);
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

        // Estado depois da falha: ver audit_after_failure (nome exato, bytes exatos, piso lido).
        AuditIn in;
        in.may_have_changed = w.may_have_changed;
        in.new_bytes = &c.new_manifest;
        in.pre_floor = pre_floor;
        in.new_seq = c.new_seq;
        {
            const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys));
            if (post.target_exists) SW_CHECK(r.status == det::FloorStatus::Ok);
            if (r.status == det::FloorStatus::Ok) in.post_floor = r.floor;
        }
        in.target_rel = target_rel;
        in.tmp_rel = tmp_rel;
        const std::string why = audit_after_failure(pre, post, in);
        if (!why.empty())
            std::cerr << "ESTADO INVALIDO apos falha em K=" << k << " (" << c.name << "): " << why
                      << " (may_have_changed=" << w.may_have_changed << ")" << std::endl;
        SW_CHECK(why.empty());

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
            SW_CHECK(fin.target_exists && same_bytes(fin.target, c.new_manifest));
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

    test_audit_self_check();

    const std::vector<ReadCase> reads = {
        {"piso presente", [&](const fs::path& d) { install(d, m7, keys); }},
        {"primeira instalacao (sem installed/)", [&](const fs::path&) {}},
        {"cache existente sem piso (index.bin presente)",
         [&](const fs::path& d) { write_all(d / "index.bin", Bytes(4, std::byte{1})); }},
        {"installed e um arquivo comum",
         [&](const fs::path& d) { write_all(d / "installed", Bytes(3, std::byte{2})); }},
    };
    std::vector<CommitCase> commits = {
        {"primeira instalacao", [&](const fs::path&) {}, m5, 5, det::FloorStatus::Ok, true},
        {"atualizacao 5 -> 7", [&](const fs::path& d) { install(d, m5, keys); }, m7, 7,
         det::FloorStatus::Ok, true},
        {"mesmo build_seq com bytes diferentes (reassinatura)",
         [&](const fs::path& d) { install(d, m5, keys); }, m5_reissue, 5, det::FloorStatus::Ok,
         true},
        {"copia identica (nada a gravar)", [&](const fs::path& d) { install(d, m5, keys); }, m5, 5,
         det::FloorStatus::Ok, false},
        {"downgrade recusado (9 -> 7)", [&](const fs::path& d) { install(d, m9, keys); }, m7, 7,
         det::FloorStatus::Rollback, false},
    };
    CommitCase after_swap{"erro de replace DEPOIS da troca (5 -> 7)",
                          [&](const fs::path& d) { install(d, m5, keys); },
                          m7,
                          7,
                          det::FloorStatus::Io,
                          false};
    after_swap.inject_replace_error_after_swap = true;
    commits.push_back(after_swap);

    // 1) Levantamento dos caminhos executados: passada SEPARADA, SEM injecao de falha. Só informa.
    std::cout << "[Levantamento] passada sem injecao de falha; informa os caminhos executados, "
                 "nao e lista completa nem evidencia de ausencia de alocacao"
              << std::endl;
    for (const ReadCase& c : reads)
        survey_read(c);
    for (const CommitCase& c : commits)
        survey_commit(c);

    // 2) Varredura de falhas (sem exclusoes).
    for (const ReadCase& c : reads)
        sweep_read(c);
    for (const CommitCase& c : commits)
        sweep_commit(c);

    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA DO PISO PASSARAM" << std::endl;
    return 0;
}
