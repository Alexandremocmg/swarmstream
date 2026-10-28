// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Leitor do manifesto v0 (docs/03 §7 e §8, ADR-0012): a ÚNICA implementação. O `sw-pack`
// (`build_manifest`) a reutiliza para reler o que gerou; nada aqui escreve manifesto.

#include "swarmstream/manifest_view.hpp"

#include "swarmstream/crypto.hpp"
#include "swarmstream/manifest_wire.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_set>

namespace swarmstream::pack {

namespace {

std::uint16_t rd16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t rd32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

const std::uint8_t* raw(std::span<const std::byte> bytes) noexcept
{
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

ManifestStatus fail(ManifestError code, const char* detail) noexcept
{
    return ManifestStatus{code, detail};
}

// ================================================================ leitor de FlatBuffers
//
// Acesso com limites conferidos. Qualquer violação marca `bad` e devolve 0; quem chama confere
// `bad` antes de usar o que leu. Também exige o alinhamento natural, como o Verifier oficial.
struct Fb
{
    const std::uint8_t* p;
    std::size_t n;
    bool bad = false;

    std::uint32_t u32(std::size_t off) noexcept
    {
        if (off > n || n - off < 4) {
            bad = true;
            return 0;
        }
        return rd32(p + off);
    }
    std::uint16_t u16(std::size_t off) noexcept
    {
        if (off > n || n - off < 2) {
            bad = true;
            return 0;
        }
        return rd16(p + off);
    }
    std::uint8_t u8(std::size_t off) noexcept
    {
        if (off >= n) {
            bad = true;
            return 0;
        }
        return p[off];
    }
    std::uint64_t u64(std::size_t off) noexcept
    {
        if (off > n || n - off < 8) {
            bad = true;
            return 0;
        }
        return rd64(p + off);
    }

    struct Tbl
    {
        std::size_t pos = 0;
        std::size_t vt = 0;
        std::size_t vsize = 0;
        std::size_t tsize = 0;
    };

    // Confere a tabela em `pos` e o seu vtable.
    bool table(std::size_t pos, Tbl& t) noexcept
    {
        if (pos % 4 != 0) {
            bad = true;
            return false;
        }
        const auto soffset = static_cast<std::int32_t>(u32(pos));
        const std::int64_t vt = static_cast<std::int64_t>(pos) - soffset;
        if (bad || vt < 0 || vt % 2 != 0 || static_cast<std::uint64_t>(vt) + 4 > n) {
            bad = true;
            return false;
        }
        t.pos = pos;
        t.vt = static_cast<std::size_t>(vt);
        t.vsize = u16(t.vt);
        t.tsize = u16(t.vt + 2);
        if (bad || t.vsize < 4 || t.vsize % 2 != 0 || t.vt + t.vsize > n || t.tsize < 4 ||
            t.pos + t.tsize > n) {
            bad = true;
            return false;
        }
        return true;
    }

    // Posição absoluta do campo `id` (0 se ausente). `size` é também o alinhamento exigido.
    std::size_t field(const Tbl& t, unsigned id, std::size_t size) noexcept
    {
        const std::size_t slot = 4 + 2 * static_cast<std::size_t>(id);
        if (slot + 2 > t.vsize) {
            return 0;
        }
        const std::size_t fo = u16(t.vt + slot);
        if (fo == 0) {
            return 0;
        }
        if (fo < 4 || fo + size > t.tsize || (t.pos + fo) % size != 0) {
            bad = true;
            return 0;
        }
        return t.pos + fo;
    }

    // Segue o uoffset que está em `slot`; o alvo fica em [slot, n).
    std::size_t follow(std::size_t slot) noexcept
    {
        const std::uint32_t off = u32(slot);
        if (bad || off == 0 || off > n - slot) {
            bad = true;
            return 0;
        }
        return slot + off;
    }

    // Tabela apontada pelo campo de referência `id` de `parent`; false se ausente.
    bool sub_table(const Tbl& parent, unsigned id, Tbl& out) noexcept
    {
        const std::size_t slot = field(parent, id, 4);
        if (slot == 0 || bad) {
            return false;
        }
        const std::size_t target = follow(slot);
        return !bad && table(target, out);
    }

    // Vetor de `elem_size` bytes por elemento (alinhado a `align`); posição e contagem.
    bool vector(const Tbl& parent, unsigned id, std::size_t elem_size, std::size_t align,
                std::size_t& first, std::size_t& count) noexcept
    {
        first = 0;
        count = 0;
        const std::size_t slot = field(parent, id, 4);
        if (slot == 0 || bad) {
            return !bad;
        }
        const std::size_t target = follow(slot);
        if (bad || target % 4 != 0) {
            bad = true;
            return false;
        }
        const std::size_t len = u32(target);
        const std::size_t start = target + 4;
        if (bad || start > n || len > (n - start) / elem_size || start % align != 0) {
            bad = true;
            return false;
        }
        first = start;
        count = len;
        return true;
    }

    // String apontada pelo uoffset em `slot`: [len][bytes][NUL], len <= kMaxStringLen.
    std::string_view string(std::size_t slot) noexcept
    {
        const std::size_t target = follow(slot);
        if (bad || target % 4 != 0) {
            bad = true;
            return {};
        }
        const std::size_t len = u32(target);
        if (bad || len > kMaxStringLen || target + 4 + len + 1 > n || p[target + 4 + len] != 0) {
            bad = true;
            return {};
        }
        return {reinterpret_cast<const char*>(p + target + 4), len};
    }

    // String no campo `id` (vazia se ausente).
    std::string_view string_field(const Tbl& t, unsigned id) noexcept
    {
        const std::size_t slot = field(t, id, 4);
        if (slot == 0 || bad) {
            return {};
        }
        return string(slot);
    }
};

// ================================================================ validações de texto

bool valid_utf8(std::string_view s) noexcept
{
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t min = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            code = c & 0x1F;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            code = c & 0x0F;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            code = c & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        if (s.size() - i <= extra) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<std::uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (cc & 0x3F);
        }
        if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// docs/03 §6. A normalização NFC não é conferida aqui (precisaria das tabelas do Unicode).
bool valid_path(std::string_view path) noexcept
{
    if (path.empty() || path.size() > kMaxStringLen || !valid_utf8(path)) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (segment.find('\0') != std::string_view::npos ||
            segment.find('\\') != std::string_view::npos) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        start = slash + 1;
    }
}

bool valid_group_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 48) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool valid_game_id(std::string_view id) noexcept
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
}

