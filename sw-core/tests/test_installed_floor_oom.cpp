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
//              gravação SEM falha (operações reais) tem o resultado calculado do estado do alvo já
//              validado: alvo com os bytes do manifesto novo => Ok, wrote=false, bytes preservados;
//              senão Ok/wrote=true e bytes novos (o .tmp que sobrou é consumido) ou a recusa do
//              cenário.
// Não há pontos excluídos: se uma alocação existe no cenário, ela é varrida. A instrumentação de
// diagnóstico (set_terminate, DbgHelp, levantamento) não é evidência de ausência de alocação.
//
// MODO DA VARREDURA. Em Linux e em Release a varredura é EM PROCESSO (uma execução percorre todos
// os K). No MSVC em Debug (_ITERATOR_DEBUG_LEVEL != 0) um construtor `noexcept` da STL que aloca o
// proxy dos iteradores vira std::terminate, que mata o processo: a varredura em processo revelaria
// UM ponto por rodada de CI. Por isso lá cada K roda num PROCESSO-FILHO (`--ponto
// <leitura|gravacao> <indice> <K>`, fixture nova, falha armada só em torno da operação). O pai
// distingue sucesso, falha de asserção, terminate, timeout, falha ao iniciar o filho e resultado
// inconsistente; confere que o ponto K foi alcançado e a injeção disparou (um filho que termina
// antes de K é o FIM do cenário e não conta como cobertura); percorre TODOS os K de TODOS os
// cenários e imprime o mapa K -> pilha do ponto da injeção (símbolos resolvidos pelo pai) e os
// tempos. `--filhos` e `--em-processo` forçam o modo (`--filhos` só no Windows). Nenhum ponto é
// omitido nem a execução encerrada para declarar o levantamento concluído; o limite de 600 s do
// ctest é de execução.
//
// Com o libsodium STUB local a verificação de assinatura sempre aceita; isto não muda nenhuma
// alocação nem o que é conferido aqui.
#include "alloc_survey.hpp"
#include "floor_test_support.hpp"
#include "oom_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <swarmstream/manifest_view.hpp>
#include <swarmstream/sha256.hpp>
#include <type_traits>
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

// ------------------------------------------------------ auditoria da REPETIÇÃO (sem falha)
// A expectativa da repetição sai do estado VALIDADO do alvo depois da 1a chamada e ANTES da
// repetição, não de may_have_changed (que só diz "antigo OU novo", sem dizer qual ficou):
//  - o alvo já tem EXATAMENTE os bytes do manifesto novo (a troca aconteceu antes da falha, ou a
//    cópia já era idêntica): nada a gravar => Ok, wrote=false, bytes preservados, pasta intacta;
//  - senão o alvo ainda precisa do que o cenário grava sem falha (`from_scratch`): Ok/wrote=true e
//    bytes novos, ou a recusa do cenário (downgrade: Rollback, nada muda).
struct AgainExpect
{
    det::FloorStatus status;
    bool wrote;
};

AgainExpect expected_again(bool target_is_new, const AgainExpect& from_scratch)
{
    if (target_is_new) return {det::FloorStatus::Ok, false};
    return from_scratch;
}

const char* status_name(det::FloorStatus s)
{
    switch (s) {
    case det::FloorStatus::Ok: return "Ok";
    case det::FloorStatus::Io: return "Io";
    case det::FloorStatus::Rollback: return "Rollback";
    case det::FloorStatus::OutOfMemory: return "OutOfMemory";
    }
    return "?";
}

// Estado ENCONTRADO do alvo, em texto, para a mensagem de falha (cenário e K vêm de quem chama).
std::string describe_target(const Disk& d, const Disk& pre, const Bytes& new_bytes)
{
    if (!d.target_exists) return "ausente";
    std::string s = std::to_string(d.target.size()) + " bytes, ";
    if (same_bytes(d.target, new_bytes))
        s += "IGUAIS aos do manifesto novo";
    else if (pre.target_exists && same_bytes(d.target, pre.target))
        s += "IGUAIS aos do manifesto antigo";
    else
        s += "diferentes do antigo e do novo";
    s += d.tmp_exists ? "; .tmp presente" : "; .tmp ausente";
    return s;
}

// "" se a repetição está de acordo com `e`; senão, o motivo. `before` é o estado logo antes da
// repetição, `fin` o logo depois.
std::string audit_again(const det::FloorWrite& again, const AgainExpect& e, bool target_is_new,
                        const Disk& pre, const Disk& before, const Disk& fin,
                        const Bytes& new_bytes)
{
    if (again.status != e.status)
        return std::string("status da repeticao ") + status_name(again.status) + ", esperado " +
               status_name(e.status);
    if (again.wrote != e.wrote)
        return std::string("wrote da repeticao ") + (again.wrote ? "true" : "false") +
               ", esperado " + (e.wrote ? "true" : "false");
    if (e.wrote || target_is_new) {
        if (!fin.target_exists || !same_bytes(fin.target, new_bytes))
            return "o alvo final nao tem os bytes do manifesto novo";
    }
    if (e.wrote && fin.tmp_exists) return "o .tmp que sobrou nao foi consumido";
    if (target_is_new && !e.wrote && fin.files != before.files)
        return "a repeticao sem o que gravar alterou a pasta";
    if (!target_is_new && !e.wrote) { // recusa (downgrade): o alvo do cenário não muda
        if (fin.target_exists != pre.target_exists ||
            (pre.target_exists && !same_bytes(fin.target, pre.target)))
            return "o alvo mudou numa repeticao que nao grava";
    }
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

    // --- expectativa e auditoria da repetição sem falha
    const AgainExpect update_ok{det::FloorStatus::Ok, true};
    const AgainExpect refused{det::FloorStatus::Rollback, false};
    SW_CHECK(expected_again(true, update_ok).status == det::FloorStatus::Ok);
    SW_CHECK(!expected_again(true, update_ok).wrote); // alvo ja novo: nada a gravar
    SW_CHECK(expected_again(false, update_ok).wrote); // ainda precisa atualizar
    SW_CHECK(expected_again(false, refused).status == det::FloorStatus::Rollback);
    SW_CHECK(!expected_again(false, refused).wrote);

    auto write_result = [](det::FloorStatus st, bool wrote) {
        det::FloorWrite w;
        w.status = st;
        w.wrote = wrote;
        return w;
    };
    // alvo ja tem os bytes novos: repetir => Ok, wrote=false, bytes e pasta preservados
    SW_CHECK(audit_again(write_result(det::FloorStatus::Ok, false), expected_again(true, update_ok),
                         true, pre, bom, bom, novo)
                 .empty());
    // MUTANTE: a repeticao diz que gravou quando o alvo ja era o novo (o defeito antigo do teste)
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, true), expected_again(true, update_ok),
                          true, pre, bom, bom, novo)
                  .empty());
    // MUTANTE: repeticao que nao grava mas altera a pasta
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, false),
                          expected_again(true, update_ok), true, pre, bom, com_tmp, novo)
                  .empty());
    // MUTANTE: repeticao que nao grava e deixa o alvo com bytes que nao sao os novos
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, false),
                          expected_again(true, update_ok), true, pre, bom, corrompido, novo)
                  .empty());
    // alvo ainda antigo: repetir => Ok, wrote=true, bytes novos, sem .tmp sobrando
    SW_CHECK(audit_again(write_result(det::FloorStatus::Ok, true), expected_again(false, update_ok),
                         false, pre9, pre9, bom, novo)
                 .empty());
    // MUTANTE: devolve wrote=false quando ainda precisava atualizar
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, false),
                          expected_again(false, update_ok), false, pre9, pre9, bom, novo)
                  .empty());
    // MUTANTE: wrote=true mas o alvo final nao tem os bytes novos
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, true),
                          expected_again(false, update_ok), false, pre9, pre9, pre9, novo)
                  .empty());
    // MUTANTE: wrote=true e o .tmp sobrou
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Ok, true),
                          expected_again(false, update_ok), false, pre9, pre9, com_tmp, novo)
                  .empty());
    // MUTANTE: status errado
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Io, false),
                          expected_again(true, update_ok), true, pre, bom, bom, novo)
                  .empty());
    // recusa (downgrade): Rollback, wrote=false, alvo antigo intacto
    SW_CHECK(audit_again(write_result(det::FloorStatus::Rollback, false),
                         expected_again(false, refused), false, pre9, pre9, pre9, novo)
                 .empty());
    // MUTANTE: a recusa mexeu no alvo
    SW_CHECK(!audit_again(write_result(det::FloorStatus::Rollback, false),
                          expected_again(false, refused), false, pre9, pre9, bom, novo)
                  .empty());
    // o texto do estado encontrado distingue ausente / novo / antigo / outro
    SW_CHECK(describe_target(pre, pre, novo) == "ausente");
    SW_CHECK(describe_target(bom, pre, novo).find("IGUAIS aos do manifesto novo") !=
             std::string::npos);
    SW_CHECK(describe_target(pre9, pre9, novo).find("IGUAIS aos do manifesto antigo") !=
             std::string::npos);
    SW_CHECK(describe_target(corrompido, pre9, novo).find("diferentes do antigo e do novo") !=
             std::string::npos);
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

