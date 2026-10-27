// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Manifesto v0 (docs/03 §7, ADR-0012): envelope fixo + payload FlatBuffers, assinado com Ed25519.
//
//   build_manifest()   modelo em memória -> bytes assinados (e se confere lendo de volta)
//   ManifestView::open() bytes -> leitura sem cópia, na ordem do §7.1: cabeçalho, chave,
//                      ASSINATURA, sondagem do min_reader, verificador e validações do §8.
//                      Nada do payload é usado antes de a assinatura passar.
//
// O payload é escrito e verificado à mão (sem `flatc` nem a biblioteca do FlatBuffers): o
// esquema é fixo e pequeno, e assim o `sw-pack` continua sem dependências. O layout segue o
// formato do FlatBuffers, então um leitor oficial (Go, ADR-0010) lê os mesmos bytes.
#pragma once

#include "swarmstream/ed25519.hpp"
#include "swarmstream/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swarmstream::pack {

using Hash32 = Sha256Digest;

enum class Codec : std::uint8_t { None = 0, Zstd = 1, Lz4 = 2 };
enum class FileClass : std::uint8_t { Core = 0, Group = 1, Optional = 2 };

// Constantes do formato (docs/03 §7.1 e §8).
inline constexpr std::size_t kEnvelopeSize = 104;
inline constexpr std::uint64_t kMaxPayloadLen = 256ULL * 1024 * 1024;
inline constexpr std::uint16_t kSupportedPayloadRevision = 0;
inline constexpr std::uint32_t kMaxBlocks = 16U * 1024 * 1024;
inline constexpr std::uint32_t kMaxFiles = 4U * 1024 * 1024;
inline constexpr std::uint32_t kMaxGroups = 4096;
inline constexpr std::size_t kMaxStringLen = 1024;
inline constexpr std::uint32_t kMaxRawBlockSize = 262144;
inline constexpr std::uint32_t kMaxPieceSize = 1048576;

// Erros, na ordem em que o leitor os produz; espelham SW_E_* de swarmstream.h.
enum class ManifestError : std::uint8_t {
    Ok = 0,
    Invalid,          // SW_E_MANIFEST_INVALID
    Unsupported,      // SW_E_MANIFEST_UNSUPPORTED
    SignatureInvalid, // SW_E_SIGNATURE_INVALID (assinatura ruim ou chave desconhecida)
    Rollback,         // SW_E_MANIFEST_ROLLBACK
};

struct ManifestStatus
{
    ManifestError code = ManifestError::Ok;
    const char* detail = ""; // texto estático, para log e diagnóstico; nunca para decidir
    [[nodiscard]] bool ok() const noexcept { return code == ManifestError::Ok; }
};

[[nodiscard]] const char* to_string(ManifestError error) noexcept;

// ---------------------------------------------------------------- modelo em memória

struct ChunkerInfo
{
    std::uint32_t min_size = 0;
    std::uint32_t avg_size = 0;
    std::uint32_t max_size = 0;
    std::string algorithm;
    friend bool operator==(const ChunkerInfo&, const ChunkerInfo&) = default;
};

struct GroupInfo
{
    std::string name;
    std::uint8_t priority = 0;
    std::uint32_t piece_first = 0;
    std::uint32_t piece_count = 0;
    std::vector<std::uint16_t> deps; // v0: sempre vazio
    friend bool operator==(const GroupInfo&, const GroupInfo&) = default;
};

// Tabelas colunares, como no esquema (um vetor por campo, mesmo índice).
struct ManifestModel
{
    std::string game_id;
    std::string build_id;
    std::uint64_t build_seq = 0;
    std::int64_t created_at = 0;
    std::uint16_t min_reader = 0;
    ChunkerInfo chunker;

    // Blocks (ordem física)
    std::vector<Hash32> block_hash;
    std::vector<std::uint32_t> block_raw_size;
    std::vector<std::uint32_t> block_packed_size;
    std::vector<Codec> block_codec;

    // Pieces
    std::vector<Hash32> piece_hash;
    std::vector<std::uint32_t> piece_size;
    std::vector<std::uint32_t> piece_first_block;
    std::vector<std::uint32_t> piece_block_count;
    std::vector<std::uint16_t> piece_group;

    std::vector<std::uint32_t> block_refs;

    // Files (ordenados por caminho, bytewise)
    std::vector<std::string> file_path;
    std::vector<std::uint64_t> file_size;
    std::vector<FileClass> file_class;
    std::vector<std::uint16_t> file_group;
    std::vector<std::uint32_t> file_first_ref;
    std::vector<std::uint32_t> file_ref_count;

    std::vector<GroupInfo> groups;

    // Sources
    std::vector<std::string> cdn_base_urls;
    std::vector<std::string> trackers;
    std::vector<Hash32> torrent_infohashes;

    std::uint8_t cipher_id = 0;

    friend bool operator==(const ManifestModel&, const ManifestModel&) = default;
};

// ---------------------------------------------------------------- escrita

// Só o envelope e o payload, sem conferir nada do modelo. Para testes que precisam de um
// manifesto assinado e inválido; o código de produção usa build_manifest.
[[nodiscard]] std::vector<std::byte> encode_manifest(const ManifestModel& model,
                                                     const Ed25519Seed& signing_seed);

// Envelope assinado em volta de um payload qualquer (sem olhar o conteúdo). Existe para os
// testes que precisam de payload malformado com assinatura VÁLIDA, o que só um atacante com a
// chave faria; o código de produção usa build_manifest.
[[nodiscard]] std::vector<std::byte> sign_payload(std::span<const std::uint8_t> payload,
                                                  const Ed25519Seed& signing_seed);