std::string ascii_lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

// ================================================================ leitor do manifesto

class ManifestReader
{
public:
    static ManifestStatus open(std::span<const std::byte> bytes, const ReadOptions& options,
                               ManifestView& out);

private:
    static ManifestStatus parse(ManifestView& v);
    static ManifestStatus validate(const ManifestView& v, const ReadOptions& options);
};

const char* to_string(ManifestError error) noexcept
{
    switch (error) {
    case ManifestError::Ok: return "ok";
    case ManifestError::Invalid: return "SW_E_MANIFEST_INVALID";
    case ManifestError::Unsupported: return "SW_E_MANIFEST_UNSUPPORTED";
    case ManifestError::SignatureInvalid: return "SW_E_SIGNATURE_INVALID";
    case ManifestError::Rollback: return "SW_E_MANIFEST_ROLLBACK";
    }
    return "?";
}

KeyId key_id_of(const Ed25519PublicKey& public_key) noexcept
{
    const Sha256Digest digest = sha256(std::as_bytes(std::span<const std::uint8_t>(public_key)));
    KeyId id{};
    std::copy_n(digest.begin(), id.size(), id.begin());
    return id;
}

ManifestStatus probe_min_reader(std::span<const std::byte> payload,
                                std::uint16_t& min_reader) noexcept
{
    // Algoritmo do §7.1, passo 4, na mesma ordem e com os mesmos limites.
    const std::uint8_t* const p = raw(payload);
    const std::uint64_t n = payload.size();
    const auto invalid = [] { return fail(ManifestError::Invalid, "sondagem do min_reader"); };
    min_reader = 0;
    if (n < 8 || std::memcmp(p + 4, kFileIdentifier.data(), 4) != 0) {
        return invalid();
    }
    const std::uint64_t r = rd32(p);
    if (r < 8 || r % 4 != 0 || r + 4 > n) {
        return invalid();
    }
    const std::int64_t v = static_cast<std::int64_t>(r) - static_cast<std::int32_t>(rd32(p + r));
    if (v < 0 || v % 2 != 0 || static_cast<std::uint64_t>(v) + 4 > n) {
        return invalid();
    }
    const auto vs = static_cast<std::uint64_t>(rd16(p + v));
    const auto ts = static_cast<std::uint64_t>(rd16(p + v + 2));
    if (vs < 4 || vs % 2 != 0 || static_cast<std::uint64_t>(v) + vs > n || ts < 4 || r + ts > n) {
        return invalid();
    }
    const std::uint64_t slot = 4 + 2 * kMinReaderId;
    const std::uint64_t fo = (slot + 2 <= vs) ? rd16(p + v + slot) : 0;
    std::uint16_t value = 0;
    if (fo != 0) {
        if (fo < 4 || fo + 2 > ts) {
            return invalid();
        }
        value = rd16(p + r + fo);
    }
    min_reader = value;
    if (value > kSupportedPayloadRevision) {
        return fail(ManifestError::Unsupported, "payload exige um leitor mais novo (min_reader)");
    }
    return {};
}