// Impede o compilador de descartar um objeto que o teste construiu só para contar alocações.
std::atomic<const void*> g_sink{nullptr};
void escape(const void* p) noexcept { g_sink.store(p, std::memory_order_relaxed); }

// ------------------------------------------------------------------------------------ fixture
// Manifestos, chaves e cenários. Existe igual no processo pai e em cada filho (o filho reconstrói
// tudo do zero: mesma fixture, nenhum estado herdado do pai).
struct Fixture
{
    std::vector<sp::Ed25519PublicKey> keys;
    Bytes m5, m7, m9, m5_reissue;
    std::vector<ReadCase> reads;
    std::vector<CommitCase> commits;

    Fixture()
        : keys{public_key_of(kSeed)}, m5(make_manifest(kGame, 5, kSeed)),
          m7(make_manifest(kGame, 7, kSeed)), m9(make_manifest(kGame, 9, kSeed)),
          m5_reissue(make_manifest(kGame, 5, kSeed, "reassinado"))
    {
        SW_CHECK(!same_bytes(m5, m5_reissue));
        reads = {
            {"piso presente", [this](const fs::path& d) { install(d, m7, keys); }},
            {"primeira instalacao (sem installed/)", [](const fs::path&) {}},
            {"cache existente sem piso (index.bin presente)",
             [](const fs::path& d) { write_all(d / "index.bin", Bytes(4, std::byte{1})); }},
            {"installed e um arquivo comum",
             [](const fs::path& d) { write_all(d / "installed", Bytes(3, std::byte{2})); }},
        };
        commits = {
            {"primeira instalacao", [](const fs::path&) {}, m5, 5, det::FloorStatus::Ok, true},
            {"atualizacao 5 -> 7", [this](const fs::path& d) { install(d, m5, keys); }, m7, 7,
             det::FloorStatus::Ok, true},
            {"mesmo build_seq com bytes diferentes (reassinatura)",
             [this](const fs::path& d) { install(d, m5, keys); }, m5_reissue, 5,
             det::FloorStatus::Ok, true},
            {"copia identica (nada a gravar)", [this](const fs::path& d) { install(d, m5, keys); },
             m5, 5, det::FloorStatus::Ok, false},
            {"downgrade recusado (9 -> 7)", [this](const fs::path& d) { install(d, m9, keys); }, m7,
             7, det::FloorStatus::Rollback, false},
        };
        CommitCase after_swap{"erro de replace DEPOIS da troca (5 -> 7)",
                              [this](const fs::path& d) { install(d, m5, keys); },
                              m7,
                              7,
                              det::FloorStatus::Io,
                              false};
        after_swap.inject_replace_error_after_swap = true;
        commits.push_back(after_swap);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

// Saída que também vai para o resumo do job (GITHUB_STEP_SUMMARY) quando existe: o ctest só mostra
// a saída dos testes que FALHAM, e o mapa por cenário/K precisa estar visível mesmo com tudo verde.
std::string g_summary;

void note(const std::string& line)
{
    std::cout << line << std::endl;
    g_summary += line;
    g_summary += '\n';
}

void flush_summary(const char* title)
{
    if (g_summary.empty()) return;
#if defined(_WIN32)
    char path[4096];
    const DWORD n = ::GetEnvironmentVariableA("GITHUB_STEP_SUMMARY", path, sizeof(path));
    const bool has = n > 0 && n < sizeof(path);
#else
    const char* path = std::getenv("GITHUB_STEP_SUMMARY");
    const bool has = path != nullptr && *path != 0;
#endif
    if (!has) return;
    std::ofstream f(path, std::ios::app);
    if (!f) {
        std::cerr << "AVISO: GITHUB_STEP_SUMMARY existe mas nao abriu para escrita" << std::endl;
        return;
    }
    f << "\n### " << title << "\n\n```\n" << g_summary << "```\n";
    if (!f) std::cerr << "AVISO: falha ao escrever o resumo do job" << std::endl;
}

// ------------------------------------------------------------------ levantamento (sem falha)
std::map<std::string, std::size_t> g_survey_allocs; // alocações vistas por cenário (sem injeção)

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
    g_survey_allocs[c.name] = oom::survey::g_seen.load();
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
    g_survey_allocs[c.name] = oom::survey::g_seen.load();
    oom::survey::report(c.name);
    cleanup(dir);
}

// ------------------------------------------------------------- um ponto K da LEITURA
// `point(k)` arma a falha SÓ em torno da operação examinada; antes (fixture, leitura de base) e
// depois (conferências) o injetor está desarmado. Devolve se a falha DISPAROU; false = a operação
// terminou antes de K (e então só o resultado de base foi conferido). Usado igual pelo modo em
// processo e pelo processo-filho.
class ReadSweep
{
public:
    explicit ReadSweep(const ReadCase& c)
        : c_(c), keys_{public_key_of(kSeed)}, dir_(fresh_dir("oomread"))
    {
        c_.setup(dir_);
        base_ = det::read_installed_floor(dir_, kGame, keys_of(keys_));
        before_ = snapshot(dir_);
        oom::warm_up_exceptions();
        oom::g_progress_scenario.store(c_.name);
    }

