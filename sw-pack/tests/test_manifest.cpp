// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Testes do manifesto v0 e das primitivas que ele usa (F1-04):
//   Ed25519 (libsodium) contra o RFC 8032, vectors-v0.json e 96 pares gerados pelo OpenSSL;
//   leitura dos vetores de envelope (recusas e sondagem do min_reader);
//   manifesto lido de volta idêntico; adulterado recusado (todo byte, toda regra do §8);
//   payload malformado com assinatura válida nunca derruba o leitor.
//   sw-pack-manifest-tests <vectors-v0.json> <ed25519-vectors.txt>

#include "check.hpp"
#include "json_min.hpp"
#include "manifest_fixture.hpp"
#include "swarmstream/ed25519.hpp"
#include "swarmstream/manifest.hpp"
#include "swarmstream/sha256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sp = swarmstream::pack;
namespace st = swarmstream::test;

namespace {

using Bytes = std::vector<std::byte>;
using st::Json;
using st::JsonParser;

std::vector<std::uint8_t> from_hex(const std::string& hex)
{
    SW_CHECK(hex.size() % 2 == 0);
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

Bytes bytes_from_hex(const std::string& hex)
{
    Bytes out;
    for (const std::uint8_t b : from_hex(hex)) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

template <std::size_t N>
std::array<std::uint8_t, N> array_from_hex(const std::string& hex)
{
    const auto v = from_hex(hex);
    SW_CHECK(v.size() == N);
    std::array<std::uint8_t, N> out{};
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

template <std::size_t N>
std::string hex_of(const std::array<std::uint8_t, N>& a)
{
    return sp::to_hex(a);
}

// A chave de teste (semente 00..1f) publicada em sw-pack/testdata (ADR-0012, item 4). Fica só
// nos testes: a biblioteca não a conhece, e nenhum build de release pode confiar nela.
sp::Ed25519Seed test_seed()
{
    sp::Ed25519Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::uint8_t>(i);
    }
    return sp::Ed25519Seed(seed);
}

sp::Ed25519Seed other_seed()
{
    sp::Ed25519Seed seed = test_seed();
    seed[0] ^= 0xFF;
    return seed;
}

// O libsodium iniciou e derivou/assinou; senão o teste nem faz sentido.
sp::Ed25519PublicKey public_key_of(const sp::Ed25519Seed& seed)
{
    const auto key = sp::ed25519_public_key(seed);
    SW_CHECK(key.has_value());
    return *key;
}

sp::Ed25519Signature sign_of(const sp::Ed25519Seed& seed, sp::MessageParts message)
{
    const auto signature = sp::ed25519_sign(seed, message);
    SW_CHECK(signature.has_value());
    return *signature;
}

sp::ReadOptions trusting(const std::array<sp::Ed25519PublicKey, 1>& keys)
{
    sp::ReadOptions options;
    options.trusted_keys = keys;
    return options;
}

sp::ManifestStatus open_bytes(const Bytes& bytes, const sp::ReadOptions& options,
                              sp::ManifestView* out = nullptr)
{
    sp::ManifestView local;
    return sp::ManifestView::open(bytes, options, out != nullptr ? *out : local);
}

// ---------------------------------------------------------------- Ed25519 (libsodium)

void test_ed25519_rfc8032()
{
    // RFC 8032 §7.1, teste 1 (mensagem vazia) e teste 2 (1 byte, 0x72).
    {
        const auto seed =
            array_from_hex<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto pk = public_key_of(seed);
        SW_CHECK(hex_of(pk) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto sig = sign_of(seed, {});
        SW_CHECK(hex_of(sig) ==
                 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bac"
                 "c61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        SW_CHECK(sp::ed25519_verify(pk, sig, {}));
    }
    {
        const auto seed =
            array_from_hex<32>("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        const auto pk = public_key_of(seed);
        SW_CHECK(hex_of(pk) == "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
        const std::byte msg[1] = {std::byte{0x72}};
        const auto sig = sign_of(seed, {std::span<const std::byte>(msg)});
        SW_CHECK(hex_of(sig) ==
                 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e"
                 "458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
        SW_CHECK(sp::ed25519_verify(pk, sig, {std::span<const std::byte>(msg)}));
        // A mesma mensagem em duas partes dá a mesma assinatura (sem copiar o payload).
        const std::byte empty[1] = {};
        SW_CHECK(sign_of(seed, {std::span<const std::byte>(empty, 0),
                                std::span<const std::byte>(msg)}) == sig);
    }
}

// Pares (semente, chave, mensagem, assinatura) gerados pelo OpenSSL
// (testdata/gen_ed25519_vectors.py): o libsodium tem de reproduzir chave pública e assinatura byte
// a byte e aceitar a assinatura.
void test_ed25519_openssl_vectors(const char* path)
{
    std::ifstream file(path);
    SW_CHECK(file.good());
    std::string seed_hex, key_hex, message_hex, signature_hex;
    std::size_t cases = 0;
    while (file >> seed_hex >> key_hex >> message_hex >> signature_hex) {
        const auto seed = array_from_hex<32>(seed_hex);
        const Bytes message = message_hex == "-" ? Bytes{} : bytes_from_hex(message_hex);
        const auto key = array_from_hex<32>(key_hex);
        const auto signature = array_from_hex<64>(signature_hex);
        SW_CHECK(public_key_of(seed) == key);
        SW_CHECK(sign_of(seed, {message}) == signature);
        SW_CHECK(sp::ed25519_verify(key, signature, {message}));
        // Em duas partes (como o envelope) dá o mesmo.
        const std::span<const std::byte> all(message);
        SW_CHECK(sign_of(seed, {all.first(message.size() / 2), all.subspan(message.size() / 2)}) ==
                 signature);
        ++cases;
    }
    SW_CHECK(cases == 96);
}

void test_ed25519_strict_verification()
{
    const sp::Ed25519Seed seed = test_seed();
    const auto pk = public_key_of(seed);
    const Bytes msg = bytes_from_hex("00112233445566778899aabbccddeeff");
    const auto sig = sign_of(seed, {msg});
    SW_CHECK(sp::ed25519_verify(pk, sig, {msg}));

    // Qualquer bit da assinatura, da chave ou da mensagem muda o resultado.
    for (std::size_t bit = 0; bit < 64 * 8; bit += 7) {
        auto bad = sig;
        bad[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
        SW_CHECK(!sp::ed25519_verify(pk, bad, {msg}));
    }
    for (std::size_t bit = 0; bit < 32 * 8; bit += 9) {
        auto bad = pk;
        bad[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
        SW_CHECK(!sp::ed25519_verify(bad, sig, {msg}));
    }
    Bytes other = msg;
    other[3] ^= std::byte{1};
    SW_CHECK(!sp::ed25519_verify(pk, sig, {other}));

    // Maleabilidade: S + L tem os mesmos pontos, mas S >= L tem de ser recusado (RFC 8032 §5.1.7).
    const std::uint8_t l_bytes[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                                      0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                      0,    0,    0,    0,    0,    0,    0,    0,
                                      0,    0,    0,    0,    0,    0,    0,    0x10};
    auto malleable = sig;
    unsigned carry = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const unsigned sum = malleable[32 + i] + l_bytes[i] + carry;
        malleable[32 + i] = static_cast<std::uint8_t>(sum);
        carry = sum >> 8;
    }
    if (carry == 0) { // S + L ainda cabe em 256 bits
        SW_CHECK(!sp::ed25519_verify(pk, malleable, {msg}));
    }
}

// ---------------------------------------------------------------- vetores do JSON

void test_envelope_vectors(const Json& envelope)
{
    const auto seed = array_from_hex<32>(envelope.at("test_key_seed_hex").as_string());
    SW_CHECK(seed == test_seed());
    const auto pk = array_from_hex<32>(envelope.at("test_public_key_hex").as_string());
    SW_CHECK(public_key_of(seed) == pk);
    SW_CHECK(hex_of(sp::key_id_of(pk)) == envelope.at("key_id_hex").as_string());
    const std::array<sp::Ed25519PublicKey, 1> trusted = {pk};
    const sp::ReadOptions options = trusting(trusted);

    // O envelope de referência: a assinatura bate byte a byte com a do vetor (OpenSSL a
    // reproduz), e o manifest_id é o SHA-256 do arquivo inteiro.
    const Bytes manifest = bytes_from_hex(envelope.at("manifest_hex").as_string());
    const Bytes msg = bytes_from_hex(envelope.at("msg_hex").as_string());
    const auto signature = array_from_hex<64>(envelope.at("signature_hex").as_string());
    SW_CHECK(sign_of(seed, {msg}) == signature);
    SW_CHECK(sp::ed25519_verify(pk, signature, {msg}));
    SW_CHECK(hex_of(sp::sha256(manifest)) == envelope.at("manifest_id_hex").as_string());
    // O payload desse vetor é opaco (não é FlatBuffers): a assinatura passa e o leitor o
    // recusa depois, na sondagem, como inválido. Nunca como assinatura ruim.
    {
        const sp::ManifestStatus s = open_bytes(manifest, options);
        SW_CHECK(s.code == sp::ManifestError::Invalid);
    }

    // Casos que o leitor DEVE recusar, com o erro certo.
    for (const Json& c : envelope.at("reject_cases").items) {
        const std::string& label = c.at("label").as_string();
        const std::string& expect = c.at("expect").as_string();
        const sp::ManifestStatus s =
            open_bytes(bytes_from_hex(c.at("manifest_hex").as_string()), options);
        sp::ManifestError want = sp::ManifestError::Ok;
        if (expect == "signature" || expect == "key") {
            want = sp::ManifestError::SignatureInvalid;
        } else if (expect == "header") {
            want = sp::ManifestError::Invalid;
        } else if (expect == "unsupported") {
            want = sp::ManifestError::Unsupported;
        }
        if (s.code != want) {
            std::fprintf(stderr, "caso '%s': esperava %s, veio %s (%s)\n", label.c_str(),
                         sp::to_string(want), sp::to_string(s.code), s.detail);
        }
        SW_CHECK(want != sp::ManifestError::Ok && s.code == want);
    }

    // Sondagem do min_reader (§7.1, passo 4): o leitor v0 (revisão 0).
    const Json& probe = envelope.at("payload_probe");
    SW_CHECK(probe.at("supported_payload_revision").as_size() == sp::kSupportedPayloadRevision);
    std::size_t probes = 0;
    for (const Json& c : probe.at("cases").items) {
        const std::string& label = c.at("label").as_string();
        const std::string& expect = c.at("expect").as_string();
        const Bytes payload = bytes_from_hex(c.at("payload_hex").as_string());
        std::uint16_t min_reader = 99;
        const sp::ManifestStatus s = sp::probe_min_reader(payload, min_reader);
        if (expect == "ok") {
            SW_CHECK(s.ok());
            SW_CHECK(min_reader == c.at("min_reader").as_size());
        } else if (expect == "unsupported") {
            SW_CHECK(s.code == sp::ManifestError::Unsupported);
        } else {
            SW_CHECK(expect == "invalid" && s.code == sp::ManifestError::Invalid);
        }
        // Pelo leitor inteiro: "unsupported" e "invalid" saem como no vetor, depois da
        // assinatura; "ok" passa da sondagem e cai na verificação do payload mínimo (inválido).
        const sp::ManifestStatus whole =
            open_bytes(bytes_from_hex(c.at("manifest_hex").as_string()), options);
        if (expect == "unsupported") {
            SW_CHECK(whole.code == sp::ManifestError::Unsupported);
        } else {
            SW_CHECK(whole.code == sp::ManifestError::Invalid);
        }
        (void)label;
        ++probes;
    }
    SW_CHECK(probes >= 7);
}

// ---------------------------------------------------------------- ida e volta

void check_view_matches(const sp::ManifestModel& model, const sp::ManifestView& view)
{
    SW_CHECK(view.game_id() == model.game_id);
    SW_CHECK(view.build_id() == model.build_id);
    SW_CHECK(view.build_seq() == model.build_seq);
    SW_CHECK(view.created_at() == model.created_at);
    SW_CHECK(view.block_count() == model.block_hash.size());
    SW_CHECK(view.piece_count() == model.piece_hash.size());
    SW_CHECK(view.file_count() == model.file_path.size());
    SW_CHECK(view.group_count() == model.groups.size());
    SW_CHECK(view.block_ref_count() == model.block_refs.size());
    for (std::size_t i = 0; i < view.block_count(); ++i) {
        SW_CHECK(view.block_hash(i) == model.block_hash[i]);
        SW_CHECK(view.block_raw_size(i) == model.block_raw_size[i]);
        SW_CHECK(view.block_packed_size(i) == model.block_packed_size[i]);
        SW_CHECK(view.block_codec(i) == model.block_codec[i]);
    }
    for (std::size_t i = 0; i < view.piece_count(); ++i) {
        SW_CHECK(view.piece_hash(i) == model.piece_hash[i]);
        SW_CHECK(view.piece_size(i) == model.piece_size[i]);
        SW_CHECK(view.piece_first_block(i) == model.piece_first_block[i]);
        SW_CHECK(view.piece_block_count(i) == model.piece_block_count[i]);
        SW_CHECK(view.piece_group(i) == model.piece_group[i]);
    }
    for (std::size_t i = 0; i < view.block_ref_count(); ++i) {
        SW_CHECK(view.block_ref(i) == model.block_refs[i]);
    }
    for (std::size_t i = 0; i < view.file_count(); ++i) {
        SW_CHECK(view.file_path(i) == model.file_path[i]);
        SW_CHECK(view.file_size(i) == model.file_size[i]);
        SW_CHECK(view.file_class(i) == model.file_class[i]);
        SW_CHECK(view.file_group(i) == model.file_group[i]);
        SW_CHECK(view.file_first_ref(i) == model.file_first_ref[i]);
        SW_CHECK(view.file_ref_count(i) == model.file_ref_count[i]);
    }
}

void check_round_trip(const sp::ManifestModel& model)
{
    const sp::Ed25519Seed seed = test_seed();
    const std::array<sp::Ed25519PublicKey, 1> trusted = {public_key_of(seed)};

    const sp::BuildResult built = sp::build_manifest(model, seed);
    if (!built.status.ok()) {
        std::fprintf(stderr, "build_manifest falhou: %s (%s)\n", sp::to_string(built.status.code),
                     built.status.detail);
    }
    SW_CHECK(built.status.ok() && !built.bytes.empty());

    sp::ManifestView view;
    SW_CHECK(open_bytes(built.bytes, trusting(trusted), &view).ok());
    SW_CHECK(view.to_model() == model); // lido de volta é idêntico
    check_view_matches(model, view);
    SW_CHECK(view.key_id() == sp::key_id_of(trusted[0]));
    SW_CHECK(view.manifest_id() == sp::sha256(built.bytes));
    SW_CHECK(view.payload_size() + sp::kEnvelopeSize == built.bytes.size());

    // Determinístico: o mesmo modelo dá os mesmos bytes, e reescrever o que foi lido também.
    SW_CHECK(sp::build_manifest(model, seed).bytes == built.bytes);
    SW_CHECK(sp::encode_manifest(view.to_model(), seed) == built.bytes);

    // O envelope tem o layout do §7.1.
    SW_CHECK(std::memcmp(built.bytes.data(), "SWMANIF\0", 8) == 0);
    const auto* b = reinterpret_cast<const std::uint8_t*>(built.bytes.data());
    SW_CHECK(b[8] == 0 && b[9] == 0 && b[10] == 1 && b[11] == 1);
    std::uint64_t payload_len = 0;
    for (int i = 7; i >= 0; --i) {
        payload_len = (payload_len << 8) | b[16 + i];
    }
    SW_CHECK(payload_len == built.bytes.size() - sp::kEnvelopeSize);
    SW_CHECK(std::memcmp(b + 24, view.key_id().data(), 8) == 0);
    // O payload é FlatBuffers com o identificador "SWM0" (a sondagem do min_reader depende disso).
    SW_CHECK(std::memcmp(b + sp::kEnvelopeSize + 4, "SWM0", 4) == 0);
}

void test_round_trips()
{
    // Vazio: só o grupo core, sem arquivo nem bloco.
    sp::ManifestModel empty;
    empty.game_id = "vazio";
    empty.build_id = "0";
    empty.groups.push_back({"core", 0, 0, 0, {}});
    check_round_trip(empty);

    // Um arquivo vazio (0 blocos) é válido (§1).
    sp::ManifestModel with_empty_file = empty;
    with_empty_file.file_path = {"a/vazio.txt"};
    with_empty_file.file_size = {0};
    with_empty_file.file_class = {sp::FileClass::Core};
    with_empty_file.file_group = {0};
    with_empty_file.file_first_ref = {0};
    with_empty_file.file_ref_count = {0};
    check_round_trip(with_empty_file);

    // Sintéticos de vários tamanhos, com todos os campos preenchidos.
    for (const std::size_t blocks : {1U, 2U, 9U, 60U, 500U, 6000U}) {
        sp::ManifestModel m = st::make_synthetic_model(blocks, 6, blocks);
        st::SplitMix64 rng(blocks);
        m.torrent_infohashes = {st::random_hash(rng), st::random_hash(rng), st::random_hash(rng)};
        check_round_trip(m);
    }

    // Valores nos limites dos tipos: build_seq e created_at enormes/negativos, texto UTF-8.
    sp::ManifestModel m = st::make_synthetic_model(40, 4, 5);
    m.build_seq = 0xFFFFFFFFFFFFFFFFULL;
    m.created_at = -1;
    m.build_id = "1.3.0+4521 \xC3\xA7\xE2\x82\xAC";
    m.file_path[0] = "a_core/a\xC3\xA7/arquivo \xE2\x82\xAC.ctex"; // ainda ordenado: "a" < "d"
    check_round_trip(m);
}

// ---------------------------------------------------------------- adulteração

void test_tamper_every_byte()
{
    const sp::Ed25519Seed seed = test_seed();
    const std::array<sp::Ed25519PublicKey, 1> trusted = {public_key_of(seed)};
    const sp::ReadOptions options = trusting(trusted);
    const sp::ManifestModel model = st::make_synthetic_model(3, 2, 3);
    const Bytes good = sp::build_manifest(model, seed).bytes;
    SW_CHECK(!good.empty() && open_bytes(good, options).ok());

    // Os três casos exigidos pela spec (03 §12) e mais: um bit de CADA byte do arquivo
    // (a posição do bit varia com o byte) tem de fazer o leitor recusar.
    for (std::size_t i = 0; i < good.size(); ++i) {
        Bytes bad = good;
        bad[i] ^= static_cast<std::byte>(1U << (i % 8));
        const sp::ManifestStatus s = open_bytes(bad, options);
        if (s.ok()) {
            std::fprintf(stderr, "byte %zu adulterado e aceito\n", i);
        }
        SW_CHECK(!s.ok());
    }
    // O byte do payload e o do hash de bloco, explicitamente, com o erro de assinatura.
    {
        Bytes bad = good;
        bad[good.size() - 1] ^= std::byte{0x80};
        SW_CHECK(open_bytes(bad, options).code == sp::ManifestError::SignatureInvalid);
        bad = good;
        sp::ManifestView view;
        SW_CHECK(open_bytes(good, options, &view).ok());
        const std::size_t hash_at = sp::kEnvelopeSize + 120;
        bad[hash_at] ^= std::byte{1};
        SW_CHECK(open_bytes(bad, options).code == sp::ManifestError::SignatureInvalid);
        // Assinatura trocada por outra válida, de outra mensagem.
        const Bytes other = sp::build_manifest(st::make_synthetic_model(3, 2, 4), seed).bytes;
        bad = good;
        std::copy(other.begin() + 40, other.begin() + 104, bad.begin() + 40);
        SW_CHECK(open_bytes(bad, options).code == sp::ManifestError::SignatureInvalid);
    }
    // Truncado em qualquer tamanho, e com bytes a mais.
    for (std::size_t n = 0; n < good.size(); n += (n < 130 ? 1 : 97)) {
        const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        SW_CHECK(!open_bytes(cut, options).ok());
    }
    {
        Bytes longer = good;
        longer.push_back(std::byte{0});
        SW_CHECK(open_bytes(longer, options).code == sp::ManifestError::Invalid);
    }
    // Assinado por outra chave: a chave não é confiável.
    {
        const Bytes foreign = sp::build_manifest(model, other_seed()).bytes;
        SW_CHECK(open_bytes(foreign, options).code == sp::ManifestError::SignatureInvalid);
        const std::array<sp::Ed25519PublicKey, 1> foreign_key = {public_key_of(other_seed())};
        SW_CHECK(open_bytes(foreign, trusting(foreign_key)).ok());
        // Sem nenhuma chave confiável, nada passa.
        SW_CHECK(open_bytes(good, sp::ReadOptions{}).code == sp::ManifestError::SignatureInvalid);
    }
}

struct Mutation
{
    const char* name;
    sp::ManifestError error;
    const char* detail; // trecho do detalhe esperado (isola a regra); nullptr = qualquer
    std::function<void(sp::ManifestModel&)> apply;
};

void test_semantic_rejections()
{
    const sp::Ed25519Seed seed = test_seed();
    const std::array<sp::Ed25519PublicKey, 1> trusted = {public_key_of(seed)};
    const sp::ReadOptions options = trusting(trusted);
    sp::ManifestModel base = st::make_synthetic_model(60, 4, 11);
    SW_CHECK(sp::build_manifest(base, seed).status.ok());
    SW_CHECK(base.groups.size() == 3 && base.file_path.size() >= 6);

    const auto last_block = static_cast<std::uint32_t>(base.block_hash.size() - 1);
    using E = sp::ManifestError;
    const std::vector<Mutation> mutations = {
        {"game_id com maiuscula", E::Invalid, "game_id invalido",
         [](auto& m) { m.game_id = "Jogo"; }},
        {"game_id vazio", E::Invalid, "game_id invalido", [](auto& m) { m.game_id.clear(); }},
        {"game_id de 65 bytes", E::Invalid, "game_id invalido",
         [](auto& m) { m.game_id.assign(65, 'a'); }},
        {"grupo 0 nao e core", E::Invalid, "core", [](auto& m) { m.groups[0].name = "base"; }},
        {"nome de grupo repetido", E::Invalid, "nome de grupo",
         [](auto& m) { m.groups[2].name = m.groups[1].name; }},
        {"nome de grupo invalido", E::Invalid, "nome de grupo",
         [](auto& m) { m.groups[1].name = "Regiao Norte"; }},
        {"nome de grupo longo", E::Invalid, "nome de grupo",
         [](auto& m) { m.groups[1].name.assign(49, 'a'); }},
        {"deps nao vazio", E::Invalid, "deps", [](auto& m) { m.groups[1].deps = {0}; }},
        {"sem grupos", E::Invalid, "sem grupos", [](auto& m) { m.groups.clear(); }},
        {"coluna de Blocks mais curta", E::Invalid, "Blocks",
         [](auto& m) { m.block_raw_size.pop_back(); }},
        {"coluna de Pieces mais curta", E::Invalid, "Pieces",
         [](auto& m) { m.piece_group.pop_back(); }},
        {"coluna de Files mais curta", E::Invalid, "Files",
         [](auto& m) { m.file_size.pop_back(); }},
        {"infohashes com tamanho errado", E::Invalid, "torrent_infohashes",
         [](auto& m) { m.torrent_infohashes.resize(2); }},
        {"cdn sem https", E::Invalid, "cdn_base_url",
         [](auto& m) { m.cdn_base_urls = {"http://cdn.example.test"}; }},
        {"cdn com barra final", E::Invalid, "cdn_base_url",
         [](auto& m) { m.cdn_base_urls = {"https://cdn.example.test/"}; }},
        {"raw_size zero", E::Invalid, "bloco", [](auto& m) { m.block_raw_size[0] = 0; }},
        {"raw_size acima de 256 KiB", E::Invalid, "bloco",
         [](auto& m) { m.block_raw_size[0] = 262145; }},
        {"codec None com packed diferente", E::Invalid, "bloco",
         [](auto& m) { m.block_codec[0] = sp::Codec::None; }},
        {"codec desconhecido", E::Invalid, "bloco",
         [](auto& m) { m.block_codec[0] = static_cast<sp::Codec>(3); }},
        {"lacuna entre pecas", E::Invalid, "lacuna", [](auto& m) { m.piece_first_block[1] += 1; }},
        {"peca com size errado", E::Invalid, "size da peca", [](auto& m) { m.piece_size[0] += 1; }},
        {"peca com size zero", E::Invalid, nullptr, [](auto& m) { m.piece_size[0] = 0; }},
        {"peca com size acima de 1 MiB", E::Invalid, nullptr,
         [](auto& m) { m.piece_size[0] = 1048577; }},
        {"peca com 0 blocos", E::Invalid, nullptr, [](auto& m) { m.piece_block_count[0] = 0; }},
        {"grupo da peca fora de groups", E::Invalid, "grupo de peca",
         [](auto& m) { m.piece_group[0] = 9; }},
        {"peca no intervalo de outro grupo", E::Invalid, nullptr,
         [](auto& m) { m.piece_group[1] = 2; }},
        {"piece_first do grupo deslocado", E::Invalid, nullptr,
         [](auto& m) { m.groups[1].piece_first += 1; }},
        {"piece_count do grupo maior", E::Invalid, nullptr,
         [](auto& m) { m.groups[2].piece_count += 1; }},
        {"caminho com ..", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core/../x"; }},
        {"caminho com segmento vazio", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core//x"; }},
        {"caminho comecando com /", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "/a_core/x"; }},
        {"caminho terminando com /", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core/x/"; }},
        {"caminho com barra invertida", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core\\x"; }},
        {"caminho com NUL", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = std::string("a_core/x\0y", 10); }},
        {"caminho com UTF-8 invalido", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core/\xC3(x"; }},
        {"caminho com UTF-8 longo demais (overlong)", E::Invalid, "caminho invalido",
         [](auto& m) { m.file_path[1] = "a_core/\xC0\xAF"; }},
        {"caminho de 1025 bytes", E::Invalid, nullptr,
         [](auto& m) { m.file_path[1] = "a_core/" + std::string(1020, 'x'); }},
        {"caminhos fora de ordem", E::Invalid, "ordem",
         [](auto& m) { std::swap(m.file_path[0], m.file_path[1]); }},
        {"caminhos repetidos", E::Invalid, "ordem",
         [](auto& m) { m.file_path[1] = m.file_path[0]; }},
        {"caminhos que colidem ignorando maiusculas", E::Invalid, "maiusculas",
         [](auto& m) {
             m.file_path[0] = "a_core/X";
             m.file_path[1] = "a_core/x";
         }},
        {"CORE com grupo diferente de 0", E::Invalid, "class e group",
         [](auto& m) { m.file_class[0] = sp::FileClass::Group; }},
        {"GROUP com grupo 0", E::Invalid, "class e group",
         [&](auto& m) { m.file_group[m.file_group.size() - 1] = 0; }},
        {"class desconhecida", E::Invalid, "class e group",
         [](auto& m) { m.file_class[0] = static_cast<sp::FileClass>(3); }},
        {"arquivo com grupo fora de groups", E::Invalid, "class e group",
         [](auto& m) { m.file_group[0] = 9; }},
        {"first_ref fora da particao", E::Invalid, "first_ref",
         [](auto& m) { m.file_first_ref[1] += 1; }},
        {"ref_count passa do fim", E::Invalid, "ref_count",
         [](auto& m) { m.file_ref_count.back() += 1000; }},
        {"block_refs fora de Blocks", E::Invalid, "fora de Blocks",
         [](auto& m) { m.block_refs[0] = 9999; }},
        {"block_refs sobrando", E::Invalid, "nenhum arquivo",
         [](auto& m) { m.block_refs.push_back(0); }},
        {"size do arquivo errado", E::Invalid, "size do arquivo",
         [](auto& m) { m.file_size[0] += 1; }},
        {"fechamento: arquivo do grupo 1 usa bloco do grupo 2", E::Invalid, "outro grupo",
         [&](auto& m) {
             // O primeiro arquivo do grupo 1 passa a apontar para um bloco do grupo 2.
             for (std::size_t i = 0; i < m.file_group.size(); ++i) {
                 if (m.file_group[i] == 1) {
                     m.block_refs[m.file_first_ref[i]] = last_block;
                     return;
                 }
             }
         }},
        {"fechamento: arquivo CORE usa bloco de grupo", E::Invalid, "outro grupo",
         [&](auto& m) { m.block_refs[0] = last_block; }},
        {"cipher_id diferente de 0", E::Unsupported, "cipher_id", [](auto& m) { m.cipher_id = 1; }},
        {"min_reader maior que o suportado", E::Unsupported, "min_reader",
         [](auto& m) { m.min_reader = 1; }},
    };

    for (const Mutation& mutation : mutations) {
        sp::ManifestModel m = base;
        mutation.apply(m);
        // Assinado com a chave certa: só as validações do §8 podem recusar.
        const Bytes bytes = sp::encode_manifest(m, seed);
        SW_CHECK(!bytes.empty());
        const sp::ManifestStatus s = open_bytes(bytes, options);
        if (s.code != mutation.error ||
            (mutation.detail != nullptr && std::strstr(s.detail, mutation.detail) == nullptr)) {
            std::fprintf(stderr, "mutacao '%s': esperava %s [%s], veio %s (%s)\n", mutation.name,
                         sp::to_string(mutation.error),
                         mutation.detail != nullptr ? mutation.detail : "-", sp::to_string(s.code),
                         s.detail);
        }
        SW_CHECK(s.code == mutation.error);
        if (mutation.detail != nullptr) {
            SW_CHECK(std::strstr(s.detail, mutation.detail) != nullptr);
        }
        // O construtor também se recusa a produzir isso.
        SW_CHECK(!sp::build_manifest(m, seed).status.ok());
    }

    // Anti-rollback (item 10): build_seq menor que o instalado; igual e maior passam.
    {
        const Bytes bytes = sp::build_manifest(base, seed).bytes;
        sp::ReadOptions rollback = options;
        rollback.installed_build_seq = base.build_seq + 1;
        SW_CHECK(open_bytes(bytes, rollback).code == sp::ManifestError::Rollback);
        rollback.installed_build_seq = base.build_seq;
        SW_CHECK(open_bytes(bytes, rollback).ok());
        rollback.installed_build_seq = 0;
        SW_CHECK(open_bytes(bytes, rollback).ok());
    }

    // Limites de sanidade (item 9): mais de 4096 grupos é recusado antes de qualquer laço.
    {
        sp::ManifestModel m = base;
        m.groups.resize(4097, sp::GroupInfo{"x", 0, 0, 0, {}});
        const Bytes bytes = sp::encode_manifest(m, seed);
        const sp::ManifestStatus s = open_bytes(bytes, options);
        SW_CHECK(s.code == sp::ManifestError::Invalid &&
                 std::strstr(s.detail, "limite") != nullptr);
    }
}

// Payload malformado com assinatura VÁLIDA (um build com defeito, ou quem tem a chave): o
// leitor devolve erro, nunca lê fora do buffer (o CI roda isto sob ASan/UBSan).
void test_signed_garbage_payloads()
{
    const sp::Ed25519Seed seed = test_seed();
    const std::array<sp::Ed25519PublicKey, 1> trusted = {public_key_of(seed)};
    const sp::ReadOptions options = trusting(trusted);
    const Bytes good = sp::build_manifest(st::make_synthetic_model(12, 3, 2), seed).bytes;
    const auto* base = reinterpret_cast<const std::uint8_t*>(good.data()) + sp::kEnvelopeSize;
    const std::vector<std::uint8_t> payload(base, good.size() - sp::kEnvelopeSize + base);

    st::SplitMix64 rng(0xC0FFEE);
    std::size_t accepted = 0;
    constexpr int kIterations = 300;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        std::vector<std::uint8_t> mutated = payload;
        const unsigned flips = 1 + rng.between(0, 3);
        for (unsigned f = 0; f < flips; ++f) {
            // Metade das mutações cai nos primeiros 256 bytes (raiz, vtables, offsets).
            const std::size_t at =
                rng.between(0, 1) == 0
                    ? rng.between(0, static_cast<std::uint32_t>(
                                         std::min<std::size_t>(255, mutated.size() - 1)))
                    : rng.between(0, static_cast<std::uint32_t>(mutated.size() - 1));
            mutated[at] = static_cast<std::uint8_t>(rng.next());
        }
        if (iteration % 50 == 49) { // e payloads truncados
            mutated.resize(rng.between(0, static_cast<std::uint32_t>(mutated.size())));
        }
        const Bytes signed_bytes = sp::sign_payload(mutated, seed);
        sp::ManifestView view;
        const sp::ManifestStatus s = open_bytes(signed_bytes, options, &view);
        if (s.ok()) {
            // Aceito: tem de ser coerente (ex.: mutação num hash ou num tamanho que ainda soma).
            const sp::ManifestModel m = view.to_model();
            SW_CHECK(sp::encode_manifest(m, seed).size() > sp::kEnvelopeSize);
            ++accepted;
        } else {
            SW_CHECK(s.code == sp::ManifestError::Invalid ||
                     s.code == sp::ManifestError::Unsupported);
        }
    }
    std::printf("  payloads corrompidos: %d, aceitos (mutacao inofensiva): %zu\n", kIterations,
                accepted);
    SW_CHECK(accepted < kIterations);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <vectors-v0.json>\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "nao consegui abrir %s\n", argv[1]);
        return 2;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const Json document = JsonParser(buffer.str()).parse();

    auto t0 = std::chrono::steady_clock::now();
    const auto lap = [&](const char* what) {
        const auto now = std::chrono::steady_clock::now();
        std::printf("%s (%.1f s)\n", what, std::chrono::duration<double>(now - t0).count());
        t0 = now;
    };
    test_ed25519_rfc8032();
    test_ed25519_openssl_vectors(argv[2]);
    test_ed25519_strict_verification();
    lap("ed25519 ok");
    test_envelope_vectors(document.at("envelope"));
    lap("vetores de envelope ok");
    test_round_trips();
    lap("ida e volta ok");
    test_tamper_every_byte();
    lap("adulteracao ok");
    test_semantic_rejections();
    lap("validacoes do 03 secao 8 ok");
    test_signed_garbage_payloads();
    std::puts("TODOS OS TESTES DO MANIFESTO PASSARAM");
    return 0;
}
