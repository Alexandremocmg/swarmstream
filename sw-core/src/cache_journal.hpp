// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// journal.bin e index.bin do cache em disco (F1-11, docs/04 §2, §4 e §5). Interno do sw-core:
// só a thread dona (CacheOwner) usa, e os testes incluem para montar arquivos à mão.
//
// journal.bin: cabeçalho de 64 bytes e registros só de append (COMMIT_PIECE, FREE_SLOT,
// CHECKPOINT), cada um com XXH3-64 próprio. O cabeçalho NUNCA é regravado no lugar: um
// cabeçalho rasgado perderia o journal inteiro. O checkpoint cria um journal novo ao lado e o
// troca por rename atômico (replace_file); `last_lsn` do cabeçalho é o LSN na criação.
//
// index.bin: cabeçalho de 64 bytes, tabela de entradas de 56 bytes e região de checksums
// (u64 por bloco), com XXH3-64 do arquivo inteiro (campo zerado no cálculo). Também só é
// trocado por rename atômico.
//
// Recuperação (04 §5): índice do último checkpoint + registros do journal com LSN maior, na
// ordem; o primeiro registro inválido (rasgado, CRC errado, campo fora dos limites, LSN que não
// cresce) encerra a leitura e o journal é truncado ali. Nunca se aceita dado não verificado:
// o pior caso é perder a INDEXAÇÃO de peças, que são baixadas de novo.

#pragma once

#include "platform_file.hpp"
#include "swarmstream/result.hpp"
#include "swarmstream/store_files.hpp"
#include "swarmstream/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace swarmstream::detail {

constexpr uint64_t kJournalMagic = 0x004E52554F4A5753ULL; // "SWJOURN\0" lido em little-endian
constexpr uint16_t kJournalVersion = 1;
constexpr uint32_t kJournalPageSize = 4096;
constexpr size_t kJournalHeaderSize = 64;
constexpr uint32_t kRecordMagic = 0x57414C52; // "WALR"
constexpr size_t kRecordFixedSize = 72;       // até block_count, inclusive
constexpr size_t kMaxPieceSize = 1048576;     // 03 §3
constexpr uint16_t kFlagPinned = 0x01;

constexpr uint64_t kIndexMagic = 0x005845444E495753ULL; // "SWINDEX\0" lido em little-endian
constexpr uint16_t kIndexVersion = 1;
constexpr size_t kIndexHeaderSize = 64;
constexpr size_t kIndexEntrySize = 56;

enum class JournalType : uint16_t {
    CommitPiece = 1,
    FreeSlot = 2,
    Checkpoint = 3,
};

struct JournalRecord
{
    JournalType type = JournalType::CommitPiece;
    uint16_t flags = 0;
    uint64_t lsn = 0;
    int64_t timestamp_us = 0;
    PieceHash piece{};
    uint32_t store_file_id = 0;
    uint32_t slot_in_file = 0;
    uint32_t piece_size = 0;
    std::vector<uint64_t> blocks; // XXH3-64 por bloco (só COMMIT_PIECE)
};

// Bytes de um registro, exatamente no layout do 04 §2 (80 + 8·n). Acrescenta em `out`.
void encode_record(const JournalRecord& r, std::vector<std::byte>& out);
std::array<std::byte, kJournalHeaderSize> encode_journal_header(uint64_t checkpoint_lsn,
                                                                uint64_t last_lsn);

struct JournalReplay
{
    bool created = false;         // o arquivo não existia
    bool header_ok = true;        // false: cabeçalho inválido, nenhum registro aproveitado
    uint64_t checkpoint_lsn = 0;  // do cabeçalho
    uint64_t last_lsn = 0;        // maior LSN válido lido (ou o do cabeçalho)
    uint64_t records = 0;         // registros válidos lidos
    uint64_t applied = 0;         // registros com LSN > after_lsn entregues a `apply`
    uint64_t truncated_bytes = 0; // bytes descartados a partir do primeiro registro inválido
};