    bool point(std::int64_t k)
    {
        const std::size_t handles_before = open_handles();
        oom::g_progress_k.store(k);
        arm(k);
        const det::FloorRead r = det::read_installed_floor(dir_, kGame, keys_of(keys_));
        const bool fired = g_fired.load();
        disarm();
        if (!fired) {
            SW_CHECK(same_read(r, base_));
            SW_CHECK(open_handles() == handles_before);
            return false;
        }
        if (r.status == det::FloorStatus::Ok)
            std::cerr << "LEITURA devolveu Ok apos falha de alocacao em K=" << k << " (" << c_.name
                      << ")" << std::endl;
        SW_CHECK(r.status != det::FloorStatus::Ok); // jamais "ausente" nem piso errado
        SW_CHECK(r.status == det::FloorStatus::OutOfMemory || r.status == det::FloorStatus::Io);
        if (open_handles() != handles_before)
            std::cerr << "VAZAMENTO (leitura) em K=" << k << " (" << c_.name << ")" << std::endl;
        SW_CHECK(open_handles() == handles_before);
        const Disk after = snapshot(dir_);
        SW_CHECK(after.files == before_.files); // leitura não altera a pasta
        SW_CHECK(same_read(det::read_installed_floor(dir_, kGame, keys_of(keys_)), base_));
        return true;
    }

    void finish() { cleanup(dir_); }
    [[nodiscard]] const char* name() const { return c_.name; }

private:
    const ReadCase& c_;
    std::vector<sp::Ed25519PublicKey> keys_;
    fs::path dir_;
    det::FloorRead base_;
    Disk before_;
};

// ------------------------------------------------------------- um ponto K da GRAVAÇÃO
// Uma gravação (com as operações reais ou com a falha de replace DEPOIS da troca).
det::FloorWrite run_commit(const CommitCase& c, const fs::path& dir,
                           const std::vector<sp::Ed25519PublicKey>& keys, FaultOps& ops)
{
    return det::commit_installed_floor(dir, c.new_manifest, keys_of(keys), ops);
}

class CommitSweep
{
public:
    explicit CommitSweep(const CommitCase& c)
        : c_(c), keys_{public_key_of(kSeed)}, dir_(fresh_dir("oomwrite"))
    {
        target_rel_ = rel_of(dir_, det::installed_floor_path(dir_, kGame));
        tmp_rel_ = rel_of(dir_, det::installed_tmp_path(dir_, kGame));
        oom::warm_up_exceptions();
        oom::g_progress_scenario.store(c_.name);
    }

    int again_target_new = 0; // repetições em que o alvo já tinha os bytes do manifesto novo
    bool last_again_target_new = false;

    bool point(std::int64_t k)
    {
        const CommitCase& c = c_;
        const fs::path& dir = dir_;
        // Estado inicial limpo a cada ponto.
        cleanup(dir);
        fs::create_directories(dir);
        c.setup(dir);
        const Disk pre = snapshot(dir);
        std::optional<std::uint64_t> pre_floor;
        {
            const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys_));
            SW_CHECK(r.status == det::FloorStatus::Ok);
            pre_floor = r.floor;
        }
        const std::size_t handles_before = open_handles();

        FaultOps ops;
        if (c.inject_replace_error_after_swap)
            ops.replace_fault = FaultOps::ReplaceFault::FailAfterSwap;
        oom::g_progress_k.store(k);
        arm(k);
        const det::FloorWrite w = run_commit(c, dir, keys_, ops);
        const bool fired = g_fired.load();
        disarm();

        const Disk post = snapshot(dir);
        if (!fired) {
            SW_CHECK(w.status == c.expect_status);
            SW_CHECK(w.wrote == c.expect_wrote);
            SW_CHECK(open_handles() == handles_before);
            return false;
        }

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
            const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(keys_));
            if (post.target_exists) SW_CHECK(r.status == det::FloorStatus::Ok);
            if (r.status == det::FloorStatus::Ok) in.post_floor = r.floor;
        }
        in.target_rel = target_rel_;
        in.tmp_rel = tmp_rel_;
        const std::string why = audit_after_failure(pre, post, in);
        if (!why.empty())
            std::cerr << "ESTADO INVALIDO apos falha em K=" << k << " (cenario '" << c.name
                      << "'): " << why << " (may_have_changed=" << w.may_have_changed
                      << "; alvo: " << describe_target(post, pre, c.new_manifest) << ")"
                      << std::endl;
        SW_CHECK(why.empty());

        // Uma NOVA gravação, SEM falha nenhuma (operações reais), chega ao estado final esperado.
        // A expectativa sai do estado do alvo JÁ VALIDADO acima (nome, bytes e piso) e lido de novo
        // aqui, imediatamente antes da repetição: se o alvo já tem exatamente os bytes do manifesto
        // novo, não há o que gravar (Ok, wrote=false); senão, o que o cenário grava sem falha.
        const Disk before_again = snapshot(dir);
        const bool target_is_new =
            before_again.target_exists && same_bytes(before_again.target, c.new_manifest);
        last_again_target_new = target_is_new;
        if (target_is_new) ++again_target_new;
        const AgainExpect from_scratch = c.inject_replace_error_after_swap
                                             ? AgainExpect{det::FloorStatus::Ok, true}
                                             : AgainExpect{c.expect_status, c.expect_wrote};
        const AgainExpect want = expected_again(target_is_new, from_scratch);
        FaultOps again_ops;
        const det::FloorWrite again = run_commit(c, dir, keys_, again_ops);
        const Disk fin = snapshot(dir);
        const std::string again_why =
            audit_again(again, want, target_is_new, pre, before_again, fin, c.new_manifest);
        if (!again_why.empty())
            std::cerr << "REPETICAO invalida apos falha em K=" << k << " (cenario '" << c.name
                      << "'): " << again_why << "; 1a chamada: status=" << status_name(w.status)
                      << " wrote=" << w.wrote << " may_have_changed=" << w.may_have_changed
                      << "; alvo antes da repeticao: "
                      << describe_target(before_again, pre, c.new_manifest)
                      << "; repeticao: status=" << status_name(again.status)
                      << " wrote=" << again.wrote
                      << " (esperado status=" << status_name(want.status) << " wrote=" << want.wrote
                      << "); alvo depois: " << describe_target(fin, pre, c.new_manifest)
                      << std::endl;
        SW_CHECK(again_why.empty());
        return true;
    }

    void finish() { cleanup(dir_); }
    [[nodiscard]] const char* name() const { return c_.name; }

private:
    const CommitCase& c_;
    std::vector<sp::Ed25519PublicKey> keys_;
    fs::path dir_;
    std::string target_rel_;
    std::string tmp_rel_;
};

// ------------------------------------------------------------ varredura EM PROCESSO (padrão)
void sweep_read(const ReadCase& c)
{
    std::cout << "[Test] varredura OOM, leitura do piso: " << c.name << std::endl;
    ReadSweep sweep(c);
    int failed_points = 0;
    int ok_at = 0;
    for (std::int64_t k = 1; k <= 2000; ++k) {
        if (!sweep.point(k)) {
            ok_at = static_cast<int>(k);
            break;
        }
        ++failed_points;
    }
    SW_CHECK(ok_at > 0);
    SW_CHECK(failed_points > 0);
    note(std::string("  [em processo] leitura '") + c.name +
         "': pontos de falha cobertos: " + std::to_string(failed_points) +
         " (leitura completa na " + std::to_string(ok_at) + "a alocacao)");
    sweep.finish();
}

