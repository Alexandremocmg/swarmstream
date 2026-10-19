// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/chunker.hpp"

#include "swarmstream/sha256.hpp"

#include <algorithm>
#include <bit>

namespace swarmstream::pack {

namespace {

GearTable build_gear_table() noexcept
{
    static constexpr std::string_view kPrefix = "swarmstream-gear-v0";
    GearTable table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        Sha256 hasher;
        hasher.update(kPrefix);
        const std::byte index{static_cast<unsigned char>(i)};
        hasher.update(std::span<const std::byte>(&index, 1));
        const Sha256Digest digest = hasher.finish();
        std::uint64_t value = 0;
        for (std::size_t k = 0; k < 8; ++k) { // little-endian dos 8 primeiros bytes
            value |= std::uint64_t{digest[k]} << (8U * k);
        }
        table[i] = value;
    }
    return table;
}

// Os `bits` bits mais significativos de 64 em 1 (1 <= bits <= 64).
constexpr std::uint64_t high_bits_mask(unsigned bits) noexcept
{
    return ~std::uint64_t{0} << (64U - bits);
}

} // namespace

const GearTable& Chunker::gear_table() noexcept
{
    static const GearTable table = build_gear_table(); // inicialização thread-safe
    return table;
}

std::optional<Chunker> Chunker::make(ChunkerParams params) noexcept
{
    if (params.min_size == 0 || params.min_size > params.avg_size ||
        params.avg_size > params.max_size || !std::has_single_bit(params.avg_size) ||
        params.avg_size < 8) {
        return std::nullopt;
    }
    const auto log2_avg = static_cast<unsigned>(std::bit_width(params.avg_size) - 1);
    (void)gear_table(); // garante a tabela pronta antes do primeiro corte
    return Chunker(params, high_bits_mask(log2_avg + 2), high_bits_mask(log2_avg - 2));
}

std::size_t Chunker::cut(std::span<const std::byte> data) const noexcept
{
    const std::size_t remaining = data.size();
    if (remaining <= params_.min_size) {
        return remaining;
    }
    const std::size_t limit = std::min<std::size_t>(remaining, params_.max_size);
    const std::size_t normal = std::min<std::size_t>(params_.avg_size, limit);

    const std::uint64_t* gear = gear_table().data();
    const std::byte* bytes = data.data();
    std::uint64_t fingerprint = 0; // o hash começa em `min_size`; as posições antes não entram
    std::size_t i = params_.min_size;

    for (; i < normal; ++i) {
        fingerprint = (fingerprint << 1) + gear[std::to_integer<std::size_t>(bytes[i])];
        if ((fingerprint & mask_small_) == 0) {
            return i + 1; // o bloco inclui o byte que disparou o corte
        }
    }
    for (; i < limit; ++i) {
        fingerprint = (fingerprint << 1) + gear[std::to_integer<std::size_t>(bytes[i])];
        if ((fingerprint & mask_large_) == 0) {
            return i + 1;
        }
    }
    return limit;
}

std::vector<std::size_t> Chunker::split(std::span<const std::byte> data) const
{
    std::vector<std::size_t> lengths;
    lengths.reserve(data.size() / params_.avg_size + 1);
    while (!data.empty()) {
        const std::size_t length = cut(data); // >= 1 enquanto houver dados (max_size >= 1)
        lengths.push_back(length);
        data = data.subspan(length);
    }
    return lengths;
}

} // namespace swarmstream::pack