struct BuildResult
{
    ManifestStatus status;
    std::vector<std::byte> bytes; // vazio se status não for Ok
};

// Codifica, assina e LÊ DE VOLTA o resultado (com a chave pública derivada da semente), só
// devolvendo bytes que o leitor aceita e que reconstroem exatamente o modelo.
[[nodiscard]] BuildResult build_manifest(const ManifestModel& model,
                                         const Ed25519Seed& signing_seed);

// ---------------------------------------------------------------- leitura

using KeyId = std::array<std::uint8_t, 8>;

// key_id = primeiros 8 bytes de SHA-256(chave pública) (docs/03 §7.1).
[[nodiscard]] KeyId key_id_of(const Ed25519PublicKey& public_key) noexcept;

struct ReadOptions
{
    std::span<const Ed25519PublicKey> trusted_keys;
    // build_seq do manifesto instalado do mesmo game_id; o novo precisa ser >= a ele (§8, item 10).
    std::optional<std::uint64_t> installed_build_seq;
};

// Sondagem do min_reader (§7.1, passo 4) sobre bytes JÁ autenticados. Ok ⇒ `min_reader`
// preenchido; Invalid ⇒ payload malformado; Unsupported ⇒ min_reader acima da revisão suportada.
[[nodiscard]] ManifestStatus probe_min_reader(std::span<const std::byte> payload,
                                              std::uint16_t& min_reader) noexcept;

// Visão sobre os bytes do manifesto (que precisam continuar vivos e imutáveis). Depois de
// open() ok, todos os índices e limites foram conferidos; os acessores não verificam de novo,
// então o chamador respeita `0 <= i < contagem`.
class ManifestView
{
public:
    [[nodiscard]] static ManifestStatus open(std::span<const std::byte> bytes,
                                             const ReadOptions& options, ManifestView& out);

    [[nodiscard]] const Hash32& manifest_id() const noexcept { return manifest_id_; }
    [[nodiscard]] KeyId key_id() const noexcept { return key_id_; }
    [[nodiscard]] std::size_t payload_size() const noexcept { return payload_size_; }

    [[nodiscard]] std::string_view game_id() const noexcept { return game_id_; }
    [[nodiscard]] std::string_view build_id() const noexcept { return build_id_; }
    [[nodiscard]] std::uint64_t build_seq() const noexcept { return build_seq_; }
    [[nodiscard]] std::int64_t created_at() const noexcept { return created_at_; }
    [[nodiscard]] std::uint16_t min_reader() const noexcept { return min_reader_; }

    [[nodiscard]] std::size_t block_count() const noexcept { return block_n_; }
    [[nodiscard]] Hash32 block_hash(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t block_raw_size(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t block_packed_size(std::size_t i) const noexcept;
    [[nodiscard]] Codec block_codec(std::size_t i) const noexcept;

    [[nodiscard]] std::size_t piece_count() const noexcept { return piece_n_; }
    [[nodiscard]] Hash32 piece_hash(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t piece_size(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t piece_first_block(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t piece_block_count(std::size_t i) const noexcept;
    [[nodiscard]] std::uint16_t piece_group(std::size_t i) const noexcept;

    [[nodiscard]] std::size_t block_ref_count() const noexcept { return ref_n_; }
    [[nodiscard]] std::uint32_t block_ref(std::size_t i) const noexcept;

    [[nodiscard]] std::size_t file_count() const noexcept { return file_n_; }
    [[nodiscard]] std::string_view file_path(std::size_t i) const noexcept;
    [[nodiscard]] std::uint64_t file_size(std::size_t i) const noexcept;
    [[nodiscard]] FileClass file_class(std::size_t i) const noexcept;
    [[nodiscard]] std::uint16_t file_group(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t file_first_ref(std::size_t i) const noexcept;
    [[nodiscard]] std::uint32_t file_ref_count(std::size_t i) const noexcept;

    [[nodiscard]] std::size_t group_count() const noexcept { return group_n_; }
    [[nodiscard]] GroupInfo group(std::size_t i) const;

    // Cópia completa para o modelo (testes e ferramentas; o caminho de leitura não usa).
    [[nodiscard]] ManifestModel to_model() const;

private:
    struct Column
    {
        std::size_t pos = 0; // posição do primeiro elemento no payload
        std::size_t n = 0;
    };

    [[nodiscard]] std::string_view string_at(const Column& vec, std::size_t i) const noexcept;
    [[nodiscard]] std::size_t group_table(std::size_t i) const noexcept;

    friend class ManifestReader;

    std::span<const std::uint8_t> payload_;
    std::size_t payload_size_ = 0;
    Hash32 manifest_id_{};
    KeyId key_id_{};

    std::string_view game_id_;
    std::string_view build_id_;
    std::uint64_t build_seq_ = 0;
    std::int64_t created_at_ = 0;
    std::uint16_t min_reader_ = 0;
    ChunkerInfo chunker_;

    std::size_t block_n_ = 0, piece_n_ = 0, ref_n_ = 0, file_n_ = 0, group_n_ = 0;
    Column block_hash_, block_raw_, block_packed_, block_codec_;
    Column piece_hash_, piece_size_, piece_first_, piece_blocks_, piece_group_;
    Column refs_;
    Column file_path_, file_size_, file_class_, file_group_, file_first_ref_, file_ref_count_;
    Column group_tables_; // vetor de uoffsets para as tabelas Group
    Column cdn_, trackers_, infohashes_;
    std::uint8_t cipher_id_ = 0;
};

} // namespace swarmstream::pack