void sweep_commit(const CommitCase& c)
{
    std::cout << "[Test] varredura OOM, gravacao do piso: " << c.name << std::endl;
    CommitSweep sweep(c);
    int failed_points = 0;
    int ok_at = 0;
    for (std::int64_t k = 1; k <= 2000; ++k) {
        if (!sweep.point(k)) {
            ok_at = static_cast<int>(k);
            break;
        }
        ++failed_points;
    }
    SW_CHECK(ok_at > 0);
    SW_CHECK(failed_points > 0);
    note(std::string("  [em processo] gravacao '") + c.name + "': pontos de falha cobertos: " +
         std::to_string(failed_points) + " (gravacao completa na " + std::to_string(ok_at) +
         "a alocacao); repeticoes com o alvo ja novo: " + std::to_string(sweep.again_target_new) +
         ", repeticoes que ainda atualizam ou recusam: " +
         std::to_string(failed_points - sweep.again_target_new));
    sweep.finish();
}

// ------------------------------------------------------------------ PROCESSO-FILHO (um ponto K)
// `--ponto <leitura|gravacao> <indice> <K>`: reconstrói a fixture, faz o setup, arma a falha em K
// só em torno da operação, confere o mesmo que a varredura em processo e sai com um código que o
// pai entende (oom_support.hpp). Linha "PONTO|..." diz exatamente o que foi exercitado; em caso de
// disparo, "FRAMES|..." traz a pilha do ponto da injeção (RVAs; o pai resolve os símbolos).
int run_child(const Fixture& fx, int argc, char** argv)
{
    if (argc != 5) return oom::kExitUsage;
    const std::string kind = argv[2];
    const std::size_t index = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
    const std::int64_t k = std::strtoll(argv[4], nullptr, 10);
    if (k < 1) return oom::kExitUsage;
    oom::g_child_mode.store(true);
    bool fired = false;
    const char* name = "?";
    int target_new = 0;
    if (kind == "leitura") {
        if (index >= fx.reads.size()) return oom::kExitUsage;
        ReadSweep sweep(fx.reads[index]);
        name = sweep.name();
        fired = sweep.point(k);
        sweep.finish();
    } else if (kind == "gravacao") {
        if (index >= fx.commits.size()) return oom::kExitUsage;
        CommitSweep sweep(fx.commits[index]);
        name = sweep.name();
        fired = sweep.point(k);
        target_new = fired && sweep.last_again_target_new ? 1 : 0;
        sweep.finish();
    } else {
        return oom::kExitUsage;
    }
    std::printf("PONTO|%s|%zu|%s|K=%lld|disparou=%d|alvo_novo=%d|ok\n", kind.c_str(), index, name,
                static_cast<long long>(k), fired ? 1 : 0, target_new);
    if (fired) oom::print_fire_frames_rva();
    std::fflush(nullptr);
    return fired ? oom::kExitFiredOk : oom::kExitNotFiredOk;
}

// Modos de AUTOTESTE do filho (`--ponto-teste <modo>`), só para o pai provar que sabe distinguir
// cada resultado (controles positivos da classificação). Não fazem parte da varredura.
int run_child_selftest(const std::string& mode)
{
    oom::g_child_mode.store(true);
    if (mode == "terminate-injetado") {
        // Uma exceção de alocação ESCAPA de uma função noexcept: std::terminate no ponto da
        // injeção.
        oom::warm_up_exceptions();
        oom::g_progress_scenario.store("autoteste");
        oom::g_progress_k.store(1);
        oom::arm(1);
        []() noexcept {
            auto* p = new char[100];
            escape(p);
            delete[] p;
        }();
        return 0; // inalcançável
    }
    if (mode == "assercao") {
        SW_CHECK(mode == "nunca");
        return 0;
    }
    if (mode == "travar") {
#if defined(_WIN32)
        for (;;)
            ::Sleep(1000);
#else
        return oom::kExitUsage;
#endif
    }
    if (mode == "saida7") return 7;
    if (mode == "sem-ponto") return 0;
    if (mode == "ponto-errado") { // diz que exercitou K=999, o pai pediu K=1
        std::printf("PONTO|leitura|0|x|K=999|disparou=1|alvo_novo=0|ok\nFRAMES|1\n");
        std::fflush(nullptr);
        return oom::kExitFiredOk;
    }
    if (mode == "disparo-contraditorio") { // diz que disparou mas sai como "terminou antes de K"
        std::printf("PONTO|leitura|0|x|K=1|disparou=1|alvo_novo=0|ok\nFRAMES|1\n");
        std::fflush(nullptr);
        return oom::kExitNotFiredOk;
    }
    if (mode == "disparo-sem-pilha") { // sai 0 e diz que disparou, mas sem a pilha da injeção
        std::printf("PONTO|leitura|0|x|K=1|disparou=1|alvo_novo=0|ok\n");
        std::fflush(nullptr);
        return oom::kExitFiredOk;
    }
    return oom::kExitUsage;
}

// ------------------------------------------------------------------ PAI: um filho por K (Windows)
#if defined(_WIN32)
namespace children {

enum class Outcome {
    FiredOk,        // exit 0, falha disparou em K, conferências passaram
    NotFiredOk,     // exit 10, a operação terminou antes de K (NÃO conta como cobertura)
    Terminate,      // exit 20, std::terminate
    CheckFailed,    // exit 1, SW_CHECK reprovou
    UnexpectedExit, // qualquer outro código (abort, crash, uso inválido)
    Timeout,        // o pai matou o filho
    StartFailed,    // não foi possível criar o filho
    Inconsistent,   // código e linha PONTO não concordam (ponto/ K/ disparo)
};

const char* outcome_name(Outcome o)
{
    switch (o) {
    case Outcome::FiredOk: return "OK";
    case Outcome::NotFiredOk: return "FIM";
    case Outcome::Terminate: return "TERMINATE";
    case Outcome::CheckFailed: return "ASSERCAO";
    case Outcome::UnexpectedExit: return "SAIDA-INESPERADA";
    case Outcome::Timeout: return "TIMEOUT";
    case Outcome::StartFailed: return "FALHA-AO-INICIAR";
    case Outcome::Inconsistent: return "INCONSISTENTE";
    }
    return "?";
}

struct Point
{
    std::int64_t k = 0;
    Outcome outcome = Outcome::StartFailed;
    unsigned long exit_code = 0;
    double seconds = 0;
    bool target_new = false;
    std::size_t frame_count = 0; // quadros da pilha da injeção que o filho registrou
    std::string frames;          // pilha do ponto da injeção, resolvida pelo pai
    std::string log_tail;        // últimas linhas do log do filho (só quando não é OK/FIM)
};

struct ScenarioResult
{
    std::string kind;
    std::string name;
    std::size_t survey_allocs = 0;
    std::vector<Point> points;
    int fired_ok = 0;
    int bad = 0;
    int again_target_new = 0;
    std::int64_t end_k = 0; // K em que a operação terminou sem disparar (0 = não chegou)
    double seconds = 0;
    [[nodiscard]] bool ok() const { return bad == 0 && end_k > 0 && fired_ok > 0; }
};

struct Spawn
{
    bool started = false;
    bool timed_out = false;
    unsigned long exit_code = 0;
    double seconds = 0;
    std::string error;
    std::string log;
};

std::wstring exe_path()
{
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    SW_CHECK(n > 0 && n < buf.size());
    buf.resize(n);
    return buf;
}

std::string win_error(const char* what)
{
    return std::string(what) + " (erro do Windows " + std::to_string(::GetLastError()) + ")";
}

Spawn spawn_child(const std::wstring& exe, const std::wstring& args, const fs::path& log_path,
                  DWORD timeout_ms)
{
    Spawn r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hlog = ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hlog == INVALID_HANDLE_VALUE) {
        r.error = win_error("nao criou o log do filho");
        return r;
    }
    HANDLE hnul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hnul == INVALID_HANDLE_VALUE ? nullptr : hnul;
    si.hStdOutput = hlog;
    si.hStdError = hlog;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    const auto t0 = std::chrono::steady_clock::now();
    const BOOL created = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!created) r.error = win_error("CreateProcess falhou");
    // As cópias do pai; o filho tem as dele.
    (void)::CloseHandle(hlog);
    if (hnul != INVALID_HANDLE_VALUE) (void)::CloseHandle(hnul);
    if (!created) return r;
    r.started = true;
    const DWORD w = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        r.timed_out = true;
        (void)::TerminateProcess(pi.hProcess, 1);
        (void)::WaitForSingleObject(pi.hProcess, 10000);
    } else if (w != WAIT_OBJECT_0) {
        r.error = win_error("espera pelo filho falhou");
        (void)::TerminateProcess(pi.hProcess, 1);
        (void)::WaitForSingleObject(pi.hProcess, 10000);
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(pi.hProcess, &code)) r.error = win_error("GetExitCodeProcess falhou");
    r.exit_code = code;
    (void)::CloseHandle(pi.hThread);
    (void)::CloseHandle(pi.hProcess);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const Bytes raw = read_all(log_path);
    r.log.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return r;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Última linha que começa com `prefix` (sem o prefixo e sem o \r); "" se não houver.