ManifestStatus ManifestReader::open(std::span<const std::byte> bytes, const ReadOptions& options,
                                    ManifestView& out)
{
    const std::uint8_t* const p = raw(bytes);

    // Passo 1: cabeçalho, nesta ordem (a), (b), (c).
    if (bytes.size() < kEnvelopeSize || std::memcmp(p, kMagic.data(), kMagic.size()) != 0) {
        return fail(ManifestError::Invalid, "magic errado ou arquivo curto");
    }
    if (rd16(p + 8) != 0) {
        // Não autenticado: não deve provocar nenhuma ação automática (docs/03 §7.1).
        return fail(ManifestError::Unsupported, "format_version desconhecida");
    }
    const std::uint64_t payload_len = rd64(p + 16);
    if (p[10] != 1 || p[11] != 1 || rd32(p + 12) != 0 || rd64(p + 32) != 0) {
        return fail(ManifestError::Invalid, "hash_alg, sig_alg ou reservado invalido");
    }
    if (payload_len > kMaxPayloadLen || bytes.size() != kEnvelopeSize + payload_len) {
        return fail(ManifestError::Invalid, "payload_len nao bate com o tamanho do arquivo");
    }

    // O key_id e a assinatura precisam do libsodium: sem ele, nada é autenticado.
    if (!crypto_ready()) {
        return fail(ManifestError::SignatureInvalid, "libsodium nao iniciou");
    }

    // Passo 2: a chave pelo key_id.
    KeyId wanted{};
    std::memcpy(wanted.data(), p + 24, wanted.size());
    std::vector<const Ed25519PublicKey*> candidates;
    for (const Ed25519PublicKey& key : options.trusted_keys) {
        if (key_id_of(key) == wanted) {
            candidates.push_back(&key);
        }
    }
    if (candidates.empty()) {
        return fail(ManifestError::SignatureInvalid, "chave desconhecida (key_id)");
    }

    // Passo 3: a assinatura, ANTES de ler qualquer coisa do payload.
    Ed25519Signature signature{};
    std::memcpy(signature.data(), p + 40, signature.size());
    const std::span<const std::byte> payload_bytes = bytes.subspan(kEnvelopeSize);
    const auto as_span = [](std::string_view s) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    bool signed_ok = false;
    for (const Ed25519PublicKey* key : candidates) {
        if (ed25519_verify(
                *key, signature,
                {as_span(kSignaturePrefix), bytes.first(kSignedHeaderSize), payload_bytes})) {
            signed_ok = true;
            break;
        }
    }
    if (!signed_ok) {
        return fail(ManifestError::SignatureInvalid, "assinatura nao confere");
    }

    // Passo 4: sondagem do min_reader (autenticada).
    std::uint16_t min_reader = 0;
    if (const ManifestStatus probe = probe_min_reader(payload_bytes, min_reader); !probe.ok()) {
        return probe;
    }

    // Passo 5: verificador e validações semânticas.
    ManifestView view;
    view.payload_ = {p + kEnvelopeSize, static_cast<std::size_t>(payload_len)};
    view.payload_size_ = static_cast<std::size_t>(payload_len);
    view.key_id_ = wanted;
    view.min_reader_ = min_reader;
    {
        Sha256 hasher;
        hasher.update(bytes);
        view.manifest_id_ = hasher.finish();
    }
    if (const ManifestStatus s = parse(view); !s.ok()) {
        return s;
    }
    if (const ManifestStatus s = validate(view, options); !s.ok()) {
        return s;
    }
    out = std::move(view);
    return {};
}

