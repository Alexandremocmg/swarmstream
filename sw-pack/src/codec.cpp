// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

namespace {

constexpr std::array<std::string_view, 25> kNoneExt = {
    "png", "jpg", "jpeg", "webp", "avif", "ogg",  "oga",  "ogv", "opus",
    "mp3", "aac", "flac", "mp4",  "webm", "mkv",  "zip",  "gz",  "zst",
    "lz4", "7z",  "rar",  "xz",   "bz2",  "ktx2", "basis"};
constexpr std::array<std::string_view, 13> kLz4Ext = {
    "res", "mesh", "wav", "dds", "bin", "pck", "scn", "exr", "hdr", "glb", "bank", "dat", "ctex"};

template <std::size_t N>
bool in(const std::array<std::string_view, N>& list, std::string_view ext)
{
    return std::find(list.begin(), list.end(), ext) != list.end();
}

} // namespace

Codec codec_for_extension(std::string_view extension) noexcept
{
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    std::array<char, 16> lower{};
    if (extension.size() > lower.size()) return Codec::Zstd;
    for (std::size_t i = 0; i < extension.size(); ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
    const std::string_view ext(lower.data(), extension.size());
    if (in(kNoneExt, ext)) return Codec::None;
    if (in(kLz4Ext, ext)) return Codec::Lz4;
    return Codec::Zstd;
}

Codec codec_for_path(std::string_view path) noexcept
{
    const auto slash = path.find_last_of("/\\");
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == name.size()) return Codec::Zstd;
    return codec_for_extension(name.substr(dot + 1));
}

CompressResult compress_block(std::span<const std::byte> raw, Codec wanted,
                              std::vector<std::byte>& out, const CodecOptions& options)
{
    out.clear();
    if (raw.empty() || raw.size() > kMaxBlockRawSize) return {false, Codec::None};

    std::size_t packed = 0;
    if (wanted == Codec::Zstd) {
        out.resize(ZSTD_compressBound(raw.size()));
        // ZSTD_compress grava Frame_Content_Size e não usa dicionário.
        const std::size_t n =
            ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), options.zstd_level);
        packed = ZSTD_isError(n) ? 0 : n;
    } else if (wanted == Codec::Lz4) {
        const int bound = LZ4_compressBound(static_cast<int>(raw.size()));
        out.resize(static_cast<std::size_t>(bound));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
                                           reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(raw.size()), bound);
        packed = n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    if (packed == 0 || packed >= raw.size()) {
        out.assign(raw.begin(), raw.end());
        return {true, Codec::None};
    }
    out.resize(packed);
    return {true, wanted};
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
