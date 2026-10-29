// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-17: o CONTEÚDO conhecido das fixtures do VFS: tamanhos dos blocos, bytes de cada bloco e a
// lista de arquivos (caminho, classe, grupo, blocos referenciados). É a fonte única: o gerador
// (sw-pack/tools/gen_vfs_fixture.cpp) monta o manifesto assinado a partir daqui, e o teste usa os
// mesmos dados para reconstruir os bytes dos arquivos. Só cabeçalho, sem dependências.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swarmstream::test::vfs_fixture {

inline constexpr std::array<std::uint32_t, 9> kBlockRawSize = {1000, 4096, 4096, 4096, 3000,
                                                               5000, 2048, 1,    700};
// Blocos 0..7 estão na peça 0 (grupo core); o bloco 8, na peça 1 (grupo `extra`).
inline constexpr std::size_t kCoreBlocks = 8;
inline constexpr std::uint32_t kPackedCore = 1000 + 4096 * 3 + 3000 + 5000 + 2048 + 1;

// Byte `i` do bloco `block` (descomprimido): pseudoaleatório determinístico (SplitMix64 por 8
// bytes).
inline std::uint8_t block_byte(std::uint32_t block, std::uint32_t i)
{
    std::uint64_t z = (static_cast<std::uint64_t>(block) << 32) + (i / 8) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<std::uint8_t>(z >> (8 * (i % 8)));
}

inline std::vector<std::uint8_t> block_bytes(std::uint32_t block)
{
    std::vector<std::uint8_t> out(kBlockRawSize[block]);
    for (std::uint32_t i = 0; i < out.size(); ++i) {
        out[i] = block_byte(block, i);
    }
    return out;
}

struct FileDef
{
    std::string path;
    int file_class; // 0 Core, 1 Group, 2 Optional (igual a pack::FileClass)
    std::uint16_t group;
    std::vector<std::uint32_t> refs; // índices na tabela de blocos
};

// Ordenados por caminho em bytes (como o manifesto exige). Cobre: arquivo vazio; arquivo de 1
// bloco; arquivos de 2, 3 e 4 blocos; blocos repetidos (dedup, no mesmo arquivo e entre arquivos);
// "sub" / "sub-1.bin" / "sub.bin" / "sub/deep.bin" (ordem de diretório diferente da do manifesto);
// caminho com byte não-ASCII (U+00E1, NFC); arquivos do grupo `extra` (Group e Optional).
inline std::vector<FileDef> files()
{
    return {
        {"config.ini", 0, 0, {0}},
        {"empty.bin", 0, 0, {}},
        {"maps/level1.bin", 0, 0, {1, 2, 3, 4}},
        {"maps/level2.bin", 0, 0, {5, 6}},
        {"maps/sub-1.bin", 0, 0, {4}},
        {"maps/sub.bin", 0, 0, {7}},
        {"maps/sub/deep.bin", 0, 0, {0, 7, 1}},
        {"maps/zeta.bin", 0, 0, {6}},
        {"maps/\xC3\xA1"
         "baco.bin",
         0,
         0,
         {3}},
        {"textures/a.bin", 0, 0, {2, 2}},
        {"textures/g.bin", 1, 1, {8}},
        {"textures/opt.bin", 2, 1, {8, 0}},
    };
}

inline std::uint64_t file_size_of(const FileDef& f)
{
    std::uint64_t s = 0;
    for (const std::uint32_t b : f.refs) {
        s += kBlockRawSize[b];
    }
    return s;
}

// Bytes do arquivo, concatenando os blocos conhecidos: a verdade contra a qual o teste compara.
inline std::vector<std::uint8_t> file_bytes(const FileDef& f)
{
    std::vector<std::uint8_t> out;
    for (const std::uint32_t b : f.refs) {
        const auto bytes = block_bytes(b);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

} // namespace swarmstream::test::vfs_fixture