bool find_line(const std::string& log, const char* prefix, std::string& out)
{
    bool found = false;
    std::size_t pos = 0;
    const std::size_t plen = std::strlen(prefix);
    while (pos < log.size()) {
        std::size_t end = log.find('\n', pos);
        if (end == std::string::npos) end = log.size();
        std::string line = log.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, plen, prefix) == 0) {
            out = line.substr(plen);
            found = true;
        }
        pos = end + 1;
    }
    return found;
}

std::string tail_lines(const std::string& log, std::size_t n)
{
    std::vector<std::string> lines = split(log, '\n');
    while (!lines.empty() && (lines.back().empty() || lines.back() == "\r"))
        lines.pop_back();
    std::string out;
    const std::size_t from = lines.size() > n ? lines.size() - n : 0;
    for (std::size_t i = from; i < lines.size(); ++i)
        out += "      | " + lines[i] + "\n";
    return out;
}

std::vector<std::uint64_t> parse_rvas(const std::string& csv)
{
    std::vector<std::uint64_t> out;
    for (const std::string& part : split(csv, ',')) {
        if (part.empty()) continue;
        out.push_back(std::strtoull(part.c_str(), nullptr, 16));
    }
    return out;
}

// Classifica o que o filho fez a partir do código de saída E da linha PONTO que ele imprimiu: o
// código sozinho não basta (um filho que sai com 0 sem ter exercitado o ponto K seria "cobertura").
Point classify(const Spawn& s, const char* kind, std::size_t index, const std::string& name,
               std::int64_t k, oom::survey::SymbolSession& syms)
{
    Point p;
    p.k = k;
    p.seconds = s.seconds;
    p.exit_code = s.exit_code;
    if (!s.started) {
        p.outcome = Outcome::StartFailed;
        p.log_tail = "      | " + s.error + "\n";
        return p;
    }
    if (s.timed_out) {
        p.outcome = Outcome::Timeout;
        p.log_tail = tail_lines(s.log, 20);
        return p;
    }
    if (!s.error.empty()) {
        p.outcome = Outcome::UnexpectedExit;
        p.log_tail = "      | " + s.error + "\n" + tail_lines(s.log, 20);
        return p;
    }
    // Linha PONTO: o que o filho diz que exercitou.
    bool has_ponto = false;
    bool ponto_ok = false;
    bool ponto_fired = false;
    std::string line;
    if (find_line(s.log, "PONTO|", line)) {
        has_ponto = true;
        const std::vector<std::string> f = split(line, '|'); // kind|index|name|K=..|disparou=..|..
        ponto_ok = f.size() >= 7 && f[0] == kind && f[1] == std::to_string(index) && f[2] == name &&
                   f[3] == "K=" + std::to_string(k) && f.back() == "ok";
        ponto_fired = f.size() >= 5 && f[4] == "disparou=1";
        p.target_new = f.size() >= 6 && f[5] == "alvo_novo=1";
    }
    std::string frames_csv;
    if (find_line(s.log, "FRAMES|", frames_csv)) {
        const std::vector<std::uint64_t> rvas = parse_rvas(frames_csv);
        p.frame_count = rvas.size();
        p.frames = syms.describe(rvas);
    }

    switch (s.exit_code) {
    case static_cast<unsigned long>(oom::kExitFiredOk):
        // Disparou: a pilha da injeção TEM de ter sido registrada (prova de que o ponto K foi
        // alcançado e a falha foi injetada ali).
        p.outcome = has_ponto && ponto_ok && ponto_fired && p.frame_count > 0
                        ? Outcome::FiredOk
                        : Outcome::Inconsistent;
        break;
    case static_cast<unsigned long>(oom::kExitNotFiredOk):
        p.outcome =
            has_ponto && ponto_ok && !ponto_fired ? Outcome::NotFiredOk : Outcome::Inconsistent;
        break;
    case static_cast<unsigned long>(oom::kExitTerminate): p.outcome = Outcome::Terminate; break;
    case 1UL: p.outcome = Outcome::CheckFailed; break;
    default: p.outcome = Outcome::UnexpectedExit; break;
    }
    if (p.outcome != Outcome::FiredOk && p.outcome != Outcome::NotFiredOk)
        p.log_tail = tail_lines(s.log, 20);
    return p;
}

Point run_point(const std::wstring& exe, const char* kind, std::size_t index,
                const std::string& name, std::int64_t k, const fs::path& log_path,
                oom::survey::SymbolSession& syms)
{
    const std::wstring args = L"--ponto " + std::wstring(kind, kind + std::strlen(kind)) + L" " +
                              std::to_wstring(index) + L" " + std::to_wstring(k);
    return classify(spawn_child(exe, args, log_path, 180000), kind, index, name, k, syms);
}