ManifestStatus ManifestReader::parse(ManifestView& v)
{
    Fb fb{v.payload_.data(), v.payload_.size()};
    const auto bad = [] { return fail(ManifestError::Invalid, "estrutura FlatBuffers invalida"); };
    const auto too_big = [] { return fail(ManifestError::Invalid, "limite de sanidade excedido"); };

    const std::size_t root_pos = fb.u32(0);
    Fb::Tbl root;
    if (fb.bad || !fb.table(root_pos, root)) {
        return bad();
    }

    const auto read_u64 = [&](unsigned id) -> std::uint64_t {
        const std::size_t at = fb.field(root, id, 8);
        return at == 0 ? 0 : fb.u64(at);
    };
    v.game_id_ = fb.string_field(root, 0);
    v.build_id_ = fb.string_field(root, 1);
    v.build_seq_ = read_u64(2);
    v.created_at_ = static_cast<std::int64_t>(read_u64(3));
    {
        const std::size_t at = fb.field(root, 12, 1);
        v.cipher_id_ = at == 0 ? 0 : fb.u8(at);
    }
    if (fb.bad) {
        return bad();
    }

    // Chunker (informativo).
    if (Fb::Tbl t; fb.sub_table(root, 5, t)) {
        const auto u32_field = [&](unsigned id) -> std::uint32_t {
            const std::size_t at = fb.field(t, id, 4);
            return at == 0 ? 0 : fb.u32(at);
        };
        v.chunker_.min_size = u32_field(0);
        v.chunker_.avg_size = u32_field(1);
        v.chunker_.max_size = u32_field(2);
        v.chunker_.algorithm = std::string(fb.string_field(t, 3));
    }
    if (fb.bad) {
        return bad();
    }

    using Column = ManifestView::Column;
    const auto column = [&](const Fb::Tbl& t, unsigned id, std::size_t elem, std::size_t align,
                            Column& c) {
        std::size_t first = 0;
        std::size_t count = 0;
        fb.vector(t, id, elem, align, first, count);
        c.pos = first;
        c.n = count;
    };

    if (Fb::Tbl t; fb.sub_table(root, 6, t)) { // Blocks
        column(t, 0, 32, 1, v.block_hash_);
        column(t, 1, 4, 4, v.block_raw_);
        column(t, 2, 4, 4, v.block_packed_);
        column(t, 3, 1, 1, v.block_codec_);
    }
    if (Fb::Tbl t; fb.sub_table(root, 7, t)) { // Pieces
        column(t, 0, 32, 1, v.piece_hash_);
        column(t, 1, 4, 4, v.piece_size_);
        column(t, 2, 4, 4, v.piece_first_);
        column(t, 3, 4, 4, v.piece_blocks_);
        column(t, 4, 2, 2, v.piece_group_);
    }
    column(root, 8, 4, 4, v.refs_);
    if (Fb::Tbl t; fb.sub_table(root, 9, t)) { // Files
        column(t, 0, 4, 4, v.file_path_);
        column(t, 1, 8, 8, v.file_size_);
        column(t, 2, 1, 1, v.file_class_);
        column(t, 3, 2, 2, v.file_group_);
        column(t, 4, 4, 4, v.file_first_ref_);
        column(t, 5, 4, 4, v.file_ref_count_);
    }
    column(root, 10, 4, 4, v.group_tables_);    // [Group]
    if (Fb::Tbl t; fb.sub_table(root, 11, t)) { // Sources
        column(t, 0, 4, 4, v.cdn_);
        column(t, 1, 4, 4, v.trackers_);
        column(t, 2, 32, 1, v.infohashes_);
    }
    if (fb.bad) {
        return bad();
    }

    // Limites de sanidade (§8, item 9), antes de qualquer laço sobre os elementos.
    if (v.block_hash_.n > kMaxBlocks || v.file_path_.n > kMaxFiles ||
        v.group_tables_.n > kMaxGroups) {
        return too_big();
    }
    v.block_n_ = v.block_hash_.n;
    v.piece_n_ = v.piece_hash_.n;
    v.ref_n_ = v.refs_.n;
    v.file_n_ = v.file_path_.n;
    v.group_n_ = v.group_tables_.n;

    // Todas as strings dos vetores (caminhos, URLs, trackers) e as tabelas de grupo.
    for (const Column* col : {&v.file_path_, &v.cdn_, &v.trackers_}) {
        for (std::size_t i = 0; i < col->n; ++i) {
            fb.string(col->pos + 4 * i);
        }
    }
    for (std::size_t i = 0; i < v.group_n_; ++i) {
        const std::size_t slot = v.group_tables_.pos + 4 * i;
        Fb::Tbl g;
        const std::size_t target = fb.follow(slot);
        if (fb.bad || !fb.table(target, g)) {
            return bad();
        }
        fb.string_field(g, 0);
        fb.field(g, 1, 1);
        fb.field(g, 2, 4);
        fb.field(g, 3, 4);
        std::size_t first = 0;
        std::size_t count = 0;
        fb.vector(g, 4, 2, 2, first, count);
    }
    if (fb.bad) {
        return bad();
    }
    return {};
}

