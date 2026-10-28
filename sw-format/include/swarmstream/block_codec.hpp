// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Codec do bloco e DESCOMPRESSÃO (docs/03 §4): NONE, ZSTD (frame padrão, com Frame_Content_Size,
// sem dicionário) e LZ4 (block format). Parte do `sw-format` (cliente); comprimir é do `sw-pack`
// (`swarmstream/codec.hpp`, incluído aqui para quem comprime e descomprime).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace swarmstream::pack {

// Valores gravados em `codec` no manifesto (docs/03 §4).
enum class Codec : std::uint8_t {
    None = 0,
    Zstd = 1,
    Lz4 = 2,
};

[[nodiscard]] std::string_view codec_name(Codec codec) noexcept;

// Tamanho máximo do bloco bruto (docs/03 §4).
inline constexpr std::size_t kMaxBlockRawSize = 262144;

// Descomprime um frame para `out` (redimensionado para exatamente `raw_size`). Falha (false) se
// `raw_size` estiver fora de 1..kMaxBlockRawSize, se o frame for inválido, se a saída não tiver
// exatamente `raw_size` bytes ou, para None, se `packed.size() != raw_size`. No Zstd o
// Frame_Content_Size DEVE estar presente e igual a `raw_size`, e o Dictionary_ID DEVE ser 0.
[[nodiscard]] bool decompress_block(Codec codec, std::span<const std::byte> packed,
                                    std::size_t raw_size, std::vector<std::byte>& out);

} // namespace swarmstream::pack