ScenarioResult run_scenario(const std::wstring& exe, const char* kind, std::size_t index,
                            const std::string& name, std::size_t survey_allocs,
                            const fs::path& log_dir, oom::survey::SymbolSession& syms, int& serial)
{
    ScenarioResult res;
    res.kind = kind;
    res.name = name;
    res.survey_allocs = survey_allocs;
    // O levantamento (sem injeção) diz quantas alocações o cenário faz; o limite dá folga para as
    // diferenças entre a passada do levantamento e a da varredura. Não achar o fim dentro do limite
    // é resultado INCOMPLETO (nunca "concluído").
    const std::int64_t cap = static_cast<std::int64_t>(survey_allocs) + 60;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::int64_t k = 1; k <= cap; ++k) {
        const fs::path log = log_dir / ("filho_" + std::to_string(serial++) + ".log");
        Point p = run_point(exe, kind, index, name, k, log, syms);
        std::error_code ec;
        fs::remove(log, ec); // o que importa já foi lido (tail e frames estão no Point)
        const Outcome o = p.outcome;
        if (p.target_new) ++res.again_target_new;
        res.points.push_back(std::move(p));
        if (o == Outcome::FiredOk) {
            ++res.fired_ok;
            continue;
        }
        if (o == Outcome::NotFiredOk) {
            res.end_k = k;
            break;
        }
        ++res.bad;
    }
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

void print_scenario(const ScenarioResult& r)
{
    note("[Filhos] " + r.kind + " '" + r.name +
         "': alocacoes no levantamento=" + std::to_string(r.survey_allocs));
    for (const Point& p : r.points) {
        char head[96];
        std::snprintf(head, sizeof(head), "  K=%3lld %-16s %6.2fs", static_cast<long long>(p.k),
                      outcome_name(p.outcome), p.seconds);
        std::string line = head;
        if (p.outcome == Outcome::NotFiredOk)
            line += "  (a operacao terminou antes de K: fim do cenario; nao conta como cobertura)";
        else if (!p.frames.empty())
            line += "  " + p.frames;
        note(line);
        if (!p.log_tail.empty()) {
            note("    saida " + std::to_string(p.exit_code) + ", ultimas linhas do filho:");
            note(p.log_tail);
        }
    }
    char tail[256];
    std::snprintf(tail, sizeof(tail),
                  "  => disparos conferidos=%d, falhas=%d, fim do cenario em K=%lld, %.1fs",
                  r.fired_ok, r.bad, static_cast<long long>(r.end_k), r.seconds);
    note(tail);
    if (r.kind == "gravacao")
        note("     repeticoes com o alvo ja novo: " + std::to_string(r.again_target_new));
    if (!r.ok())
        note(std::string("  => CENARIO REPROVADO OU INCOMPLETO: ") +
             (r.end_k == 0 ? "nao chegou ao fim do cenario (K acima do limite); " : "") +
             (r.bad > 0 ? std::to_string(r.bad) + " ponto(s) com falha; " : "") +
             (r.fired_ok == 0 ? "nenhum disparo conferido; " : ""));
}

std::wstring widen(const char* text) { return std::wstring(text, text + std::strlen(text)); }

// Controles positivos do pai: cada resultado possível do filho é provocado de propósito e tem de
// ser classificado como tal. Sem isso, "nenhum terminate/asserção/timeout" não provaria nada.
void test_child_classification(const std::wstring& exe, const Fixture& fx,
                               oom::survey::SymbolSession& syms)
{
    std::cout << "[Test] pai: classificacao dos resultados do filho (controles positivos)"
              << std::endl;
    const fs::path dir = fresh_dir("filhoteste");
    int serial = 0;
    auto log = [&]() { return dir / ("t" + std::to_string(serial++) + ".log"); };
    auto selftest = [&](const char* mode, DWORD timeout_ms) {
        return classify(spawn_child(exe, L"--ponto-teste " + widen(mode), log(), timeout_ms),
                        "leitura", 0, "x", 1, syms);
    };
    auto expect = [&](const char* what, const Point& got, Outcome want) {
        std::cout << "  " << what << " -> " << outcome_name(got.outcome) << " (saida "
                  << got.exit_code << ")" << std::endl;
        SW_CHECK(got.outcome == want);
    };
    const std::string name0 = fx.reads[0].name;
    const Point fired = run_point(exe, "leitura", 0, name0, 1, log(), syms);
    expect("filho real, K=1 (a falha dispara)", fired, Outcome::FiredOk);
    SW_CHECK(fired.frame_count > 0); // a pilha do ponto da injeção veio junto
    const Point beyond = run_point(exe, "leitura", 0, name0, 1000, log(), syms);
    expect("filho real, K=1000 (a operacao termina antes de K)", beyond, Outcome::NotFiredOk);
    const Point term = selftest("terminate-injetado", 60000);
    expect("std::terminate fora de funcao noexcept", term, Outcome::Terminate);
    SW_CHECK(term.frame_count > 0);
    expect("SW_CHECK reprovado", selftest("assercao", 60000), Outcome::CheckFailed);
    expect("filho que trava (timeout de 3s)", selftest("travar", 3000), Outcome::Timeout);
    expect("saida inesperada (7)", selftest("saida7", 60000), Outcome::UnexpectedExit);
    expect("saida 0 sem a linha PONTO", selftest("sem-ponto", 60000), Outcome::Inconsistent);
    expect("PONTO de outro K", selftest("ponto-errado", 60000), Outcome::Inconsistent);
    expect("disparo contraditorio com o codigo de saida", selftest("disparo-contraditorio", 60000),
           Outcome::Inconsistent);
    expect("disparou mas sem a pilha da injecao", selftest("disparo-sem-pilha", 60000),
           Outcome::Inconsistent);
    expect("falha ao iniciar o filho",
           classify(spawn_child(L"C:\\swarmstream_nao_existe\\filho.exe", L"--ponto-teste x", log(),
                                10000),
                    "leitura", 0, "x", 1, syms),
           Outcome::StartFailed);
    // O veredito do cenario: qualquer ponto ruim, cenario que nao chegou ao fim ou sem disparo
    // conferido reprova.
    ScenarioResult good;
    good.fired_ok = 2;
    good.end_k = 3;
    SW_CHECK(good.ok());
    ScenarioResult bad_point = good;
    bad_point.bad = 1;
    SW_CHECK(!bad_point.ok());
    ScenarioResult no_end = good;
    no_end.end_k = 0;
    SW_CHECK(!no_end.ok());
    ScenarioResult no_fire = good;
    no_fire.fired_ok = 0;
    SW_CHECK(!no_fire.ok());
    cleanup(dir);
}

// Percorre TODOS os K de TODOS os cenários, um processo por ponto. Devolve false se qualquer
// ponto falhou ou ficou incompleto (mas só depois de mapear tudo).
bool sweep_with_children(const Fixture& fx)
{
    const std::wstring exe = exe_path();
    const fs::path log_dir = fresh_dir("filhos");
    int serial = 0;
    const auto t0 = std::chrono::steady_clock::now();
    note(
        "[Filhos] modo processo-filho por K (cada ponto: processo novo, fixture nova, falha armada "
        "so em torno da operacao)");
    oom::survey::SymbolSession syms; // um DbgHelp para todas as pilhas
    test_child_classification(exe, fx, syms);
    std::vector<ScenarioResult> all;
    for (std::size_t i = 0; i < fx.reads.size(); ++i) {
        const std::string name = fx.reads[i].name;
        all.push_back(
            run_scenario(exe, "leitura", i, name, g_survey_allocs[name], log_dir, syms, serial));
        print_scenario(all.back());
    }
    for (std::size_t i = 0; i < fx.commits.size(); ++i) {
        const std::string name = fx.commits[i].name;
        all.push_back(
            run_scenario(exe, "gravacao", i, name, g_survey_allocs[name], log_dir, syms, serial));
        print_scenario(all.back());
    }
    const double total =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    bool all_ok = true;
    int children = 0;
    int fired = 0;
    int bad = 0;
    for (const ScenarioResult& r : all) {
        all_ok = all_ok && r.ok();
        children += static_cast<int>(r.points.size());
        fired += r.fired_ok;
        bad += r.bad;
    }
    char summary[320];
    std::snprintf(summary, sizeof(summary),
                  "[Filhos] RESUMO: %zu cenarios, %d processos-filho, %d disparos conferidos, %d "
                  "pontos com falha, tempo total %.1fs (limite do ctest: 600s)",
                  all.size(), children, fired, bad, total);
    note(summary);
    note(all_ok ? "[Filhos] TODOS OS PONTOS PASSARAM (cada cenario chegou ao fim)"
                : "[Filhos] RESULTADO: REPROVADO OU INCOMPLETO (ver os pontos acima)");
    std::error_code ec;
    fs::remove_all(log_dir, ec);
    return all_ok;
}

} // namespace children
#endif

