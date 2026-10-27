// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Manifesto sintético e válido para os testes e para a medição (F1-04). Determinístico: mesma
// entrada, mesmos bytes. Não é o mapa da F0-09 (os assets não são versionados); imita a forma
// dele: blocos de 32 a 256 KiB, ~60% do tamanho depois do Zstd, peças de até 1 MiB, grupos
// `core` (~10% dos blocos) e dois grupos de região, peças de cada grupo contíguas.
#pragma once

#include "swarmstream/chunker.hpp"
#include "swarmstream/manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace swarmstream::test {

class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next()
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // [lo, hi]
    std::uint32_t between(std::uint32_t lo, std::uint32_t hi)
    {
        return lo + static_cast<std::uint32_t>(next() % (std::uint64_t{hi} - lo + 1));
    }

private:
    std::uint64_t state_;
};

inline pack::Hash32 random_hash(SplitMix64& rng)
{
    pack::Hash32 h;
    for (std::size_t i = 0; i < h.size(); i += 8) {
        const std::uint64_t v = rng.next();
        for (std::size_t b = 0; b < 8; ++b) {
            h[i + b] = static_cast<std::uint8_t>(v >> (8 * b));
        }
    }
    return h;
}

// Arquivos de 1 a `2*avg_blocks_per_file - 1` blocos; os blocos são repartidos entre os grupos
// 0 (10%), 1 (45%) e 2 (45%).
inline pack::ManifestModel make_synthetic_model(std::size_t total_blocks,
                                                std::uint32_t avg_blocks_per_file,
                                                std::uint64_t seed)
{
    using namespace pack;
    SplitMix64 rng(seed);
    ManifestModel m;
    m.game_id = "swarmstream.testmap";
    m.build_id = "0.1.0+test";
    m.build_seq = 7;
    m.created_at = 1'790'000'000;
    m.chunker = {32U * 1024U, 128U * 1024U, 256U * 1024U, std::string(Chunker::kAlgorithm)};
    m.cdn_base_urls = {"https://cdn.example.test/swarmstream"};
    m.trackers = {"https://tracker.example.test/announce"};

    const char* const group_names[3] = {"core", "regiao_norte", "regiao_sul"};
    const char* const group_dirs[3] = {"a_core", "b_norte", "c_sul"};
    const std::size_t core_blocks = total_blocks / 10;
    const std::size_t north_blocks = (total_blocks - core_blocks) / 2;
    const std::size_t counts[3] = {core_blocks, north_blocks,
                                   total_blocks - core_blocks - north_blocks};

    std::size_t file_index = 0;
    for (std::uint16_t g = 0; g < 3; ++g) {
        GroupInfo info;
        info.name = group_names[g];
        info.priority = static_cast<std::uint8_t>(g);
        info.piece_first = static_cast<std::uint32_t>(m.piece_hash.size());

        std::uint64_t piece_bytes = 0;
        std::uint32_t piece_blocks = 0;
        const auto close_piece = [&] {
            if (piece_blocks == 0) {
                return;
            }
            m.piece_hash.push_back(random_hash(rng));
            m.piece_size.push_back(static_cast<std::uint32_t>(piece_bytes));
            m.piece_first_block.push_back(static_cast<std::uint32_t>(m.block_hash.size()) -
                                          piece_blocks);
            m.piece_block_count.push_back(piece_blocks);
            m.piece_group.push_back(g);
            piece_bytes = 0;
            piece_blocks = 0;
        };

        std::size_t made = 0;
        while (made < counts[g]) {
            const std::size_t want = std::min<std::size_t>(
                rng.between(1, 2 * avg_blocks_per_file - 1), counts[g] - made);
            std::uint64_t file_size = 0;
            const auto first_ref = static_cast<std::uint32_t>(m.block_refs.size());
            for (std::size_t k = 0; k < want; ++k) {
                const std::uint32_t raw = rng.between(32U * 1024U, 256U * 1024U);
                const std::uint32_t packed = std::max<std::uint32_t>(1, raw * 6 / 10);
                if (piece_bytes + packed > kMaxPieceSize) {
                    close_piece();
                }
                m.block_refs.push_back(static_cast<std::uint32_t>(m.block_hash.size()));
                m.block_hash.push_back(random_hash(rng));
                m.block_raw_size.push_back(raw);
                m.block_packed_size.push_back(packed);
                m.block_codec.push_back(Codec::Zstd);
                piece_bytes += packed;
                ++piece_blocks;
                file_size += raw;
            }
            made += want;

            char path[64];
            std::snprintf(path, sizeof path, "%s/d%04zu/f%08zu.ctex", group_dirs[g],
                          file_index / 64, file_index);
            m.file_path.emplace_back(path);
            m.file_size.push_back(file_size);
            m.file_class.push_back(g == 0 ? FileClass::Core : FileClass::Group);
            m.file_group.push_back(g);
            m.file_first_ref.push_back(first_ref);
            m.file_ref_count.push_back(static_cast<std::uint32_t>(want));
            ++file_index;
        }
        close_piece();
        info.piece_count = static_cast<std::uint32_t>(m.piece_hash.size()) - info.piece_first;
        m.groups.push_back(std::move(info));
    }
    return m;
}

} // namespace swarmstream::test