ManifestStatus ManifestReader::validate(const ManifestView& v, const ReadOptions& options)
{
    const auto invalid = [](const char* what) { return fail(ManifestError::Invalid, what); };

    if (v.cipher_id_ != 0) {
        return fail(ManifestError::Unsupported, "cipher_id diferente de 0 (sigilo e reservado)");
    }
    if (!valid_game_id(v.game_id_)) {
        return invalid("game_id invalido");
    }

    // Item 2: colunas de cada tabela com o mesmo comprimento.
    const std::size_t nb = v.block_n_;
    if (v.block_raw_.n != nb || v.block_packed_.n != nb || v.block_codec_.n != nb) {
        return invalid("colunas de Blocks com comprimentos diferentes");
    }
    const std::size_t np = v.piece_n_;
    if (v.piece_size_.n != np || v.piece_first_.n != np || v.piece_blocks_.n != np ||
        v.piece_group_.n != np) {
        return invalid("colunas de Pieces com comprimentos diferentes");
    }
    const std::size_t nf = v.file_n_;
    if (v.file_size_.n != nf || v.file_class_.n != nf || v.file_group_.n != nf ||
        v.file_first_ref_.n != nf || v.file_ref_count_.n != nf) {
        return invalid("colunas de Files com comprimentos diferentes");
    }

    // Item 1: grupos.
    const std::size_t ng = v.group_n_;
    if (ng == 0) {
        return invalid("sem grupos (falta o grupo core)");
    }
    std::set<std::string, std::less<>> names;
    for (std::size_t i = 0; i < ng; ++i) {
        const GroupInfo g = v.group(i);
        if (!valid_group_name(g.name) || !names.insert(g.name).second) {
            return invalid("nome de grupo invalido ou repetido");
        }
        if (i == 0 && g.name != "core") {
            return invalid("o grupo 0 precisa se chamar core");
        }
        if (!g.deps.empty()) {
            return invalid("deps de grupo e reservado no v0");
        }
    }
    if (v.infohashes_.n != 0 && v.infohashes_.n != ng) {
        return invalid("torrent_infohashes precisa ter um por grupo");
    }
    for (std::size_t i = 0; i < v.cdn_.n; ++i) {
        const std::string_view url = v.string_at(v.cdn_, i);
        if (url.size() <= 8 || url.substr(0, 8) != "https://" || url.back() == '/') {
            return invalid("cdn_base_url precisa ser https:// e sem barra final");
        }
    }

    // Item 5: blocos.
    for (std::size_t i = 0; i < nb; ++i) {
        const std::uint32_t raw_size = v.block_raw_size(i);
        const std::uint32_t packed = v.block_packed_size(i);
        const auto codec = static_cast<std::uint8_t>(v.block_codec(i));
        if (raw_size < 1 || raw_size > kMaxRawBlockSize || packed < 1 || packed > kMaxPieceSize ||
            codec > 2 || (codec == 0 && packed != raw_size)) {
            return invalid("bloco com tamanho ou codec invalido");
        }
    }

    // Itens 3, 4 e 6: peças e grupos.
    std::uint64_t expected_first = 0;
    for (std::size_t i = 0; i < np; ++i) {
        const std::uint32_t first = v.piece_first_block(i);
        const std::uint32_t count = v.piece_block_count(i);
        if (first != expected_first) {
            return invalid("pecas nao cobrem Blocks em ordem, sem lacuna");
        }
        expected_first += count;
        if (expected_first > nb) {
            return invalid("peca passa do fim de Blocks");
        }
        const std::uint32_t size = v.piece_size(i);
        if (size < 1 || size > kMaxPieceSize) {
            return invalid("tamanho de peca fora de 1..1 MiB");
        }
        std::uint64_t sum = 0;
        for (std::uint32_t b = first; b < first + count; ++b) {
            sum += v.block_packed_size(b);
        }
        if (sum != size) {
            return invalid("size da peca diferente da soma dos packed_size");
        }
        if (v.piece_group(i) >= ng) {
            return invalid("grupo de peca fora de groups");
        }
    }
    if (expected_first != nb) {
        return invalid("pecas nao cobrem todos os blocos");
    }
    {
        std::uint64_t covered = 0;
        for (std::size_t g = 0; g < ng; ++g) {
            const GroupInfo info = v.group(g);
            const std::uint64_t end = std::uint64_t{info.piece_first} + info.piece_count;
            if (end > np) {
                return invalid("pecas do grupo passam do fim de Pieces");
            }
            for (std::uint64_t i = info.piece_first; i < end; ++i) {
                if (v.piece_group(i) != g) {
                    return invalid("peca dentro do intervalo de outro grupo");
                }
            }
            covered += info.piece_count;
        }
        if (covered != np) {
            return invalid("ha pecas fora do intervalo de qualquer grupo");
        }
    }

    // Grupo de cada bloco (para o item 8).
    std::vector<std::uint16_t> block_group(nb, 0);
    for (std::size_t i = 0; i < np; ++i) {
        const std::uint32_t first = v.piece_first_block(i);
        std::fill_n(block_group.begin() + first, v.piece_block_count(i), v.piece_group(i));
    }

    // Itens 7 e 8: arquivos.
    std::unordered_set<std::string> folded;
    folded.reserve(nf);
    std::uint64_t expected_ref = 0;
    std::string_view previous;
    for (std::size_t i = 0; i < nf; ++i) {
        const std::string_view path = v.file_path(i);
        if (!valid_path(path)) {
            return invalid("caminho invalido");
        }
        if (i > 0 && !(previous < path)) {
            return invalid("caminhos fora de ordem ou repetidos");
        }
        previous = path;
        if (!folded.insert(ascii_lower(path)).second) {
            return invalid("caminhos que colidem ignorando maiusculas");
        }

        const auto cls = static_cast<std::uint8_t>(v.file_class(i));
        const std::uint16_t group = v.file_group(i);
        if (cls > 2 || group >= ng || (cls == 0) != (group == 0)) {
            return invalid("class e group do arquivo inconsistentes");
        }
        const std::uint32_t first = v.file_first_ref(i);
        const std::uint32_t count = v.file_ref_count(i);
        if (first != expected_ref) {
            return invalid("first_ref nao forma particao de block_refs");
        }
        expected_ref += count;
        if (expected_ref > v.ref_n_) {
            return invalid("ref_count passa do fim de block_refs");
        }
        std::uint64_t size = 0;
        for (std::uint32_t r = first; r < first + count; ++r) {
            const std::uint32_t block = v.block_ref(r);
            if (block >= nb) {
                return invalid("block_refs aponta para fora de Blocks");
            }
            const std::uint16_t block_owner = block_group[block];
            if (block_owner != 0 && block_owner != group) {
                return invalid("arquivo depende de bloco de outro grupo que nao e core");
            }
            size += v.block_raw_size(block);
        }
        if (size != v.file_size(i)) {
            return invalid("size do arquivo diferente da soma dos raw_size");
        }
    }
    if (expected_ref != v.ref_n_) {
        return invalid("block_refs tem entradas que nenhum arquivo usa");
    }

    // Item 10: anti-rollback.
    if (options.installed_build_seq && v.build_seq_ < *options.installed_build_seq) {
        return fail(ManifestError::Rollback, "build_seq menor que o instalado");
    }
    return {};
}

