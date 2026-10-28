// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/block_codec.hpp"

#include <limits>
#include <lz4.h>
#include <zstd.h>

namespace swarmstream::pack {

std::string_view codec_name(Codec codec) noexcept
{
    switch (codec) {
    case Codec::None: return "none";
    case Codec::Zstd: return "zstd";
    case Codec::Lz4: return "lz4";
    }
    return "?";
}

bool decompress_block(Codec codec, std::span<const std::byte> packed, std::size_t raw_size,
                      std::vector<std::byte>& out)
{
    out.clear();
    if (raw_size == 0 || raw_size > kMaxBlockRawSize || packed.empty()) return false;

    switch (codec) {
    case Codec::None:
        if (packed.size() != raw_size) return false;
        out.assign(packed.begin(), packed.end());
        return true;
    case Codec::Zstd: {
        const unsigned long long content = ZSTD_getFrameContentSize(packed.data(), packed.size());
        if (content != raw_size || ZSTD_getDictID_fromFrame(packed.data(), packed.size()) != 0)
            return false;
        out.resize(raw_size);
        const std::size_t n = ZSTD_decompress(out.data(), raw_size, packed.data(), packed.size());
        if (ZSTD_isError(n) || n != raw_size) {
            out.clear();
            return false;
        }
        return true;
    }
    case Codec::Lz4: {
        if (packed.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        out.resize(raw_size);
        const int n = LZ4_decompress_safe(
            reinterpret_cast<const char*>(packed.data()), reinterpret_cast<char*>(out.data()),
            static_cast<int>(packed.size()), static_cast<int>(raw_size));
        if (n < 0 || static_cast<std::size_t>(n) != raw_size) {
            out.clear();
            return false;
        }
        return true;
    }
    }
    return false;
}

} // namespace swarmstream::pack