// ------------------------------------------------------------- testes das propriedades do piso

// O tipo ANTIGO (std::string inline): só como controle positivo do mecanismo.
struct OldFloorRead
{
    det::FloorStatus status = det::FloorStatus::Ok;
    det::FloorCategory category = det::FloorCategory::None;
    std::optional<std::uint64_t> floor;
    bool existing_cache_without_floor = false;
    std::string detail;
};

det::FloorRead two_named_returns_read(bool first)
{
    det::FloorRead a;
    det::FloorRead b;
    b.status = det::FloorStatus::Io;
    if (first) return a; // dois objetos nomeados: sem elisão garantida => movimento
    return b;
}

det::FloorWrite two_named_returns_write(bool first)
{
    det::FloorWrite a;
    det::FloorWrite b;
    b.status = det::FloorStatus::Io;
    if (first) return a;
    return b;
}

static_assert(std::is_nothrow_default_constructible_v<det::FloorRead>);
static_assert(std::is_nothrow_copy_constructible_v<det::FloorRead>);
static_assert(std::is_nothrow_move_constructible_v<det::FloorRead>);
static_assert(std::is_nothrow_move_assignable_v<det::FloorRead>);
static_assert(std::is_nothrow_default_constructible_v<det::FloorWrite>);
static_assert(std::is_nothrow_copy_constructible_v<det::FloorWrite>);
static_assert(std::is_nothrow_move_constructible_v<det::FloorWrite>);
static_assert(std::is_nothrow_move_assignable_v<det::FloorWrite>);

template <class T>
void print_traits(const char* name)
{
    std::cout << "    " << name << ": default_nothrow="
              << std::is_nothrow_default_constructible_v<T> << " copy_nothrow="
              << std::is_nothrow_copy_constructible_v<T> << " move_nothrow="
              << std::is_nothrow_move_constructible_v<T> << std::endl;
}

// T1 + T2.
void test_floor_types_do_not_allocate()
{
    std::cout << "[Test] FloorRead/FloorWrite: construir, copiar, mover e devolver NAO aloca"
              << std::endl;
    // Preparado FORA da contagem.
    const det::FloorDetail text = std::make_shared<const std::string>("texto de diagnostico");

    // Controle do contador: uma alocação de verdade é vista.
    {
        const std::uint64_t n0 = oom::g_total.load();
        auto* p = new int(1);
        escape(p);
        SW_CHECK(oom::g_total.load() > n0);
        delete p;
    }
    // Controle positivo do MECANISMO: o tipo antigo (std::string inline) aloca ao ser construído no
    // MSVC em Debug (e só lá). Fora dele o número é só informado.
    {
        const std::uint64_t n0 = oom::g_total.load();
        {
            OldFloorRead old;
            escape(&old);
        }
        const std::uint64_t n1 = oom::g_total.load();
        std::cout << "  controle: construir o tipo ANTIGO (std::string inline) alocou " << n1 - n0
                  << " vez(es)" << std::endl;
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
        SW_CHECK(n1 > n0);
#endif
    }

    const std::uint64_t n0 = oom::g_total.load();
    {
        det::FloorRead a;
        escape(&a);
        a.detail = text;
        det::FloorRead b(a);
        escape(&b);
        det::FloorRead c(std::move(b));
        escape(&c);
        b = c;
        c = std::move(b);
        det::FloorRead d = two_named_returns_read(true);
        det::FloorRead e = two_named_returns_read(false);
        escape(&d);
        escape(&e);
        d = e;
        e = std::move(d);
    }
    {
        det::FloorWrite a;
        escape(&a);
        a.detail = text;
        a.may_have_changed = true;
        det::FloorWrite b(a);
        escape(&b);
        det::FloorWrite c(std::move(b));
        escape(&c);
        b = c;
        c = std::move(b);
        det::FloorWrite d = two_named_returns_write(true);
        det::FloorWrite e = two_named_returns_write(false);
        escape(&d);
        escape(&e);
        d = e;
        e = std::move(d);
    }
    const std::uint64_t n1 = oom::g_total.load();
    std::cout << "  FloorRead/FloorWrite (padrao, copia, movimento, retornos nomeados): " << n1 - n0
              << " alocacao(oes)" << std::endl;
    SW_CHECK(n1 == n0);
    // T2: propriedades impressas. is_nothrow_* diz que o construtor é `noexcept`; NAO diz que ele
    // nao aloca nem que e seguro (no MSVC Debug, std::string() e noexcept E aloca). A prova de cada
    // ponto e a operacao executada + a fronteira noexcept + a evidencia (contagem e varredura).
    std::cout << "  propriedades (informativas; NAO provam seguranca nem causa):" << std::endl;
    print_traits<det::FloorRead>("FloorRead");
    print_traits<det::FloorWrite>("FloorWrite");
    print_traits<OldFloorRead>("OldFloorRead(std::string)");
    print_traits<std::string>("std::string");
    print_traits<Bytes>("std::vector<std::byte>");
    print_traits<fs::path>("std::filesystem::path");
    print_traits<sp::ChunkerInfo>("pack::ChunkerInfo");
    print_traits<sp::ManifestView>("pack::ManifestView");
}

// T3 + T4: os nomes dos arquivos do piso são byte a byte os de antes (hex local = pack::to_hex).
std::string old_leaf_hex(const std::string& id)
{
    const auto* p = reinterpret_cast<const std::byte*>(id.data());
    return sp::to_hex(sp::sha256(std::span<const std::byte>(p, id.size())));
}