// ================================================================ acessores

Hash32 ManifestView::block_hash(std::size_t i) const noexcept
{
    Hash32 h;
    std::memcpy(h.data(), payload_.data() + block_hash_.pos + 32 * i, 32);
    return h;
}
std::uint32_t ManifestView::block_raw_size(std::size_t i) const noexcept
{
    return rd32(payload_.data() + block_raw_.pos + 4 * i);
}
std::uint32_t ManifestView::block_packed_size(std::size_t i) const noexcept
{
    return rd32(payload_.data() + block_packed_.pos + 4 * i);
}
Codec ManifestView::block_codec(std::size_t i) const noexcept
{
    return static_cast<Codec>(payload_[block_codec_.pos + i]);
}

Hash32 ManifestView::piece_hash(std::size_t i) const noexcept
{
    Hash32 h;
    std::memcpy(h.data(), payload_.data() + piece_hash_.pos + 32 * i, 32);
    return h;
}
std::uint32_t ManifestView::piece_size(std::size_t i) const noexcept
{
    return rd32(payload_.data() + piece_size_.pos + 4 * i);
}
std::uint32_t ManifestView::piece_first_block(std::size_t i) const noexcept
{
    return rd32(payload_.data() + piece_first_.pos + 4 * i);
}
std::uint32_t ManifestView::piece_block_count(std::size_t i) const noexcept
{
    return rd32(payload_.data() + piece_blocks_.pos + 4 * i);
}
std::uint16_t ManifestView::piece_group(std::size_t i) const noexcept
{
    return rd16(payload_.data() + piece_group_.pos + 2 * i);
}

