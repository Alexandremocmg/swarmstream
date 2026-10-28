// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Escrita do manifesto v0 (docs/03 §7, ADR-0012): escritor FlatBuffers, envelope e assinatura.
// O LEITOR (e toda a validação do §8) fica no `sw-format`: o `build_manifest` o reutiliza para
// reler o que gerou, sem uma segunda implementação.

#include "swarmstream/crypto.hpp"
#include "swarmstream/manifest.hpp"
#include "swarmstream/manifest_wire.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

namespace swarmstream::pack {

namespace {

// ================================================================ escritor de FlatBuffers

class Writer
{
public:
    std::vector<std::uint8_t> buf;

    [[nodiscard]] std::size_t size() const noexcept { return buf.size(); }

    void put8(std::uint8_t v) { buf.push_back(v); }
    void put16(std::uint16_t v)
    {
        put8(static_cast<std::uint8_t>(v));
        put8(static_cast<std::uint8_t>(v >> 8));
    }
    void put32(std::uint32_t v)
    {
        put16(static_cast<std::uint16_t>(v));
        put16(static_cast<std::uint16_t>(v >> 16));
    }
    void put64(std::uint64_t v)
    {
        put32(static_cast<std::uint32_t>(v));
        put32(static_cast<std::uint32_t>(v >> 32));
    }
    void set32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            buf[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    void pad_until(std::size_t extra, std::size_t alignment)
    {
        while ((buf.size() + extra) % alignment != 0) {
            put8(0);
        }
    }

    // O uoffset em `slot` passa a apontar para `target` (que vem depois).
    void patch(std::size_t slot, std::size_t target)
    {
        set32(slot, static_cast<std::uint32_t>(target - slot));
    }

    struct Field
    {
        unsigned id;
        std::size_t size; // 1, 2, 4 ou 8
        std::uint64_t value;
        bool ref; // referência (uoffset); o valor é preenchido depois com patch()
    };

    // Escreve vtable + tabela. `slots[i]` recebe a posição do campo i (para patch das refs).
    std::size_t table(const std::vector<Field>& fields, std::vector<std::size_t>& slots)
    {
        unsigned max_id = 0;
        std::size_t max_size = 4;
        for (const Field& f : fields) {
            max_id = std::max(max_id, f.id);
            max_size = std::max(max_size, f.size);
        }
        const std::size_t vsize = 4 + 2 * (static_cast<std::size_t>(max_id) + 1);

        std::vector<std::size_t> order(fields.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return fields[a].size > fields[b].size;
        });
        std::vector<std::size_t> offset(fields.size());
        std::size_t body = 4;
        for (const std::size_t i : order) {
            body = (body + fields[i].size - 1) / fields[i].size * fields[i].size;
            offset[i] = body;
            body += fields[i].size;
        }

        pad_until(vsize, max_size); // a tabela começa alinhada ao maior campo
        put16(static_cast<std::uint16_t>(vsize));
        put16(static_cast<std::uint16_t>(body));
        for (unsigned id = 0; id <= max_id; ++id) {
            std::size_t at = 0;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].id == id) {
                    at = offset[i];
                }
            }
            put16(static_cast<std::uint16_t>(at));
        }
        const std::size_t table_pos = buf.size();
        put32(static_cast<std::uint32_t>(vsize)); // soffset: o vtable fica `vsize` bytes antes
        buf.resize(table_pos + body, 0);
        slots.assign(fields.size(), 0);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const std::size_t at = table_pos + offset[i];
            slots[i] = at;
            for (std::size_t b = 0; b < fields[i].size; ++b) {
                buf[at + b] = static_cast<std::uint8_t>(fields[i].value >> (8 * b));
            }
        }
        return table_pos;
    }

    // Cabeçalho de vetor: [len] e os elementos logo depois, alinhados a `elem_align`.
    std::size_t vector_header(std::size_t count, std::size_t elem_align)
    {
        pad_until(4, std::max<std::size_t>(elem_align, 4));
        const std::size_t pos = buf.size();
        put32(static_cast<std::uint32_t>(count));
        return pos;
    }

    std::size_t string(std::string_view s)
    {
        pad_until(0, 4);
        const std::size_t pos = buf.size();
        put32(static_cast<std::uint32_t>(s.size()));
        for (const char c : s) {
            put8(static_cast<std::uint8_t>(c));
        }
        put8(0);
        return pos;
    }

    std::size_t string_vector(const std::vector<std::string>& items)
    {
        const std::size_t pos = vector_header(items.size(), 4);
        const std::size_t first = buf.size();
        buf.resize(first + 4 * items.size(), 0);
        for (std::size_t i = 0; i < items.size(); ++i) {
            patch(first + 4 * i, string(items[i]));
        }
        return pos;
    }

    std::size_t hash_vector(const std::vector<Hash32>& items)
    {
        const std::size_t pos = vector_header(items.size(), 1);
        for (const Hash32& h : items) {
            buf.insert(buf.end(), h.begin(), h.end());
        }
        return pos;
    }

    template <typename T, typename Put>
    std::size_t scalar_vector(const std::vector<T>& items, std::size_t elem_size, Put put)
    {
        const std::size_t pos = vector_header(items.size(), elem_size);
        for (const T& v : items) {
            put(*this, v);
        }
        return pos;
    }

    std::size_t u8_vector(const std::vector<std::uint8_t>& items)
    {
        return scalar_vector(items, 1, [](Writer& w, std::uint8_t v) { w.put8(v); });
    }
    std::size_t u16_vector(const std::vector<std::uint16_t>& items)
    {
        return scalar_vector(items, 2, [](Writer& w, std::uint16_t v) { w.put16(v); });
    }
    std::size_t u32_vector(const std::vector<std::uint32_t>& items)
    {
        return scalar_vector(items, 4, [](Writer& w, std::uint32_t v) { w.put32(v); });
    }
    std::size_t u64_vector(const std::vector<std::uint64_t>& items)
    {
        return scalar_vector(items, 8, [](Writer& w, std::uint64_t v) { w.put64(v); });
    }
};

