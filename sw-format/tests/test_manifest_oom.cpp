// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-73: o `sw-format` não pode terminar o processo por falta de memória.
//
// O defeito (medido no PR #37, run 42044485012): no MSVC em Debug (_ITERATOR_DEBUG_LEVEL != 0) os
// construtores padrão e de movimento de std::string, std::vector e dos contêineres de hash são
// `noexcept` e ALOCAM (o proxy dos iteradores). Um std::bad_alloc ali não chega a nenhum `catch`:
// vira std::terminate. Os seis sítios do `sw-format` em que isso acontecia têm teste aqui:
//   1 e 2  ManifestView() (ChunkerInfo::algorithm), no chamador e dentro de ManifestView::open;
//   3      a lista de chaves candidatas em ManifestView::open;
//   4      detail::ContiguousMessage (usada por ed25519_verify e pela assinatura do sw-pack);
//   5      ManifestView::group (GroupInfo);
//   6      a conferência de caminhos que colidem ignorando maiúsculas, em validate.
//
// Como se testa: cada cenário é uma operação; a varredura falha a alocação de número K, para todo
// K que a operação faz, e confere que a falha chega ao tratamento previsto (std::bad_alloc ao
// chamador, ou `false` em ed25519_verify, que é noexcept), que `out` fica intacto e que nada vaza.
//   - por PROCESSO-FILHO (um filho por K): um std::terminate mata só o filho, e o pai conta todos
//     os pontos, distinguindo sucesso, asserção, terminate, timeout e falha ao iniciar;
//   - EM PROCESSO: os mesmos pontos, de novo, no próprio processo (é onde o LeakSanitizer olha).
// As duas rodam em toda configuração, sem exclusões.
//
// Controles positivos: réplicas dos padrões ANTIGOS de cada sítio rodam como filhos. Onde a STL
// aloca em construtor noexcept (MSVC Debug) elas TÊM de terminar: sem isso, "nenhum terminate" não
// provaria nada. Nas outras STLs esses construtores não alocam, e o controle confere isso.
//
// O que este teste NÃO prova: ManifestView::to_model() fica fora (ver o cartão F1-73); alocações
// que o cenário não executa; falhas que não sejam de operator new.

#include "child_process.hpp"
#include "client_min_fixture.hpp"
#include "manifest_oom_fixture.hpp"
#include "oom_inject.hpp"
#include "swarmstream/crypto.hpp"
#include "swarmstream/ed25519_verify.hpp"
#include "swarmstream/manifest_view.hpp"
#include "swarmstream/manifest_wire.hpp"
#include "swarmstream/message_parts.hpp"
#include "swarmstream/sha256.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace sf = swarmstream::pack;
namespace st = swarmstream::test;