std::uint32_t ManifestView::block_ref(std::size_t i) const noexcept
{
    return rd32(payload_.data() + refs_.pos + 4 * i);
}

std::string_view ManifestView::string_at(const Column& vec, std::size_t i) const noexcept
{
    const std::size_t slot = vec.pos + 4 * i;
    const std::size_t target = slot + rd32(payload_.data() + slot);
    const std::size_t len = rd32(payload_.data() + target);
    return {reinterpret_cast<const char*>(payload_.data() + target + 4), len};
}

std::string_view ManifestView::file_path(std::size_t i) const noexcept
{
    return string_at(file_path_, i);
}
std::uint64_t ManifestView::file_size(std::size_t i) const noexcept
{
    return rd64(payload_.data() + file_size_.pos + 8 * i);
}
FileClass ManifestView::file_class(std::size_t i) const noexcept
{
    return static_cast<FileClass>(payload_[file_class_.pos + i]);
}
std::uint16_t ManifestView::file_group(std::size_t i) const noexcept
{
    return rd16(payload_.data() + file_group_.pos + 2 * i);
}
std::uint32_t ManifestView::file_first_ref(std::size_t i) const noexcept
{
    return rd32(payload_.data() + file_first_ref_.pos + 4 * i);
}
std::uint32_t ManifestView::file_ref_count(std::size_t i) const noexcept
{
    return rd32(payload_.data() + file_ref_count_.pos + 4 * i);
}