template <typename E>
std::vector<std::uint8_t> bytes_of(const std::vector<E>& items)
{
    std::vector<std::uint8_t> out;
    out.reserve(items.size());
    for (const E e : items) {
        out.push_back(static_cast<std::uint8_t>(e));
    }
    return out;
}

std::vector<std::uint8_t> encode_payload(const ManifestModel& m)
{
    Writer w;
    w.put32(0); // uoffset da raiz, preenchido no fim
    for (const char c : kFileIdentifier) {
        w.put8(static_cast<std::uint8_t>(c));
    }

    // Raiz: ManifestPayload. Os ids são os do esquema (docs/03 §7.2).
    enum RootId : unsigned {
        GameId = 0,
        BuildId,
        BuildSeq,
        CreatedAt,
        MinReader,
        Chunker,
        Blocks,
        Pieces,
        BlockRefs,
        Files,
        Groups,
        Sources,
        CipherId
    };
    std::vector<Writer::Field> fields = {
        {GameId, 4, 0, true},
        {BuildId, 4, 0, true},
        {BuildSeq, 8, m.build_seq, false},
        {CreatedAt, 8, static_cast<std::uint64_t>(m.created_at), false},
        {MinReader, 2, m.min_reader, false},
        {Chunker, 4, 0, true},
        {Blocks, 4, 0, true},
        {Pieces, 4, 0, true},
        {Files, 4, 0, true},
        {Sources, 4, 0, true},
    };
    if (!m.block_refs.empty()) {
        fields.push_back({BlockRefs, 4, 0, true});
    }
    if (!m.groups.empty()) {
        fields.push_back({Groups, 4, 0, true});
    }
    if (m.cipher_id != 0) {
        fields.push_back({CipherId, 1, m.cipher_id, false});
    }
    std::vector<std::size_t> slots;
    const std::size_t root = w.table(fields, slots);
    w.set32(0, static_cast<std::uint32_t>(root));
    const auto slot_of = [&](unsigned id) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].id == id) {
                return slots[i];
            }
        }
        return std::size_t{0};
    };

    w.patch(slot_of(GameId), w.string(m.game_id));
    w.patch(slot_of(BuildId), w.string(m.build_id));

    // Chunker { min_size, avg_size, max_size, algorithm }
    {
        std::vector<std::size_t> s;
        const std::size_t t = w.table({{0, 4, m.chunker.min_size, false},
                                       {1, 4, m.chunker.avg_size, false},
                                       {2, 4, m.chunker.max_size, false},
                                       {3, 4, 0, true}},
                                      s);
        w.patch(slot_of(Chunker), t);
        w.patch(s[3], w.string(m.chunker.algorithm));
    }

    // Em cada tabela colunar só entram as colunas com elementos (ausente == vazio).
    const auto emit_table = [&](unsigned root_id, std::vector<Writer::Field> table_fields,
                                const auto& fill) {
        std::vector<std::size_t> s;
        const std::size_t t = w.table(table_fields, s);
        w.patch(slot_of(root_id), t);
        fill(s);
    };

    { // Blocks { hash, raw_size, packed_size, codec }
        std::vector<Writer::Field> f;
        if (!m.block_hash.empty()) {
            f = {{0, 4, 0, true}, {1, 4, 0, true}, {2, 4, 0, true}, {3, 4, 0, true}};
        }
        emit_table(Blocks, f, [&](const std::vector<std::size_t>& s) {
            if (s.empty()) {
                return;
            }
            w.patch(s[0], w.hash_vector(m.block_hash));
            w.patch(s[1], w.u32_vector(m.block_raw_size));
            w.patch(s[2], w.u32_vector(m.block_packed_size));
            w.patch(s[3], w.u8_vector(bytes_of(m.block_codec)));
        });
    }
    { // Pieces { piece_hash, size, first_block, block_count, group }
        std::vector<Writer::Field> f;
        if (!m.piece_hash.empty()) {
            f = {{0, 4, 0, true},
                 {1, 4, 0, true},
                 {2, 4, 0, true},
                 {3, 4, 0, true},
                 {4, 4, 0, true}};
        }
        emit_table(Pieces, f, [&](const std::vector<std::size_t>& s) {
            if (s.empty()) {
                return;
            }
            w.patch(s[0], w.hash_vector(m.piece_hash));
            w.patch(s[1], w.u32_vector(m.piece_size));
            w.patch(s[2], w.u32_vector(m.piece_first_block));
            w.patch(s[3], w.u32_vector(m.piece_block_count));
            w.patch(s[4], w.u16_vector(m.piece_group));
        });
    }
    if (!m.block_refs.empty()) {
        w.patch(slot_of(BlockRefs), w.u32_vector(m.block_refs));
    }
    { // Files { path, size, class, group, first_ref, ref_count }
        std::vector<Writer::Field> f;
        if (!m.file_path.empty()) {
            f = {{0, 4, 0, true}, {1, 4, 0, true}, {2, 4, 0, true},
                 {3, 4, 0, true}, {4, 4, 0, true}, {5, 4, 0, true}};
        }
        emit_table(Files, f, [&](const std::vector<std::size_t>& s) {
            if (s.empty()) {
                return;
            }
            w.patch(s[0], w.string_vector(m.file_path));
            w.patch(s[1], w.u64_vector(m.file_size));
            w.patch(s[2], w.u8_vector(bytes_of(m.file_class)));
            w.patch(s[3], w.u16_vector(m.file_group));
            w.patch(s[4], w.u32_vector(m.file_first_ref));
            w.patch(s[5], w.u32_vector(m.file_ref_count));
        });
    }
    if (!m.groups.empty()) { // [Group]: vetor de uoffsets para tabelas
        const std::size_t vec = w.vector_header(m.groups.size(), 4);
        const std::size_t first = w.size();
        w.buf.resize(first + 4 * m.groups.size(), 0);
        w.patch(slot_of(Groups), vec);
        for (std::size_t i = 0; i < m.groups.size(); ++i) {
            const GroupInfo& g = m.groups[i];
            std::vector<Writer::Field> gf = {{0, 4, 0, true},
                                             {1, 1, g.priority, false},
                                             {2, 4, g.piece_first, false},
                                             {3, 4, g.piece_count, false}};
            if (!g.deps.empty()) {
                gf.push_back({4, 4, 0, true});
            }
            std::vector<std::size_t> s;
            const std::size_t t = w.table(gf, s);
            w.patch(first + 4 * i, t);
            w.patch(s[0], w.string(g.name));
            if (!g.deps.empty()) {
                w.patch(s[4], w.u16_vector(g.deps));
            }
        }
    }
    { // Sources { cdn_base_urls, trackers, torrent_infohashes }
        std::vector<Writer::Field> f;
        if (!m.cdn_base_urls.empty()) {
            f.push_back({0, 4, 0, true});
        }
        if (!m.trackers.empty()) {
            f.push_back({1, 4, 0, true});
        }
        if (!m.torrent_infohashes.empty()) {
            f.push_back({2, 4, 0, true});
        }
        emit_table(Sources, f, [&](const std::vector<std::size_t>& s) {
            std::size_t k = 0;
            if (!m.cdn_base_urls.empty()) {
                w.patch(s[k++], w.string_vector(m.cdn_base_urls));
            }
            if (!m.trackers.empty()) {
                w.patch(s[k++], w.string_vector(m.trackers));
            }
            if (!m.torrent_infohashes.empty()) {
                w.patch(s[k++], w.hash_vector(m.torrent_infohashes));
            }
        });
    }

    if (w.buf.size() > kMaxPayloadLen) {
        return {};
    }
    return std::move(w.buf);
}