namespace {

#if defined(_ITERATOR_DEBUG_LEVEL)
constexpr int kIteratorDebugLevel = _ITERATOR_DEBUG_LEVEL;
#else
constexpr int kIteratorDebugLevel = 0;
#endif
// Nesta STL, os construtores padrão/de movimento noexcept alocam?
constexpr bool kNoexceptCtorsAllocate = kIteratorDebugLevel != 0;

[[noreturn]] void check_failed(const char* file, int line, const char* condition)
{
    oom::disarm();
    std::fprintf(stderr, "%s:%d: falhou: %s (cenario '%s', K=%lld)\n", file, line, condition,
                 oom::g_progress_scenario.load(), static_cast<long long>(oom::g_progress_k.load()));
    std::fflush(nullptr);
    std::_Exit(oom::kExitCheckFailed);
}

// Não some com NDEBUG; termina na hora com código 1, sem caixa de diálogo.
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            check_failed(__FILE__, __LINE__, #__VA_ARGS__);                                        \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------- dados dos cenários

template <std::size_t N>
std::vector<std::byte> bytes_of(const std::array<std::uint8_t, N>& a)
{
    const auto b = std::as_bytes(std::span<const std::uint8_t>(a));
    return std::vector<std::byte>(b.begin(), b.end());
}

sf::Hash32 filled(std::uint8_t first)
{
    sf::Hash32 h{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<std::uint8_t>(first + i);
    }
    return h;
}

// Tudo o que um chamador consegue observar numa visão.
struct Snapshot
{
    sf::Hash32 manifest_id{};
    sf::KeyId key_id{};
    std::size_t payload_size = 0;
    sf::ManifestModel model;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshot(const sf::ManifestView& v)
{
    return Snapshot{v.manifest_id(), v.key_id(), v.payload_size(), v.to_model()};
}

// A mensagem assinada de um manifesto (docs/03 §7.1): prefixo, envelope[0..40) e payload.
struct SignedParts
{
    sf::Ed25519Signature signature{};
    std::span<const std::byte> prefix;
    std::span<const std::byte> header;
    std::span<const std::byte> payload;
};

SignedParts signed_parts(const std::vector<std::byte>& manifest)
{
    SignedParts p;
    std::memcpy(p.signature.data(), manifest.data() + 40, p.signature.size());
    p.prefix = std::as_bytes(std::span<const char>(sf::kSignaturePrefix));
    p.header = std::span<const std::byte>(manifest).first(sf::kSignedHeaderSize);
    p.payload = std::span<const std::byte>(manifest).subspan(sf::kEnvelopeSize);
    return p;
}

std::vector<std::byte> concatenated(const SignedParts& p)
{
    std::vector<std::byte> all;
    all.insert(all.end(), p.prefix.begin(), p.prefix.end());
    all.insert(all.end(), p.header.begin(), p.header.end());
    all.insert(all.end(), p.payload.begin(), p.payload.end());
    return all;
}

// Quantas alocações `fn` faz (sem falhar nenhuma).
template <typename Fn>
std::int64_t count_allocs(Fn&& fn)
{
    oom::arm(oom::kCountOnly);
    fn();
    const std::int64_t n = oom::g_seen.load();
    oom::disarm();
    return n;
}

struct Data
{
    std::vector<std::byte> rich = bytes_of(st::kOomManifest);       // manifest_oom_fixture.hpp
    std::vector<std::byte> mini = bytes_of(st::kClientMinManifest); // client_min_fixture.hpp
    std::array<sf::Ed25519PublicKey, 1> trusted = {st::kClientMinPublicKey};
    std::array<sf::Ed25519PublicKey, 1> unknown = {st::kClientMinPublicKey};
    Snapshot empty;     // de uma visão recém-construída
    Snapshot rich_snap; // de `rich` aberto sem falha
    Snapshot mini_snap;
    std::int64_t view_ctor_allocs = 0; // alocações de ManifestView() nesta STL

    Data()
    {
        unknown[0][0] ^= 0x01; // outra chave: outro key_id
        CHECK(sf::crypto_ready());
        sf::ReadOptions options;
        options.trusted_keys = trusted;
        {
            sf::ManifestView v;
            empty = snapshot(v);
            CHECK(sf::ManifestView::open(rich, options, v).ok());
            rich_snap = snapshot(v);
            CHECK(sf::ManifestView::open(mini, options, v).ok());
            mini_snap = snapshot(v);
        }
        view_ctor_allocs = count_allocs([] {
            const sf::ManifestView v;
            (void)v;
        });
    }
};

// ---------------------------------------------------------------- um ponto da varredura

struct PointInfo
{
    bool fired = false;            // a falha injetada disparou
    std::int64_t allocs = 0;       // alocações vistas com a operação armada
    const char* how = "sem falha"; // como a falha chegou ao chamador
};

void begin_point(const char* scenario, std::int64_t k)
{
    oom::g_progress_scenario.store(scenario);
    oom::g_progress_k.store(k);
    oom::arm(k);
}

PointInfo end_point()
{
    PointInfo info;
    info.fired = oom::g_fired.load();
    info.allocs = oom::g_seen.load();
    oom::disarm();
    return info;
}

// ---- ManifestView::open (sítios 1, 2, 3, 4, 5 e 6)

struct OpenCase
{
    const char* name;
    const std::vector<std::byte>* bytes; // o manifesto a abrir
    const std::vector<std::byte>*
        prior; // já aberto em `out` antes; nullptr: `out` nasce na operação
    std::optional<std::uint64_t> installed_build_seq;
    bool known_key;
    sf::ManifestError expected;
    const char* expected_detail;
};

PointInfo run_open(const Data& d, const OpenCase& c, std::int64_t k)
{
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        sf::ReadOptions trusting;
        trusting.trusted_keys = d.trusted;
        sf::ReadOptions options;
        options.trusted_keys = c.known_key ? std::span<const sf::Ed25519PublicKey>(d.trusted)
                                           : std::span<const sf::Ed25519PublicKey>(d.unknown);
        options.installed_build_seq = c.installed_build_seq;

        std::optional<sf::ManifestView> out;
        if (c.prior != nullptr) {
            out.emplace();
            CHECK(sf::ManifestView::open(*c.prior, trusting, *out).ok());
        }
        const Snapshot before = c.prior != nullptr ? snapshot(*out) : d.empty;
        // Alocações ANTES da verificação da assinatura e DENTRO dela (ed25519_verify é noexcept e
        // devolve false se faltar memória: é o único trecho em que a falha não vira exceção).
        const std::int64_t n_pre = c.prior != nullptr ? 0 : d.view_ctor_allocs;
        std::int64_t n_verify = 0;
        if (c.known_key) {
            const SignedParts p = signed_parts(*c.bytes);
            bool verified = false;
            n_verify = count_allocs([&] {
                verified =
                    sf::ed25519_verify(d.trusted[0], p.signature, {p.prefix, p.header, p.payload});
            });
            CHECK(verified);
        }

        bool threw = false;
        sf::ManifestStatus status;
        begin_point(c.name, k);
        try {
            if (c.prior == nullptr) {
                out.emplace(); // sítio 1: é o chamador quem constrói a visão
            }
            status = sf::ManifestView::open(*c.bytes, options, *out);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        info = end_point();

        if (!info.fired) {
            CHECK(!threw);
            CHECK(status.code == c.expected);
            CHECK(std::strcmp(status.detail, c.expected_detail) == 0);
            if (c.expected == sf::ManifestError::Ok) {
                CHECK(snapshot(*out) == (c.bytes == &d.rich ? d.rich_snap : d.mini_snap));
            } else {
                CHECK(snapshot(*out) == before);
            }
        } else if (threw) {
            if (out.has_value()) {
                CHECK(snapshot(*out) == before); // nada publicado
            } else {
                CHECK(c.prior == nullptr && k <= n_pre); // a própria construção falhou
            }
            info.how = "bad_alloc";
        } else {
            CHECK(k > n_pre && k <= n_pre + n_verify);
            CHECK(status.code == sf::ManifestError::SignatureInvalid);
            CHECK(std::strcmp(status.detail, "assinatura nao confere") == 0);
            CHECK(out.has_value() && snapshot(*out) == before);
            info.how = "assinatura recusada (ed25519_verify noexcept)";
        }
    }
    CHECK(oom::g_live.load() == live); // nada vazou
    return info;
}

// ---- construção padrão dos tipos (sítios 1, 2 e 5, isolados)

template <typename T, typename IsDefault>
PointInfo run_default_ctor(const char* name, std::int64_t k, IsDefault is_default)
{
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        std::optional<T> x;
        bool threw = false;
        begin_point(name, k);
        try {
            x.emplace();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        info = end_point();
        if (!info.fired) {
            CHECK(!threw && x.has_value());
            CHECK(is_default(*x));
        } else {
            CHECK(threw && !x.has_value());
            info.how = "bad_alloc";
        }
    }
    CHECK(oom::g_live.load() == live);
    return info;
}

// ---- ManifestView::group (sítio 5, isolado)

struct ExpectedGroup
{
    std::string_view name;
    std::uint8_t priority;
    std::uint32_t piece_first;
    std::uint32_t piece_count;
};
constexpr std::array<ExpectedGroup, 3> kRichGroups = {{
    {"core", 0, 0, 2},
    {"mapa_floresta_noturna_extra", 1, 2, 1},
    {"dlc1", 2, 3, 1},
}};

bool group_matches(const sf::GroupInfo& g, const ExpectedGroup& e)
{
    return g.name == e.name && g.priority == e.priority && g.piece_first == e.piece_first &&
           g.piece_count == e.piece_count && g.deps.empty();
}

PointInfo run_groups(const Data& d, std::int64_t k)
{
    const char* const name = "ManifestView::group de todos os grupos";
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        sf::ReadOptions trusting;
        trusting.trusted_keys = d.trusted;
        sf::ManifestView view;
        CHECK(sf::ManifestView::open(d.rich, trusting, view).ok());
        CHECK(view.group_count() == kRichGroups.size());
        bool threw = false;
        bool all_match = true;
        begin_point(name, k);
        try {
            for (std::size_t i = 0; i < kRichGroups.size(); ++i) {
                const sf::GroupInfo g = view.group(i);
                all_match = all_match && group_matches(g, kRichGroups[i]);
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        info = end_point();
        if (!info.fired) {
            CHECK(!threw && all_match);
        } else {
            CHECK(threw);
            info.how = "bad_alloc";
        }
        CHECK(snapshot(view) == d.rich_snap); // group() é const: a visão segue igual
    }
    CHECK(oom::g_live.load() == live);
    return info;
}

// ---- ed25519_verify e detail::ContiguousMessage (sítio 4, isolado)

PointInfo run_verify(const Data& d, bool single_part, std::int64_t k)
{
    const char* const name = single_part ? "ed25519_verify, mensagem em 1 parte"
                                         : "ed25519_verify, mensagem em 3 partes";
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        const SignedParts p = signed_parts(d.rich);
        const std::vector<std::byte> whole = concatenated(p);
        bool verified = false;
        begin_point(name, k);
        // noexcept: nunca lança. Se lançasse, seria std::terminate (e o filho acusaria).
        if (single_part) {
            verified =
                sf::ed25519_verify(d.trusted[0], p.signature, {std::span<const std::byte>(whole)});
        } else {
            verified =
                sf::ed25519_verify(d.trusted[0], p.signature, {p.prefix, p.header, p.payload});
        }
        info = end_point();
        if (!info.fired) {
            CHECK(verified);
        } else {
            CHECK(!verified); // faltou memória para montar a mensagem: recusa, nunca aceita
            info.how = "false (recusa)";
        }
    }
    CHECK(oom::g_live.load() == live);
    return info;
}

PointInfo run_contiguous(const Data& d, bool single_part, std::int64_t k)
{
    const char* const name = single_part ? "ContiguousMessage, 1 parte (sem copia)"
                                         : "ContiguousMessage, 3 partes (copia)";
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        const SignedParts p = signed_parts(d.rich);
        const std::vector<std::byte> whole = concatenated(p);
        const sf::MessageParts three = {p.prefix, p.header, p.payload};
        const sf::MessageParts one = {std::span<const std::byte>(whole)};
        std::optional<sf::detail::ContiguousMessage> m;
        bool threw = false;
        begin_point(name, k);
        try {
            m.emplace(single_part ? one : three);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        info = end_point();
        if (!info.fired) {
            CHECK(!threw && m.has_value());
            CHECK(m->size == whole.size());
            CHECK(std::memcmp(m->data, whole.data(), whole.size()) == 0);
            if (single_part) {
                // Uma parte só: aponta para os bytes do chamador, sem cópia.
                CHECK(m->data == reinterpret_cast<const unsigned char*>(whole.data()));
            }
        } else {
            CHECK(threw && !m.has_value());
            info.how = "bad_alloc";
        }
    }
    CHECK(oom::g_live.load() == live);
    return info;
}

// ---- controles: os padrões ANTIGOS, tal como estavam na `main` antes do F1-73

struct OldChunkerInfo // manifest_view.hpp antes do F1-73
{
    std::uint32_t min_size = 0;
    std::uint32_t avg_size = 0;
    std::uint32_t max_size = 0;
    std::string algorithm;
};

struct OldGroupInfo // manifest_view.hpp antes do F1-73
{
    std::string name;
    std::uint8_t priority = 0;
    std::uint32_t piece_first = 0;
    std::uint32_t piece_count = 0;
    std::vector<std::uint16_t> deps;
};

struct OldContiguousMessage // message_parts.hpp antes do F1-73 (caminho de uma parte)
{
    std::vector<unsigned char> storage;
    const unsigned char* data = nullptr;
    unsigned long long size = 0;

    explicit OldContiguousMessage(std::span<const std::byte> part)
    {
        data = reinterpret_cast<const unsigned char*>(part.data());
        size = part.size();
    }
};

// A operação do controle roda armada; aqui nada é conferido além de "não vazou": quem julga é o
// pai, pelo modo como o filho terminou.
template <typename Op>
PointInfo run_control(const char* name, std::int64_t k, Op op)
{
    const std::int64_t live = oom::g_live.load();
    PointInfo info;
    {
        bool threw = false;
        begin_point(name, k);
        try {
            op();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        info = end_point();
        if (info.fired) {
            CHECK(threw);
            info.how = "bad_alloc";
        }
    }
    CHECK(oom::g_live.load() == live);
    return info;
}

// ---------------------------------------------------------------- a lista de cenários

enum class Kind {
    Fixed,           // código corrigido: nenhum ponto pode terminar
    ControlMeasured, // padrão antigo de um dos seis sítios (terminate medido no run 42044485012)
    ControlObserved, // padrão que a leitura da STL aponta; aqui só se mede e se relata
};

struct Scenario
{
    const char* name;
    const char* sites;
    Kind kind;
    PointInfo (*run)(const Data&, std::int64_t);
};

const std::array kScenarios = {
    Scenario{"ManifestView() (construcao padrao)", "1,2", Kind::Fixed,
             [](const Data&, std::int64_t k) {
                 return run_default_ctor<sf::ManifestView>(
                     "ManifestView() (construcao padrao)", k, [](const sf::ManifestView& v) {
                         return v.block_count() == 0 && v.file_count() == 0 &&
                                v.group_count() == 0 && v.game_id().empty() &&
                                v.payload_size() == 0;
                     });
             }},
    Scenario{"ChunkerInfo() (construcao padrao)", "1,2", Kind::Fixed,
             [](const Data&, std::int64_t k) {
                 return run_default_ctor<sf::ChunkerInfo>(
                     "ChunkerInfo() (construcao padrao)", k,
                     [](const sf::ChunkerInfo& c) { return c == sf::ChunkerInfo{0, 0, 0, ""}; });
             }},
    Scenario{"GroupInfo() (construcao padrao)", "5", Kind::Fixed,
             [](const Data&, std::int64_t k) {
                 return run_default_ctor<sf::GroupInfo>(
                     "GroupInfo() (construcao padrao)", k, [](const sf::GroupInfo& g) {
                         return g.name.empty() && g.priority == 0 && g.piece_first == 0 &&
                                g.piece_count == 0 && g.deps.empty();
                     });
             }},
    Scenario{"open ok (3 grupos, 5 arquivos), out criado na operacao", "1,2,3,4,5,6", Kind::Fixed,
             [](const Data& d, std::int64_t k) {
                 return run_open(d,
                                 {"open ok (3 grupos, 5 arquivos), out criado na operacao", &d.rich,
                                  nullptr, std::nullopt, true, sf::ManifestError::Ok, ""},
                                 k);
             }},
    Scenario{"open ok (3 grupos, 5 arquivos), out ja valido", "2,3,4,5,6", Kind::Fixed,
             [](const Data& d, std::int64_t k) {
                 return run_open(d,
                                 {"open ok (3 grupos, 5 arquivos), out ja valido", &d.rich, &d.mini,
                                  std::nullopt, true, sf::ManifestError::Ok, ""},
                                 k);
             }},
    Scenario{"open ok (cliente minimo), out ja valido", "2,3,4,5,6", Kind::Fixed,
             [](const Data& d, std::int64_t k) {
                 return run_open(d,
                                 {"open ok (cliente minimo), out ja valido", &d.mini, &d.rich,
                                  std::nullopt, true, sf::ManifestError::Ok, ""},
                                 k);
             }},
    Scenario{"open recusado no fim (rollback), out ja valido", "2,3,4,5,6", Kind::Fixed,
             [](const Data& d, std::int64_t k) {
                 return run_open(d,
                                 {"open recusado no fim (rollback), out ja valido", &d.rich,
                                  &d.mini, std::uint64_t{8}, true, sf::ManifestError::Rollback,
                                  "build_seq menor que o instalado"},
                                 k);
             }},
    Scenario{"open com chave desconhecida, out ja valido", "3", Kind::Fixed,
             [](const Data& d, std::int64_t k) {
                 return run_open(d,
                                 {"open com chave desconhecida, out ja valido", &d.rich, &d.mini,
                                  std::nullopt, false, sf::ManifestError::SignatureInvalid,
                                  "chave desconhecida (key_id)"},
                                 k);
             }},
    Scenario{"ManifestView::group de todos os grupos", "5", Kind::Fixed, run_groups},
    Scenario{"ed25519_verify, mensagem em 3 partes", "4", Kind::Fixed,
             [](const Data& d, std::int64_t k) { return run_verify(d, false, k); }},
    Scenario{"ed25519_verify, mensagem em 1 parte", "4", Kind::Fixed,
             [](const Data& d, std::int64_t k) { return run_verify(d, true, k); }},
    Scenario{"ContiguousMessage, 3 partes (copia)", "4", Kind::Fixed,
             [](const Data& d, std::int64_t k) { return run_contiguous(d, false, k); }},
    Scenario{"ContiguousMessage, 1 parte (sem copia)", "4", Kind::Fixed,
             [](const Data& d, std::int64_t k) { return run_contiguous(d, true, k); }},

    Scenario{"ANTIGO sitios 1 e 2: struct com std::string, construcao padrao", "1,2",
             Kind::ControlMeasured,
             [](const Data&, std::int64_t k) {
                 return run_control("ANTIGO sitios 1 e 2", k, [] {
                     std::optional<OldChunkerInfo> x;
                     x.emplace();
                 });
             }},
    Scenario{"ANTIGO sitio 3: std::vector<const chave*>, construcao padrao", "3",
             Kind::ControlMeasured,
             [](const Data&, std::int64_t k) {
                 return run_control("ANTIGO sitio 3", k, [] {
                     std::optional<std::vector<const sf::Ed25519PublicKey*>> x;
                     x.emplace();
                 });
             }},
    Scenario{"ANTIGO sitio 4: ContiguousMessage com std::vector membro", "4", Kind::ControlMeasured,
             [](const Data& d, std::int64_t k) {
                 const std::span<const std::byte> part(d.rich);
                 return run_control("ANTIGO sitio 4", k, [part] {
                     std::optional<OldContiguousMessage> x;
                     x.emplace(part);
                 });
             }},
    Scenario{"ANTIGO sitio 5: GroupInfo (string e vector), construcao padrao", "5",
             Kind::ControlMeasured,
             [](const Data&, std::int64_t k) {
                 return run_control("ANTIGO sitio 5", k, [] {
                     std::optional<OldGroupInfo> x;
                     x.emplace();
                 });
             }},
    Scenario{"ANTIGO sitio 6: std::unordered_set<std::string>, construcao padrao", "6",
             Kind::ControlMeasured,
             [](const Data&, std::int64_t k) {
                 return run_control("ANTIGO sitio 6", k, [] {
                     std::optional<std::unordered_set<std::string>> x;
                     x.emplace();
                 });
             }},
    Scenario{"OBSERVADO: std::unordered_set<std::string>(8) (outro construtor)", "6",
             Kind::ControlObserved,
             [](const Data&, std::int64_t k) {
                 return run_control("OBSERVADO unordered_set(8)", k, [] {
                     std::optional<std::unordered_set<std::string>> x;
                     x.emplace(std::size_t{8});
                 });
             }},
    Scenario{"OBSERVADO: std::string construida por movimento", "5,6", Kind::ControlObserved,
             [](const Data&, std::int64_t k) {
                 return run_control("OBSERVADO string(string&&)", k, [] {
                     // A origem nasce por construtor que pode lançar (os primeiros K); o ponto
                     // observado é o último: a construção por movimento.
                     std::string source(std::size_t{40}, 'x');
                     std::optional<std::string> x;
                     x.emplace(std::move(source));
                 });
             }},
};

// ---------------------------------------------------------------- processo-filho

// Filho: um ponto K de um cenário. Sai com kExitFiredOk se a falha disparou e tudo conferiu, com
// kExitNotFiredOk se a operação terminou antes de K (fim do cenário); a asserção sai com 1 e o
// std::terminate com kExitTerminate.
int run_child_point(int argc, char** argv)
{
    if (argc != 4) return oom::kExitUsage;
    const std::size_t index = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    const std::int64_t k = std::strtoll(argv[3], nullptr, 10);
    if (index >= kScenarios.size() || k < 1) return oom::kExitUsage;
    const Data d;
    const PointInfo info = kScenarios[index].run(d, k);
    std::printf("PONTO|%s|K=%lld|disparou=%d|%s\n", kScenarios[index].name,
                static_cast<long long>(k), info.fired ? 1 : 0, info.how);
    std::fflush(nullptr);
    return info.fired ? oom::kExitFiredOk : oom::kExitNotFiredOk;
}

void throw_bad_alloc() { throw std::bad_alloc(); }
void call_noexcept(void (*fn)()) noexcept { fn(); }

// Filho de autoteste: termina de cada jeito que o pai precisa saber distinguir.
int run_child_selftest(const std::string& mode)
{
    oom::g_progress_scenario.store("autoteste");
    if (mode == "terminate") {
        void (*volatile fn)() = throw_bad_alloc;
        call_noexcept(fn);
        return oom::kExitUsage; // não chega aqui
    }
    if (mode == "falha") {
        CHECK(mode.empty());
    }
    if (mode == "travar") {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (mode == "disparou") {
        std::printf("PONTO|autoteste|K=1|disparou=1|bad_alloc\n");
        return oom::kExitFiredOk;
    }
    if (mode == "fim") {
        std::printf("PONTO|autoteste|K=1|disparou=0|sem falha\n");
        return oom::kExitNotFiredOk;
    }
    if (mode == "mudo") { // sai com 0 sem dizer que rodou: não pode contar como ponto bom
        return oom::kExitFiredOk;
    }
    return oom::kExitUsage;
}

enum class Outcome { FiredOk, NotFired, CheckFailed, Terminate, Timeout, NotStarted, Unexpected };

const char* to_string(Outcome o)
{
    switch (o) {
    case Outcome::FiredOk: return "ok";
    case Outcome::NotFired: return "fim";
    case Outcome::CheckFailed: return "FALHA";
    case Outcome::Terminate: return "TERMINATE";
    case Outcome::Timeout: return "TIMEOUT";
    case Outcome::NotStarted: return "NAO INICIOU";
    case Outcome::Unexpected: return "INESPERADO";
    }
    return "?";
}

Outcome classify(const st::ChildResult& r)
{
    if (!r.started) return Outcome::NotStarted;
    if (r.timed_out) return Outcome::Timeout;
    if (r.signaled) return Outcome::Unexpected;
    const bool said_fired = r.output.find("|disparou=1|") != std::string::npos;
    const bool said_end = r.output.find("|disparou=0|") != std::string::npos;
    switch (r.code) {
    case oom::kExitFiredOk: return said_fired ? Outcome::FiredOk : Outcome::Unexpected;
    case oom::kExitNotFiredOk: return said_end ? Outcome::NotFired : Outcome::Unexpected;
    case oom::kExitCheckFailed: return Outcome::CheckFailed;
    case oom::kExitTerminate: return Outcome::Terminate;
    default: return Outcome::Unexpected;
    }
}

// A pilha da alocação em que a falha disparou, se o filho a imprimiu (linha "FRAMES|rva,rva...").
std::string fire_stack(const std::string& output, oom::SymbolSession& symbols)
{
    const std::size_t at = output.find("FRAMES|");
    if (at == std::string::npos) return "(o filho nao imprimiu a pilha)";
    std::vector<std::uint64_t> rvas;
    const char* p = output.c_str() + at + 7;
    while (*p != '\0' && *p != '\n' && *p != '\r') {
        char* end = nullptr;
        rvas.push_back(std::strtoull(p, &end, 16));
        if (end == p) break;
        p = (*end == ',') ? end + 1 : end;
    }
    return symbols.describe(rvas);
}

constexpr std::chrono::milliseconds kChildTimeout{60000};
constexpr std::int64_t kControlMaxK = 8;

struct Totals
{
    int scenarios = 0;
    int children = 0;
    int fired = 0;
    int terminate = 0;
    int bad = 0; // asserção, timeout, não iniciou, inesperado, ou contagem que não bate
    double seconds = 0.0;
};

st::ChildResult spawn_point(const std::filesystem::path& exe, std::size_t index, std::int64_t k)
{
    return st::run_child(exe, {"--ponto", std::to_string(index), std::to_string(k)}, kChildTimeout);
}

// O pai precisa distinguir cada jeito de um filho terminar; sem isto, "nenhum terminate" poderia
// ser só um pai que não enxerga.
void test_child_classification(const std::filesystem::path& exe)
{
    std::printf("[Test] o pai distingue como o filho terminou\n");
    const auto run = [&](const char* mode, int timeout_ms) {
        return classify(
            st::run_child(exe, {"--ponto-teste", mode}, std::chrono::milliseconds(timeout_ms)));
    };
    CHECK(run("disparou", 60000) == Outcome::FiredOk);
    CHECK(run("fim", 60000) == Outcome::NotFired);
    CHECK(run("falha", 60000) == Outcome::CheckFailed);
    CHECK(run("terminate", 60000) == Outcome::Terminate);
    CHECK(run("travar", 2000) == Outcome::Timeout);
    CHECK(run("mudo", 60000) == Outcome::Unexpected);
    CHECK(run("modo-que-nao-existe", 60000) == Outcome::Unexpected);
    const std::filesystem::path missing = exe.parent_path() / "sw-format-oom-nao-existe.exe";
    CHECK(classify(st::run_child(missing, {"--ponto-teste", "fim"},
                                 std::chrono::milliseconds(60000))) == Outcome::NotStarted);
}

// Varredura de um cenário corrigido: um filho por K, de 1 até uma além do total de alocações.
void sweep_fixed_with_children(const std::filesystem::path& exe, const Data& d, std::size_t index,
                               oom::SymbolSession& symbols, Totals& totals)
{
    const Scenario& s = kScenarios[index];
    const std::int64_t expected = s.run(d, oom::kCountOnly).allocs;
    int fired = 0;
    int terminate = 0;
    int bad = 0;
    for (std::int64_t k = 1; k <= expected + 1; ++k) {
        const st::ChildResult r = spawn_point(exe, index, k);
        const Outcome o = classify(r);
        ++totals.children;
        const Outcome wanted = k <= expected ? Outcome::FiredOk : Outcome::NotFired;
        if (o == Outcome::FiredOk) ++fired;
        if (o == Outcome::Terminate) ++terminate;
        if (o != wanted) {
            if (o != Outcome::Terminate) ++bad;
            std::printf("[F1-73]   K=%lld: %s (esperava %s); pilha: %s\n",
                        static_cast<long long>(k), to_string(o), to_string(wanted),
                        fire_stack(r.output, symbols).c_str());
            std::printf("----- saida do filho -----\n%s--------------------------\n",
                        r.output.c_str());
        }
    }
    std::printf("[F1-73] cenario '%s' (sitios %s): %lld alocacoes; filhos: %d disparos, "
                "%d terminate, %d outras falhas\n",
                s.name, s.sites, static_cast<long long>(expected), fired, terminate, bad);
    ++totals.scenarios;
    totals.fired += fired;
    totals.terminate += terminate;
    totals.bad += bad;
}

// Controle: o padrão antigo. Devolve quantos pontos terminaram.
int sweep_control(const std::filesystem::path& exe, std::size_t index, oom::SymbolSession& symbols,
                  bool& broken)
{
    const Scenario& s = kScenarios[index];
    int terminate = 0;
    int points = 0;
    std::string first_stack;
    for (std::int64_t k = 1; k <= kControlMaxK; ++k) {
        const st::ChildResult r = spawn_point(exe, index, k);
        const Outcome o = classify(r);
        if (o == Outcome::NotFired) break;
        ++points;
        if (o == Outcome::Terminate) {
            if (terminate == 0) first_stack = fire_stack(r.output, symbols);
            ++terminate;
        } else if (o != Outcome::FiredOk) {
            broken = true;
            std::printf("[F1-73]   controle K=%lld: %s\n----- saida do filho -----\n%s"
                        "--------------------------\n",
                        static_cast<long long>(k), to_string(o), r.output.c_str());
        }
    }
    std::printf("[F1-73] controle '%s' (sitios %s): %d ponto(s), %d terminate%s%s\n", s.name,
                s.sites, points, terminate, terminate > 0 ? "; 1a pilha: " : "",
                first_stack.c_str());
    return terminate;
}

void sweep_with_children(const std::filesystem::path& exe, const Data& d)
{
    std::printf("[Test] varredura por processo-filho (um filho por K)\n");
    oom::SymbolSession symbols;
    const auto t0 = std::chrono::steady_clock::now();

    // 1) Controles: os padrões antigos têm de terminar onde a STL aloca em construtor noexcept, e
    //    não podem terminar onde ela não aloca.
    bool controls_broken = false;
    for (std::size_t i = 0; i < kScenarios.size(); ++i) {
        if (kScenarios[i].kind == Kind::Fixed) continue;
        const int terminate = sweep_control(exe, i, symbols, controls_broken);
        if (kScenarios[i].kind == Kind::ControlMeasured) {
            CHECK((terminate > 0) == kNoexceptCtorsAllocate);
        }
    }
    CHECK(!controls_broken);

    // 2) O código corrigido: nenhum ponto termina, todos são alcançados.
    Totals totals;
    for (std::size_t i = 0; i < kScenarios.size(); ++i) {
        if (kScenarios[i].kind != Kind::Fixed) continue;
        sweep_fixed_with_children(exe, d, i, symbols, totals);
    }
    totals.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[F1-73] varredura por processo-filho: %d cenarios, %d filhos, %d disparos, "
                "%d terminate, %d outras falhas, %.1f s (com os controles)\n",
                totals.scenarios, totals.children, totals.fired, totals.terminate, totals.bad,
                totals.seconds);
    std::fflush(stdout);
    CHECK(totals.terminate == 0);
    CHECK(totals.bad == 0);
    CHECK(totals.fired > 0);
}

// Os mesmos pontos, no próprio processo (LeakSanitizer; e prova que nenhum ponto derruba o
// processo).
void sweep_in_process(const Data& d)
{
    std::printf("[Test] varredura em processo\n");
    const auto t0 = std::chrono::steady_clock::now();
    int scenarios = 0;
    long long fired = 0;
    for (const Scenario& s : kScenarios) {
        if (s.kind != Kind::Fixed) continue;
        const std::int64_t expected = s.run(d, oom::kCountOnly).allocs;
        std::int64_t k = 1;
        for (;; ++k) {
            const PointInfo info = s.run(d, k);
            if (!info.fired) break;
        }
        CHECK(k - 1 == expected); // todos os pontos alcançados, nenhum a mais
        ++scenarios;
        fired += k - 1;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[F1-73] varredura em processo: %d cenarios, %lld disparos, 0 terminate, %.1f s\n",
                scenarios, fired, seconds);
}

// ---------------------------------------------------------------- testes sem injeção

void print_stl_facts()
{
#if defined(_MSC_FULL_VER)
    const long msc_full_ver = _MSC_FULL_VER;
#else
    const long msc_full_ver = 0; // não é MSVC
#endif
#if defined(NDEBUG)
    const int ndebug = 1;
#else
    const int ndebug = 0;
#endif
    const std::string_view sodium = sf::crypto_version();
    std::printf("[F1-73] STL: _MSC_FULL_VER=%ld _ITERATOR_DEBUG_LEVEL=%d NDEBUG=%d "
                "libsodium='%.*s'\n",
                msc_full_ver, kIteratorDebugLevel, ndebug, static_cast<int>(sodium.size()),
                sodium.data());
    std::printf("[F1-73] noexcept: string()=%d vector()=%d unordered_set()=%d string(string&&)=%d "
                "| ChunkerInfo()=%d GroupInfo()=%d ManifestView()=%d ManifestModel()=%d\n",
                std::is_nothrow_default_constructible_v<std::string> ? 1 : 0,
                std::is_nothrow_default_constructible_v<std::vector<std::uint16_t>> ? 1 : 0,
                std::is_nothrow_default_constructible_v<std::unordered_set<std::string>> ? 1 : 0,
                std::is_nothrow_move_constructible_v<std::string> ? 1 : 0,
                std::is_nothrow_default_constructible_v<sf::ChunkerInfo> ? 1 : 0,
                std::is_nothrow_default_constructible_v<sf::GroupInfo> ? 1 : 0,
                std::is_nothrow_default_constructible_v<sf::ManifestView> ? 1 : 0,
                std::is_nothrow_default_constructible_v<sf::ManifestModel> ? 1 : 0);
}

// As propriedades de que a correção depende. Antes do F1-73 as três primeiras eram `true`: a
// construção padrão era noexcept e, no MSVC Debug, alocava.
void test_type_properties()
{
    std::printf("[Test] construcao padrao de ChunkerInfo, GroupInfo e ManifestView pode lancar\n");
    CHECK(!std::is_nothrow_default_constructible_v<sf::ChunkerInfo>);
    CHECK(!std::is_nothrow_default_constructible_v<sf::GroupInfo>);
    CHECK(!std::is_nothrow_default_constructible_v<sf::ManifestView>);
    // O que os consumidores usam e não pode mudar: a cópia da visão pode lançar (Vfs::build
    // depende disso), a publicação por movimento não lança, e os dois structs continuam agregados
    // (o sw-pack os inicializa por lista).
    CHECK(!std::is_nothrow_copy_constructible_v<sf::ManifestView>);
    CHECK(std::is_nothrow_move_assignable_v<sf::ManifestView>);
    CHECK(std::is_aggregate_v<sf::ChunkerInfo>);
    CHECK(std::is_aggregate_v<sf::GroupInfo>);
    const sf::ChunkerInfo c{1, 2, 3, "x"};
    CHECK(c.min_size == 1 && c.avg_size == 2 && c.max_size == 3 && c.algorithm == "x");
    const sf::GroupInfo g{"core", 1, 2, 3, {4, 5}};
    CHECK(g.name == "core" && g.priority == 1 && g.piece_first == 2 && g.piece_count == 3 &&
          g.deps == std::vector<std::uint16_t>({4, 5}));
}

// Os resultados de SUCESSO são os mesmos de antes: manifest_id conferido contra o SHA-256
// calculado por fora do C++ (Python hashlib) e cada acessor contra o conteúdo declarado na fixture.
void test_success_values(const Data& d)
{
    std::printf("[Test] abrir os dois manifestos: manifest_id e todos os acessores\n");
    sf::ReadOptions options;
    options.trusted_keys = d.trusted;
    {
        sf::ManifestView v;
        const sf::ManifestStatus s = sf::ManifestView::open(d.rich, options, v);
        CHECK(s.ok() && std::strcmp(s.detail, "") == 0);
        CHECK(sf::to_hex(v.manifest_id()) ==
              "f9022c5484969b3e1e43c8424477f34401edfaff4c8a66f7f31e7a330e8c791d");
        CHECK(v.key_id() == sf::key_id_of(d.trusted[0]));
        CHECK(v.payload_size() == d.rich.size() - sf::kEnvelopeSize);
        CHECK(v.game_id() == "swarmstream.f173" && v.build_id() == "0.1.0+f173");
        CHECK(v.build_seq() == 7 && v.created_at() == 1'790'000'000 && v.min_reader() == 0);

        const std::array<std::uint32_t, 5> raw = {1000, 2000, 3000, 4000, 5000};
        const std::array<std::uint32_t, 5> packed = {1000, 1500, 2500, 4000, 100};
        const std::array<sf::Codec, 5> codec = {sf::Codec::None, sf::Codec::Zstd, sf::Codec::Lz4,
                                                sf::Codec::None, sf::Codec::Zstd};
        CHECK(v.block_count() == 5);
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(v.block_hash(i) == filled(static_cast<std::uint8_t>(0x10 * (i + 1))));
            CHECK(v.block_raw_size(i) == raw[i] && v.block_packed_size(i) == packed[i]);
            CHECK(v.block_codec(i) == codec[i]);
        }
        const std::array<std::uint32_t, 4> piece_size = {2500, 2500, 4000, 100};
        const std::array<std::uint32_t, 4> piece_first = {0, 2, 3, 4};
        const std::array<std::uint32_t, 4> piece_blocks = {2, 1, 1, 1};
        const std::array<std::uint16_t, 4> piece_group = {0, 0, 1, 2};
        CHECK(v.piece_count() == 4);
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK(v.piece_hash(i) == filled(static_cast<std::uint8_t>(0xA0 + i)));
            CHECK(v.piece_size(i) == piece_size[i] && v.piece_first_block(i) == piece_first[i]);
            CHECK(v.piece_block_count(i) == piece_blocks[i] && v.piece_group(i) == piece_group[i]);
        }
        const std::array<std::uint32_t, 6> refs = {0, 1, 2, 3, 0, 4};
        CHECK(v.block_ref_count() == refs.size());
        for (std::size_t i = 0; i < refs.size(); ++i) {
            CHECK(v.block_ref(i) == refs[i]);
        }
        struct File
        {
            std::string_view path;
            std::uint64_t size;
            sf::FileClass cls;
            std::uint16_t group;
            std::uint32_t first_ref;
            std::uint32_t ref_count;
        };
        const std::array<File, 5> files = {{
            {"Dados/Config.json", 1000, sf::FileClass::Core, 0, 0, 1},
            {"dados/base.bin", 5000, sf::FileClass::Core, 0, 1, 2},
            {"dados/extra/Floresta.bin", 5000, sf::FileClass::Group, 1, 3, 2},
            {"dlc/um.bin", 5000, sf::FileClass::Optional, 2, 5, 1},
            {"vazio.bin", 0, sf::FileClass::Core, 0, 6, 0},
        }};
        CHECK(v.file_count() == files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            CHECK(v.file_path(i) == files[i].path && v.file_size(i) == files[i].size);
            CHECK(v.file_class(i) == files[i].cls && v.file_group(i) == files[i].group);
            CHECK(v.file_first_ref(i) == files[i].first_ref);
            CHECK(v.file_ref_count(i) == files[i].ref_count);
        }
        CHECK(v.group_count() == kRichGroups.size());
        for (std::size_t i = 0; i < kRichGroups.size(); ++i) {
            CHECK(group_matches(v.group(i), kRichGroups[i]));
        }
        const sf::ManifestModel m = v.to_model();
        CHECK(m.chunker ==
              sf::ChunkerInfo{32U * 1024U, 128U * 1024U, 256U * 1024U, "swarmstream-fastcdc-v0"});
        CHECK(m.game_id == "swarmstream.f173" && m.build_seq == 7 && m.cipher_id == 0);
        CHECK(m.groups.size() == 3 && group_matches(m.groups[1], kRichGroups[1]));
        CHECK(m.file_path.size() == 5 && m.file_path[2] == "dados/extra/Floresta.bin");
        CHECK(m.cdn_base_urls == std::vector<std::string>({"https://cdn.example.test/f173"}));
        CHECK(m.trackers == std::vector<std::string>({"https://tracker.example.test/announce"}));
        CHECK(m.torrent_infohashes ==
              std::vector<sf::Hash32>({filled(0x01), filled(0x41), filled(0x81)}));
    }
    {
        sf::ManifestView v;
        CHECK(sf::ManifestView::open(d.mini, options, v).ok());
        CHECK(sf::to_hex(v.manifest_id()) ==
              "f250a581f52f60697f81f7c7d174c90c048b311acf373bf1fa52ce7e91dffbaa");
        CHECK(v.game_id() == "swarmstream.clientmin" && v.build_id() == "0.1.0+clientmin");
        CHECK(v.build_seq() == 1 && v.created_at() == 1'790'000'000);
        CHECK(v.block_count() == 1 && v.piece_count() == 1 && v.file_count() == 1);
        CHECK(v.block_codec(0) == sf::Codec::Zstd && v.block_raw_size(0) == 192 &&
              v.block_packed_size(0) == 34);
        CHECK(v.piece_hash(0) == filled(0xA0) && v.piece_size(0) == 34);
        CHECK(v.file_path(0) == "dados/bloco.bin" && v.file_size(0) == 192);
        CHECK(v.group_count() == 1 && group_matches(v.group(0), {"core", 0, 0, 1}));
        CHECK(v.to_model().chunker.algorithm == "swarmstream-fastcdc-v0");
    }
}

// Sítio 3: a escolha da chave pelo key_id dá o mesmo resultado de antes, com a chave certa em
// qualquer posição, repetida, ausente, e com assinatura que não confere. `out` só muda no sucesso.
void test_key_selection(const Data& d)
{
    std::printf("[Test] escolha da chave pelo key_id e recusa da assinatura\n");
    const sf::Ed25519PublicKey right = d.trusted[0];
    const sf::Ed25519PublicKey wrong = d.unknown[0];
    sf::ReadOptions trusting;
    trusting.trusted_keys = d.trusted;

    const auto open_with = [&](std::span<const sf::Ed25519PublicKey> keys,
                               const std::vector<std::byte>& bytes, sf::ManifestError code,
                               const char* detail) {
        sf::ManifestView out;
        CHECK(sf::ManifestView::open(d.mini, trusting, out).ok());
        sf::ReadOptions options;
        options.trusted_keys = keys;
        const sf::ManifestStatus s = sf::ManifestView::open(bytes, options, out);
        CHECK(s.code == code);
        CHECK(std::strcmp(s.detail, detail) == 0);
        CHECK(snapshot(out) == (s.ok() ? d.rich_snap : d.mini_snap));
    };
    const auto ok = sf::ManifestError::Ok;
    const auto bad = sf::ManifestError::SignatureInvalid;

    const std::array<sf::Ed25519PublicKey, 2> wrong_right = {wrong, right};
    const std::array<sf::Ed25519PublicKey, 2> right_wrong = {right, wrong};
    const std::array<sf::Ed25519PublicKey, 3> right_twice = {wrong, right, right};
    const std::array<sf::Ed25519PublicKey, 2> wrong_twice = {wrong, wrong};
    open_with(d.trusted, d.rich, ok, "");
    open_with(wrong_right, d.rich, ok, "");
    open_with(right_wrong, d.rich, ok, "");
    open_with(right_twice, d.rich, ok, "");
    open_with(d.unknown, d.rich, bad, "chave desconhecida (key_id)");
    open_with(wrong_twice, d.rich, bad, "chave desconhecida (key_id)");
    open_with({}, d.rich, bad, "chave desconhecida (key_id)");

    // Chave certa, conteúdo ou assinatura alterados: a chave é achada, a assinatura não confere.
    std::vector<std::byte> payload_changed = d.rich;
    payload_changed.back() ^= std::byte{0x01};
    open_with(d.trusted, payload_changed, bad, "assinatura nao confere");
    open_with(right_twice, payload_changed, bad, "assinatura nao confere");
    std::vector<std::byte> signature_changed = d.rich;
    signature_changed[40] ^= std::byte{0x01};
    open_with(d.trusted, signature_changed, bad, "assinatura nao confere");
    // key_id do envelope alterado: nenhuma chave confiável tem esse id.
    std::vector<std::byte> key_id_changed = d.rich;
    key_id_changed[24] ^= std::byte{0x01};
    open_with(d.trusted, key_id_changed, bad, "chave desconhecida (key_id)");
}

// Sítio 3, o que a varredura acima não distingue: TODAS as chaves com o key_id do envelope são
// tentadas, não só a primeira. Duas chaves diferentes com o mesmo key_id exigiriam uma colisão de
// SHA-256; aqui a mesma chave aparece duas vezes e a PRIMEIRA verificação falha por falta de
// memória (ed25519_verify devolve false): a segunda tem de aceitar, e `out` recebe o manifesto.
void test_next_candidate_after_refusal(const Data& d)
{
    std::printf("[Test] a chave seguinte com o mesmo key_id e tentada quando a primeira recusa\n");
    const std::array<sf::Ed25519PublicKey, 2> twice = {d.trusted[0], d.trusted[0]};
    sf::ReadOptions options;
    options.trusted_keys = twice;
    const SignedParts p = signed_parts(d.rich);
    const std::int64_t n_verify = count_allocs([&] {
        CHECK(sf::ed25519_verify(d.trusted[0], p.signature, {p.prefix, p.header, p.payload}));
    });
    CHECK(n_verify >= 1); // a mensagem de 3 partes é copiada: há o que falhar
    for (std::int64_t k = 1; k <= n_verify; ++k) {
        sf::ManifestView out;
        begin_point("duas chaves iguais, a primeira verificacao sem memoria", k);
        const sf::ManifestStatus s = sf::ManifestView::open(d.rich, options, out);
        const PointInfo info = end_point();
        CHECK(info.fired);
        CHECK(s.ok());
        CHECK(snapshot(out) == d.rich_snap);
    }
}

} // namespace

int main(int argc, char** argv)
{
    oom::make_abort_fail_instead_of_hang();
    oom::warm_up_exceptions();
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Filho: um ponto K (ver run_child_point). Não roda mais nada.
    if (argc >= 2 && std::strcmp(argv[1], "--ponto") == 0) return run_child_point(argc, argv);
    if (argc == 3 && std::strcmp(argv[1], "--ponto-teste") == 0) {
        return run_child_selftest(argv[2]);
    }
    if (argc != 1) {
        std::fprintf(stderr, "argumento desconhecido: %s\n", argv[1]);
        return oom::kExitUsage;
    }

    const std::filesystem::path exe = st::self_exe(argv[0]);
    print_stl_facts();
    const Data d;
    test_type_properties();
    test_success_values(d);
    test_key_selection(d);
    test_child_classification(exe);
    sweep_with_children(exe, d);
    sweep_in_process(d);
    test_next_candidate_after_refusal(d);
    std::printf("[F1-73] sw-format sem terminate por falta de memoria: OK\n");
    return 0;
}