std::size_t ManifestView::group_table(std::size_t i) const noexcept
{
    const std::size_t slot = group_tables_.pos + 4 * i;
    return slot + rd32(payload_.data() + slot);
}

GroupInfo ManifestView::group(std::size_t i) const
{
    // Já verificado em open(): a releitura não falha.
    Fb fb{payload_.data(), payload_.size()};
    Fb::Tbl t;
    fb.table(group_table(i), t);
    GroupInfo g;
    g.name = std::string(fb.string_field(t, 0));
    if (const std::size_t at = fb.field(t, 1, 1); at != 0) {
        g.priority = fb.u8(at);
    }
    if (const std::size_t at = fb.field(t, 2, 4); at != 0) {
        g.piece_first = fb.u32(at);
    }
    if (const std::size_t at = fb.field(t, 3, 4); at != 0) {
        g.piece_count = fb.u32(at);
    }
    std::size_t first = 0;
    std::size_t count = 0;
    fb.vector(t, 4, 2, 2, first, count);
    for (std::size_t k = 0; k < count; ++k) {
        g.deps.push_back(rd16(payload_.data() + first + 2 * k));
    }
    return g;
}

ManifestModel ManifestView::to_model() const
{
    ManifestModel m;
    m.game_id = std::string(game_id_);
    m.build_id = std::string(build_id_);
    m.build_seq = build_seq_;
    m.created_at = created_at_;
    m.min_reader = min_reader_;
    m.chunker = chunker_;
    for (std::size_t i = 0; i < block_n_; ++i) {
        m.block_hash.push_back(block_hash(i));
        m.block_raw_size.push_back(block_raw_size(i));
        m.block_packed_size.push_back(block_packed_size(i));
        m.block_codec.push_back(block_codec(i));
    }
    for (std::size_t i = 0; i < piece_n_; ++i) {
        m.piece_hash.push_back(piece_hash(i));
        m.piece_size.push_back(piece_size(i));
        m.piece_first_block.push_back(piece_first_block(i));
        m.piece_block_count.push_back(piece_block_count(i));
        m.piece_group.push_back(piece_group(i));
    }
    for (std::size_t i = 0; i < ref_n_; ++i) {
        m.block_refs.push_back(block_ref(i));
    }
    for (std::size_t i = 0; i < file_n_; ++i) {
        m.file_path.emplace_back(file_path(i));
        m.file_size.push_back(file_size(i));
        m.file_class.push_back(file_class(i));
        m.file_group.push_back(file_group(i));
        m.file_first_ref.push_back(file_first_ref(i));
        m.file_ref_count.push_back(file_ref_count(i));
    }
    for (std::size_t i = 0; i < group_n_; ++i) {
        m.groups.push_back(group(i));
    }
    for (std::size_t i = 0; i < cdn_.n; ++i) {
        m.cdn_base_urls.emplace_back(string_at(cdn_, i));
    }
    for (std::size_t i = 0; i < trackers_.n; ++i) {
        m.trackers.emplace_back(string_at(trackers_, i));
    }
    for (std::size_t i = 0; i < infohashes_.n; ++i) {
        Hash32 h;
        std::memcpy(h.data(), payload_.data() + infohashes_.pos + 32 * i, 32);
        m.torrent_infohashes.push_back(h);
    }
    m.cipher_id = cipher_id_;
    return m;
}

ManifestStatus ManifestView::open(std::span<const std::byte> bytes, const ReadOptions& options,
                                  ManifestView& out)
{
    return ManifestReader::open(bytes, options, out);
}

} // namespace swarmstream::pack