std::vector<std::byte> envelope_and_sign(std::span<const std::uint8_t> payload,
                                         const Ed25519Seed& seed)
{
    const std::optional<Ed25519PublicKey> public_key = ed25519_public_key(seed);
    if (!public_key) {
        return {};
    }
    const KeyId key_id = key_id_of(*public_key);

    std::vector<std::uint8_t> head;
    head.reserve(kEnvelopeSize);
    head.insert(head.end(), kMagic.begin(), kMagic.end());
    head.insert(head.end(), {0, 0});       // format_version = 0
    head.push_back(1);                     // hash_alg = SHA-256
    head.push_back(1);                     // sig_alg = Ed25519
    head.insert(head.end(), {0, 0, 0, 0}); // reserved
    for (int i = 0; i < 8; ++i) {          // payload_len
        head.push_back(
            static_cast<std::uint8_t>(static_cast<std::uint64_t>(payload.size()) >> (8 * i)));
    }
    head.insert(head.end(), key_id.begin(), key_id.end());
    head.insert(head.end(), 8, 0); // reserved2

    const auto as_span = [](const void* p, std::size_t n) {
        return std::span<const std::byte>(static_cast<const std::byte*>(p), n);
    };
    const std::optional<Ed25519Signature> signature = ed25519_sign(
        seed, {as_span(kSignaturePrefix.data(), kSignaturePrefix.size()),
               as_span(head.data(), kSignedHeaderSize), as_span(payload.data(), payload.size())});
    if (!signature) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(kEnvelopeSize + payload.size());
    const auto append = [&](const std::uint8_t* p, std::size_t n) {
        out.insert(out.end(), reinterpret_cast<const std::byte*>(p),
                   reinterpret_cast<const std::byte*>(p) + n);
    };
    append(head.data(), head.size());
    append(signature->data(), signature->size());
    append(payload.data(), payload.size());
    return out;
}

} // namespace

