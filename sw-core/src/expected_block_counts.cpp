// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/expected_block_counts.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace swarmstream {

namespace {

bool hash_less(const PieceBlockCount& a, const PieceBlockCount& b) noexcept
{
    return a.piece < b.piece;
}

} // namespace

void normalize_expected_block_counts(std::vector<PieceBlockCount>& values) noexcept
{
    std::sort(values.begin(), values.end(), hash_less);
    // Compacta no lugar: cada grupo de mesmo hash vira uma entrada, ou nenhuma se for ambíguo
    // (block_counts diferentes) ou inválido (0).
    size_t out = 0;
    size_t i = 0;
    while (i < values.size()) {
        size_t j = i + 1;
        bool consistent = values[i].block_count != 0;
        while (j < values.size() && values[j].piece == values[i].piece) {
            if (values[j].block_count != values[i].block_count) consistent = false;
            ++j;
        }
        if (consistent) values[out++] = values[i];
        i = j;
    }
    values.erase(values.begin() + static_cast<std::ptrdiff_t>(out), values.end());
}

ExpectedBlockCountsStatus build_expected_block_counts(size_t n, PieceBlockCountAt at,
                                                      const void* context,
                                                      std::vector<PieceBlockCount>& out) noexcept
{
    // Tudo o que pode alocar roda DENTRO do `try` e por construtor que NÃO é `noexcept`, e o
    // resultado só toca em `out` no fim, por move-assign noexcept (garantia forte). Só o `try` não
    // basta (F1-63, vfs.hpp): no MSVC em Debug (_ITERATOR_DEBUG_LEVEL != 0) os construtores
    // PADRÃO (e o de move) de std::vector são noexcept e alocam o proxy dos iteradores; a falta de
    // memória ali vira std::terminate antes de chegar ao catch. `vector(n)` não é noexcept: o
    // bad_alloc, inclusive o do proxy, chega aqui.
    static_assert(!std::is_nothrow_constructible_v<std::vector<PieceBlockCount>, size_t>,
                  "vector(n) precisa poder lançar bad_alloc (ver o comentário acima)");
    static_assert(std::is_nothrow_move_assignable_v<std::vector<PieceBlockCount>>,
                  "publicar em `out` não pode falhar depois da construção");
    try {
        std::vector<PieceBlockCount> next(n);
        for (size_t i = 0; i < n; ++i)
            next[i] = at(context, i);
        normalize_expected_block_counts(next);
        out = std::move(next);
        return ExpectedBlockCountsStatus::Ok;
    } catch (const std::bad_alloc&) {
        return ExpectedBlockCountsStatus::OutOfMemory;
    } catch (const std::length_error&) { // n além de max_size(): não cabe, é falta de memória
        return ExpectedBlockCountsStatus::OutOfMemory;
    }
}

bool expected_block_counts_valid(std::span<const PieceBlockCount> values) noexcept
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].block_count == 0) return false;
        if (i != 0 && !(values[i - 1].piece < values[i].piece)) return false;
    }
    return true;
}

uint32_t expected_block_count_of(std::span<const PieceBlockCount> values,
                                 const PieceHash& piece) noexcept
{
    const auto it = std::lower_bound(
        values.begin(), values.end(), piece,
        [](const PieceBlockCount& v, const PieceHash& key) noexcept { return v.piece < key; });
    return (it != values.end() && it->piece == piece) ? it->block_count : 0;
}

} // namespace swarmstream