void test_names_equal_old_expression()
{
    std::cout << "[Test] nomes dos arquivos do piso: iguais a expressao antiga; hex local = to_hex"
              << std::endl;
    std::vector<fs::path> dirs = {fs::path("cache"),
                                  fs::path("cache/"),
                                  fs::path("a/b/c"),
                                  fs::path("."),
                                  fs::path(""),
                                  fs::temp_directory_path(),
                                  fs::temp_directory_path() / "swarmstream_nomes"};
#if defined(_WIN32)
    dirs.push_back(fs::path("C:\\cache\\dir"));
    dirs.push_back(fs::path("C:\\cache\\dir\\"));
    dirs.push_back(fs::path("\\\\servidor\\share\\cache"));
#else
    dirs.push_back(fs::path("/abs/dir"));
    dirs.push_back(fs::path("/abs/dir/"));
#endif
    const std::vector<std::string> ids = {"jogo", "jogo-oom", "", "x/y",
                                          "a\xC3\xA7\xC3\xA3o-\xE6\x97\xA5\xE6\x9C\xAC"};
    int compared = 0;
    for (const std::string& id : ids) {
        const std::string hex = old_leaf_hex(id);
        SW_CHECK(det::installed_hash_hex(id) == hex); // T4
        for (const fs::path& dir : dirs) {
            const fs::path old_target = dir / "installed" / (hex + ".manifest");
            const fs::path old_tmp = dir / "installed" / (hex + ".manifest.tmp");
            SW_CHECK(det::installed_floor_path(dir, id).native() == old_target.native());
            SW_CHECK(det::installed_tmp_path(dir, id).native() == old_tmp.native());
            compared += 2;
        }
    }
    std::cout << "  " << compared << " caminhos comparados, " << ids.size() << " game_id, "
              << dirs.size() << " pastas" << std::endl;
}

// T5: pasta de cache com caracteres fora do ASCII: o diagnóstico traz o caminho em UTF-8.
void test_diagnostic_with_non_ascii_dir(const Fixture& fx)
{
    std::cout << "[Test] diagnostico com pasta de cache nao ASCII" << std::endl;
    const fs::path base = fresh_dir("utf8");
    const fs::path dir = base / fs::path(u8"cache_a\u00E7\u00E3o_\u65E5\u672C");
    std::error_code ec;
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    write_all(dir / "installed", Bytes(3, std::byte{2})); // "installed" e um arquivo comum
    const det::FloorRead r = det::read_installed_floor(dir, kGame, keys_of(fx.keys));
    SW_CHECK(r.status == det::FloorStatus::Io);
    SW_CHECK(r.category == det::FloorCategory::Read);
    const std::u8string want8 = (dir / "installed").u8string();
    const std::string want(want8.begin(), want8.end());
    std::cout << "  diagnostico: " << r.text() << std::endl;
    SW_CHECK(r.text().find(want) != std::string_view::npos);
    cleanup(base);
}

// T7: nenhuma alocação entre a TROCA do arquivo e o retorno de commit_installed_floor.
class SwapProbeOps final : public det::FloorFileOps
{
public:
    bool allocate_after_swap = false; // controle positivo do próprio probe
    bool swapped = false;
    std::uint64_t total_after_swap = 0;

    sw::CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        return det::real_floor_file_ops().create_directory(dir);
    }
    sw::CacheErrorCode write_file_durable(const fs::path& file,
                                          std::span<const std::byte> data) noexcept override
    {
        return det::real_floor_file_ops().write_file_durable(file, data);
    }
    det::FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                                  Bytes& out) noexcept override
    {
        return det::real_floor_file_ops().read_file(file, max_bytes, out);
    }
    sw::CacheErrorCode replace(const fs::path& from, const fs::path& to) noexcept override
    {
        const sw::CacheErrorCode rc = det::real_floor_file_ops().replace(from, to);
        if (rc == sw::CacheErrorCode::Success) {
            swapped = true;
            total_after_swap = oom::g_total.load();
            if (allocate_after_swap) {
                auto* p = new int(7);
                escape(p);
                delete p;
            }
        }
        return rc;
    }
};

void test_no_allocation_after_swap(const Fixture& fx)
{
    std::cout << "[Test] nenhuma alocacao entre a troca do arquivo e o retorno da gravacao"
              << std::endl;
    for (int variant = 0; variant < 3; ++variant) {
        const fs::path dir = fresh_dir("aftersw");
        if (variant >= 1) install(dir, fx.m5, fx.keys);
        const Bytes& manifest = variant == 0 ? fx.m5 : fx.m7;
        SwapProbeOps ops;
        ops.allocate_after_swap = variant == 2; // controle positivo: o probe VE uma alocação
        const det::FloorWrite w = det::commit_installed_floor(dir, manifest, keys_of(fx.keys), ops);
        const std::uint64_t total_after_return = oom::g_total.load();
        SW_CHECK(w.status == det::FloorStatus::Ok && w.wrote && ops.swapped);
        const std::uint64_t between = total_after_return - ops.total_after_swap;
        std::cout << "  variante " << variant << (variant == 2 ? " (controle positivo)" : "")
                  << ": alocacoes entre a troca e o retorno = " << between << std::endl;
        if (variant == 2)
            SW_CHECK(between > 0);
        else
            SW_CHECK(between == 0);
        cleanup(dir);
    }
}

} // namespace

int main(int argc, char** argv)
{
    oom::make_abort_fail_instead_of_hang();
    const Fixture fx;

    // Filho: um ponto K (ver run_child). Não roda mais nada.
    if (argc >= 2 && std::strcmp(argv[1], "--ponto") == 0) return run_child(fx, argc, argv);
    if (argc == 3 && std::strcmp(argv[1], "--ponto-teste") == 0) return run_child_selftest(argv[2]);

    // Modo da varredura: por padrão, UM PROCESSO POR K onde a STL tem proxy de iteradores
    // (MSVC Debug), porque lá o primeiro std::terminate mata o processo e a varredura em processo
    // só revelaria um ponto por execução. Nos demais (Linux, Release) a varredura é em processo.
    bool use_children = false;
#if defined(_WIN32) && defined(_ITERATOR_DEBUG_LEVEL)
    use_children = _ITERATOR_DEBUG_LEVEL != 0;
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filhos") == 0) {
            use_children = true;
        } else if (std::strcmp(argv[i], "--em-processo") == 0) {
            use_children = false;
        } else {
            std::cerr << "argumento desconhecido: " << argv[i] << std::endl;
            return oom::kExitUsage;
        }
    }
#if !defined(_WIN32)
    if (use_children) {
        std::cerr << "--filhos so existe no Windows" << std::endl;
        return oom::kExitUsage;
    }
#endif

    test_audit_self_check();
    test_floor_types_do_not_allocate();
    test_names_equal_old_expression();
    test_diagnostic_with_non_ascii_dir(fx);
    test_no_allocation_after_swap(fx);

    // 1) Levantamento dos caminhos executados: passada SEPARADA, SEM injecao de falha. Só informa.
    std::cout << "[Levantamento] passada sem injecao de falha; informa os caminhos executados, "
                 "nao e lista completa nem evidencia de ausencia de alocacao"
              << std::endl;
    for (const ReadCase& c : fx.reads)
        survey_read(c);
    for (const CommitCase& c : fx.commits)
        survey_commit(c);

    // 2) Varredura de falhas (sem exclusoes).
    if (use_children) {
#if defined(_WIN32)
        const bool ok = children::sweep_with_children(fx);
        flush_summary("F1-69 sw-core-floor-oom: varredura por processo-filho");
        if (!ok) return 1;
#endif
    } else {
        note("[Varredura] em processo (uma execucao, todos os K de todos os cenarios)");
        for (const ReadCase& c : fx.reads)
            sweep_read(c);
        for (const CommitCase& c : fx.commits)
            sweep_commit(c);
        flush_summary("F1-69 sw-core-floor-oom: varredura em processo");
    }

    std::cout << "TODOS OS TESTES DE FALTA DE MEMORIA DO PISO PASSARAM" << std::endl;
    return 0;
}