std::vector<std::byte> sign_payload(std::span<const std::uint8_t> payload,
                                    const Ed25519Seed& signing_seed)
{
    return envelope_and_sign(payload, signing_seed);
}

// ================================================================ escrita (API)

std::vector<std::byte> encode_manifest(const ManifestModel& model, const Ed25519Seed& signing_seed)
{
    const std::vector<std::uint8_t> payload = encode_payload(model);
    if (payload.empty()) {
        return {};
    }
    return envelope_and_sign(payload, signing_seed);
}

BuildResult build_manifest(const ManifestModel& model, const Ed25519Seed& signing_seed)
{
    BuildResult result;
    if (!crypto_ready()) {
        result.status = ManifestStatus{ManifestError::SignatureInvalid, "libsodium nao iniciou"};
        return result;
    }
    std::vector<std::byte> bytes = encode_manifest(model, signing_seed);
    if (bytes.empty()) {
        result.status = ManifestStatus{
            ManifestError::Invalid, "payload passa de 256 MiB ou a assinatura falhou (libsodium)"};
        return result;
    }
    const std::optional<Ed25519PublicKey> public_key = ed25519_public_key(signing_seed);
    if (!public_key) {
        result.status = ManifestStatus{ManifestError::SignatureInvalid,
                                       "libsodium nao derivou a chave publica"};
        return result;
    }
    const std::array<Ed25519PublicKey, 1> trusted = {*public_key};
    ManifestView view;
    ReadOptions options;
    options.trusted_keys = trusted;
    if (const ManifestStatus s = ManifestView::open(bytes, options, view); !s.ok()) {
        result.status = s;
        return result;
    }
    if (!(view.to_model() == model)) {
        result.status = ManifestStatus{ManifestError::Invalid, "leitura de volta difere do modelo"};
        return result;
    }
    result.bytes = std::move(bytes);
    return result;
}

} // namespace swarmstream::pack
