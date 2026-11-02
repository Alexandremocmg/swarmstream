// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Os block_count esperados por peça (F1-70, docs/04 §5 passo 3, F1-60 §6.3).
//
// Quem abre o cache (o sw_init, F1-67) entrega ao CacheOwner::start, em
// CacheOwnerConfig::expected_block_counts, um vetor ORDENADO {hash da peça, block_count} com as
// peças do manifesto em uso. A chave é o hash da peça (a raiz Merkle, que é a chave do cache em
// disco), não o índice dela no manifesto. Na recuperação, toda entrada do cache cuja peça consta
// ali e cujo block_count difere é descartada (a peça é de um manifesto que partiu os bytes em
// outros blocos); peça que NÃO consta é mantida (conteúdo endereçado por hash: pode servir a outro
// build; segue verificada por bloco na leitura).
//
// O hash da peça é a raiz Merkle dos bytes ARMAZENADOS (docs/03 §3), não da partição em blocos:
// dois manifestos podem ter a mesma peça com block_count diferentes. É exatamente o caso que esta
// conferência pega. Dentro de UM manifesto, a mesma peça repetida com block_counts diferentes é
// ambígua: sai do vetor (vale como "não consta"; a leitura confere pelos checksums, sem risco de
// dado errado).
//
// Quem constrói o vetor é o chamador, uma vez, e o libera depois do start() (o CacheOwner só o lê
// durante a recuperação e não guarda o span). A falta de memória na construção é um status
// (OutOfMemory), nunca uma exceção nem std::terminate; ver o comentário em .cpp sobre o MSVC em
// Debug.
#pragma once

#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swarmstream::pack {
class ManifestView;
} // namespace swarmstream::pack

namespace swarmstream {

struct PieceBlockCount
{
    PieceHash piece;
    uint32_t block_count = 0; // >= 1
};

enum class ExpectedBlockCountsStatus {
    Ok,
    OutOfMemory, // `out` fica exatamente como estava
};

// Peça `i` (0 <= i < n) de uma fonte qualquer; noexcept. `context` é de quem chama.
using PieceBlockCountAt = PieceBlockCount (*)(const void* context, size_t i) noexcept;

// Ordena `values` por hash da peça e tira as repetições: hashes repetidos com o mesmo block_count
// viram um só; com block_counts diferentes, todos saem (ambíguo, ver acima); block_count 0 sai.
// No lugar, sem alocar.
void normalize_expected_block_counts(std::vector<PieceBlockCount>& values) noexcept;

// Constrói o vetor normalizado para `n` peças. Garantia forte: com OutOfMemory, `out` não muda.
[[nodiscard]] ExpectedBlockCountsStatus
build_expected_block_counts(size_t n, PieceBlockCountAt at, const void* context,
                            std::vector<PieceBlockCount>& out) noexcept;

// O mesmo, para as peças de um manifesto aberto (piece_hash(i), piece_block_count(i)).
[[nodiscard]] ExpectedBlockCountsStatus
build_expected_block_counts(const pack::ManifestView& view,
                            std::vector<PieceBlockCount>& out) noexcept;

// O vetor é válido para o start(): estritamente crescente por hash e block_count >= 1.
[[nodiscard]] bool expected_block_counts_valid(std::span<const PieceBlockCount> values) noexcept;

// Busca binária em um vetor válido. 0 se a peça não consta (block_count 0 nunca é válido).
[[nodiscard]] uint32_t expected_block_count_of(std::span<const PieceBlockCount> values,
                                               const PieceHash& piece) noexcept;

} // namespace swarmstream