// Funções de teste para simular queda (CacheOwnerTestHook). Chamadas na thread dona.
enum class CrashPoint : int {
    AfterDataWrite = 1,     // peça gravada no slot, antes da Barreira 1
    AfterDataFlush,         // depois da Barreira 1, antes do journal
    JournalHalfWritten,     // metade dos bytes do registro gravados
    AfterJournalAppend,     // registro inteiro gravado, antes da Barreira 2
    AfterJournalSync,       // depois da Barreira 2, antes de publicar
    CheckpointIndexTmp,     // index.bin.tmp gravado e sincronizado, antes do rename
    CheckpointIndexRenamed, // index.bin trocado, journal antigo ainda no lugar
    CheckpointJournalTmp,   // journal.bin.tmp gravado, antes do rename
};
using CrashHook = void (*)(CrashPoint point, void* context);

class Journal
{
public:
    Journal() = default;

    // Abre (ou cria) o journal e entrega a `apply`, em ordem, cada registro válido com
    // LSN > after_lsn. Trunca a cauda inválida e sincroniza. Um cabeçalho inválido descarta
    // todos os registros e recria o arquivo com checkpoint = after_lsn.
    [[nodiscard]] static Result<Journal, CacheErrorCode>
    open(const std::filesystem::path& file, uint64_t after_lsn,
         const std::function<void(const JournalRecord&)>& apply, JournalReplay& info);

    // Cria `file` do zero (tmp + rename): cabeçalho e um CHECKPOINT(checkpoint_lsn).
    [[nodiscard]] static CacheErrorCode create_fresh(const std::filesystem::path& file,
                                                     uint64_t checkpoint_lsn, CrashHook hook,
                                                     void* hook_context);

    // Grava o registro no fim (sem sincronizar). Em erro, o journal fica inutilizável.
    [[nodiscard]] CacheErrorCode append(const JournalRecord& r, CrashHook hook, void* hook_context);
    [[nodiscard]] CacheErrorCode sync() noexcept;
    [[nodiscard]] CacheErrorCode close() noexcept;
    [[nodiscard]] uint64_t size_bytes() const noexcept { return end_; }

    // Testes (CacheOwnerTestHook): o próximo sync() falha como um EIO, sem sincronizar; e
    // quantos sync() chegaram de fato ao disco com sucesso.
    void fail_next_sync() noexcept { fail_next_sync_ = true; }
    [[nodiscard]] uint64_t syncs() const noexcept { return syncs_; }

private:
    File file_;
    uint64_t end_ = 0;
    uint64_t syncs_ = 0;
    bool fail_next_sync_ = false;
    std::vector<std::byte> buffer_; // reaproveitado entre appends (registros de até ~8 MiB)
};

// ------------------------------------------------------------------ index.bin

struct IndexEntry
{
    PieceHash piece{};
    SlotId slot = kInvalidSlot; // global: store_file_id × slots_per_file + slot no arquivo
    uint32_t size = 0;
    uint32_t access_counter = 0;
    uint8_t state = 2; // 2 = commitada
    uint8_t flags = 0;
    std::vector<uint64_t> blocks;
};

struct LoadedIndex
{
    uint64_t checkpoint_lsn = 0;
    std::vector<IndexEntry> entries;
};

enum class IndexLoad : uint8_t {
    Ok,
    Missing, // não existe (cache novo)
    Invalid, // magic, versão, layout, tamanhos ou XXH3 não conferem: descartado
};

// Grava `tmp`, sincroniza e troca por `file` (rename atômico e durável).
[[nodiscard]] CacheErrorCode write_index_file(const std::filesystem::path& file,
                                              const std::filesystem::path& tmp,
                                              uint64_t checkpoint_lsn, const StoreLayout& layout,
                                              std::span<const IndexEntry* const> entries,
                                              CrashHook hook, void* hook_context);

[[nodiscard]] IndexLoad read_index_file(const std::filesystem::path& file,
                                        const StoreLayout& layout, LoadedIndex& out);

} // namespace swarmstream::detail
