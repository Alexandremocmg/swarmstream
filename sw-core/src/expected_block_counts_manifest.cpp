// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// A sobrecarga que lê um ManifestView fica fora de expected_block_counts.cpp de propósito: o
// CacheOwner usa só a busca e a validação, e este arquivo puxa o ManifestView (sw-format:
// libsodium, Zstd, LZ4). Em biblioteca estática, quem não chama esta função não carrega nada disso
// (a extensão do Godot confere que o sw-format não entra, extension.yml).

#include "swarmstream/expected_block_counts.hpp"
#include "swarmstream/manifest_view.hpp"

namespace swarmstream {

ExpectedBlockCountsStatus build_expected_block_counts(const pack::ManifestView& view,
                                                      std::vector<PieceBlockCount>& out) noexcept
{
    const auto at = [](const void* context, size_t i) noexcept {
        const auto& v = *static_cast<const pack::ManifestView*>(context);
        PieceBlockCount value;
        value.piece = PieceHash(v.piece_hash(i));
        value.block_count = v.piece_block_count(i);
        return value;
    };
    return build_expected_block_counts(view.piece_count(), at, &view, out);
}

} // namespace swarmstream
