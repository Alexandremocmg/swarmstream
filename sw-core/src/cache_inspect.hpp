// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Inspeção do cache em disco SEM alterá-lo (F1-18). Interno do sw-core: usado pela ferramenta
// sw-cache-inspect e por seus testes.
//
// Reproduz a recuperação do 04 §5 (index.bin + registros do journal com LSN maior, em ordem),
// mas só para relatar: nada é criado, truncado, apagado nem sincronizado na pasta inspecionada.
// O journal é lido numa CÓPIA temporária, porque Journal::open trunca a cauda inválida; assim
// a leitura reaproveita o parser já testado em vez de duplicar o formato.
#pragma once

#include "swarmstream/result.hpp"
#include "swarmstream/store_files.hpp"
#include "swarmstream/types.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace swarmstream::detail {

enum class InspectFileState : uint8_t {
    Missing, // não existe
    Invalid, // existe mas não passa nas verificações (descartado pela recuperação)
    Ok,
};

struct InspectedPiece
{
    PieceHash piece{};
    uint32_t store_file_id = 0;
    uint32_t slot_in_file = 0;
    uint32_t size = 0;
    uint32_t block_count = 0;
    bool store_file_present = true; // false: a recuperação descartaria a peça
};

struct InspectedStoreFile
{
    uint32_t id = 0;
    bool present = false;
    uint64_t size_bytes = 0; // tamanho lógico do arquivo, se presente
    uint32_t pieces = 0;
    uint64_t piece_bytes = 0;
};

struct CacheInspection
{
    InspectFileState index = InspectFileState::Missing;
    uint64_t index_lsn = 0;
    uint64_t index_entries = 0;
    InspectFileState journal = InspectFileState::Missing;
    uint64_t journal_checkpoint_lsn = 0;
    uint64_t journal_last_lsn = 0;
    uint64_t journal_records = 0;
    uint64_t journal_applied = 0;         // registros com LSN > índice, aplicados sobre ele
    uint64_t journal_truncated_bytes = 0; // cauda que a próxima abertura descartaria
    std::vector<InspectedPiece> pieces;   // estado resultante, ordenado por (arquivo, slot)
    std::vector<InspectedStoreFile> store_files; // presentes na pasta ou citados por peças
    uint64_t total_piece_bytes = 0;
    uint32_t pieces_in_missing_files = 0;
};

// IoError se `dir` não é uma pasta ou uma leitura falha; nunca modifica `dir`.
[[nodiscard]] Result<CacheInspection, CacheErrorCode>
inspect_cache(const std::filesystem::path& dir, const StoreLayout& layout);

} // namespace swarmstream::detail
