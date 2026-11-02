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

// ---------------------------------------------------------------------------------------------
// F1-64: fixtures de reforço do VFS. Os dados abaixo são a fonte única, como acima.

// Bloco de `raw_size` bytes com a mesma função de bytes de `block_bytes`, para as tabelas de
// tamanho próprias das fixtures abaixo (a de cima indexa só `kBlockRawSize`).
inline std::vector<std::uint8_t> block_bytes_sized(std::uint32_t block, std::uint32_t raw_size)
{
    // Os 8 bytes de cada palavra vêm de UM cálculo do SplitMix64 (os mesmos bytes de `block_byte`,
    // que refaz o cálculo para cada byte): blocos de 256 KiB ficam baratos também em Debug.
    std::vector<std::uint8_t> out(raw_size);
    for (std::uint32_t i = 0; i < raw_size; i += 8) {
        std::uint64_t z =
            (static_cast<std::uint64_t>(block) << 32) + (i / 8) + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        for (std::uint32_t k = 0; k < 8 && i + k < raw_size; ++k) {
            out[i + k] = static_cast<std::uint8_t>(z >> (8 * k));
        }
    }
    return out;
}

inline std::uint64_t file_size_of(const FileDef& f, const std::uint32_t* raw_sizes)
{
    std::uint64_t s = 0;
    for (const std::uint32_t b : f.refs) {
        s += raw_sizes[b];
    }
    return s;
}

// --- Caminhos com bytes >= 0x80 logo depois do prefixo de um diretório -------------------------
// TODOS os caminhos são UTF-8 válido e canônico (NFC, sem `.`/`..`, sem byte isolado inválido): o
// §6 do docs/03 e o ManifestView::open exigem isso. Já em ordem bytewise (a do manifesto). Os
// irmãos "mapsá.bin" (C3 A1), "mapsあ.bin" (E3 81 82) e "maps😀.bin" (F0 9F 98 80) vêm DEPOIS de
// "maps/..." porque 0xC3/0xE3/0xF0 > '/' (0x2F) sem sinal; um irmão ASCII com byte < '/'
// ("maps-1.bin") vem antes e um com byte > '/' ("maps0.bin"), logo depois. O mesmo para "zeta" e
// para o diretório "ñ" (C3 B1), cujos irmãos têm byte alto depois do nome inteiro.
inline std::vector<std::string> high_byte_paths()
{
    return {
        "a/x.bin",
        "maps-1.bin",
        "maps/a.bin",
        "maps/b.bin",
        "maps0.bin",
        "maps\xC3\xA1"
        ".bin",
        "maps\xE3\x81\x82"
        ".bin",
        "maps\xF0\x9F\x98\x80"
        ".bin",
        "zeta/q.bin",
        "zeta\xC3\xA9"
        ".bin",
        "\xC3\xB1/x.bin",
        "\xC3\xB1\xC3\xA1"
        ".bin",
        "\xC3\xB1\xE3\x81\x82"
        ".bin",
    };
}
// Todo arquivo da fixture de bytes altos tem 1 bloco de 10 bytes (o bloco 0).
inline constexpr std::uint32_t kHighByteFileSize = 10;

// --- Bloco de 262144 bytes (limite do §8 item 5) ------------------------------------------------
// b0 e b1 têm o tamanho máximo; b2 tem 1 byte; b3, 262143 (um a menos que o máximo); b4, 131072;
// b5, 100 (bloco curto de cauda). Peça 0 = b0..b2 (524289 bytes), peça 1 = b3..b5 (393315).
inline constexpr std::array<std::uint32_t, 6> kMaxBlockRawSize = {262144, 262144, 1,
                                                                  262143, 131072, 100};
inline constexpr std::size_t kMaxBlockFirstPieceBlocks = 3;
inline std::vector<FileDef> max_block_files()
{
    return {
        {"max/exact.bin", 0, 0, {0}},
        {"max/mix.bin", 0, 0, {3, 0, 2, 1, 5}},
        {"max/pair.bin", 0, 0, {0, 1}},
        {"max/tail.bin", 0, 0, {4, 0, 5}},
    };
}

// --- Arquivos lógicos de mais de 4 GiB, só descritos pelo manifesto -----------------------------
// Quatro blocos distintos; os arquivos os referenciam muitas vezes (nenhum dado em disco).
inline constexpr std::array<std::uint32_t, 4> kHugeBlockRawSize = {262144, 262143, 1, 1000};
inline constexpr std::uint32_t kHugeB0 = 0; // 262144 (máximo)
inline constexpr std::uint32_t kHugeB1 = 1; // 262143
inline constexpr std::uint32_t kHugeB2 = 2; // 1
inline constexpr std::uint32_t kHugeB3 = 3; // 1000
// 16384 blocos máximos somam exatamente 2^32.
inline constexpr std::uint32_t kHugeFullBlocks = 16384;

// "huge/boundary.bin": B0 x 16383, B1, B2, B2, B2, B0. As somas acumuladas valem 2^32 - 1 depois de
// B1, 2^32 depois do primeiro B2 e 2^32 + 1 depois do segundo, 2^32 + 2 depois do terceiro: as
// referências B2 começam em 2^32 - 1, 2^32 e 2^32 + 1.
// "huge/mid.bin": B3, B0 x 16384, B1. O byte 2^32 cai DENTRO do último B0 (começa em
// 1000 + 16383 * 262144 = 4294706152; o byte 2^32 está no deslocamento 261144 do bloco).
inline std::vector<FileDef> huge_files()
{
    FileDef boundary{"huge/boundary.bin", 0, 0, {}};
    boundary.refs.assign(kHugeFullBlocks - 1, kHugeB0);
    boundary.refs.insert(boundary.refs.end(), {kHugeB1, kHugeB2, kHugeB2, kHugeB2, kHugeB0});
    FileDef mid{"huge/mid.bin", 0, 0, {kHugeB3}};
    mid.refs.insert(mid.refs.end(), kHugeFullBlocks, kHugeB0);
    mid.refs.push_back(kHugeB1);
    return {boundary, mid};
}

} // namespace swarmstream::test::vfs_fixture
