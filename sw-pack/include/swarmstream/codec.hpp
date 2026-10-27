// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Compressão por bloco (docs/03 §4): NONE, ZSTD (frame padrão, com Frame_Content_Size, sem
// dicionário) e LZ4 (block format). A escolha do codec vem do tipo de arquivo (F1-03).
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

struct CodecOptions
{
    int zstd_level = 3;
};

// Codec sugerido para um arquivo, só pela extensão (sem diferenciar maiúsculas):
//  - formatos que já vêm comprimidos (png, jpg, ogg, mp4, zip...): None (tentar só gasta tempo);
//  - binários de engine e áudio/mesh brutos (res, mesh, wav, dds, bin, pck...): Lz4 (decodifica
//    mais rápido que o Zstd, taxa menor);
//  - texto, dados estruturados e desconhecidos (json, tscn, gd, glsl...): Zstd (melhor taxa).
// É um ponto de partida; os números reais saem do relatório da CLI (`sw-pack-report`).
[[nodiscard]] Codec codec_for_extension(std::string_view extension) noexcept;
// Idem, a partir de um caminho ("a/b/c.PNG" -> extensão "png").
[[nodiscard]] Codec codec_for_path(std::string_view path) noexcept;

struct CompressResult
{
    bool ok = false;
    Codec used = Codec::None;
};

// Comprime um bloco bruto (1..kMaxBlockRawSize bytes) com o codec pedido e o devolve em `out`
// (substitui o conteúdo). `used` é o codec realmente usado: se a compressão não reduzir o
// tamanho, ou falhar, o bloco sai como None (cópia, docs/03 §4). Bloco vazio ou grande demais:
// ok = false e `out` fica vazio.
[[nodiscard]] CompressResult compress_block(std::span<const std::byte> raw, Codec wanted,
                                            std::vector<std::byte>& out,
                                            const CodecOptions& options = {});

// Descomprime um frame para `out` (redimensionado para exatamente `raw_size`). Falha (false) se
// `raw_size` estiver fora de 1..kMaxBlockRawSize, se o frame for inválido, se a saída não tiver
// exatamente `raw_size` bytes ou, para None, se `packed.size() != raw_size`. No Zstd o
// Frame_Content_Size DEVE estar presente e igual a `raw_size`, e o Dictionary_ID DEVE ser 0.
[[nodiscard]] bool decompress_block(Codec codec, std::span<const std::byte> packed,
                                    std::size_t raw_size, std::vector<std::byte>& out);

} // namespace swarmstream::pack
