// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Testes da compressão por bloco (F1-03, docs/03 §4).

#include "check.hpp"
#include "swarmstream/codec.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>
#include <zstd.h>

namespace sp = swarmstream::pack;
using Bytes = std::vector<std::byte>;

namespace {

Bytes from_hex(std::string_view hex)
{
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::byte>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
    return out;
}

Bytes pseudo_random(std::size_t n, std::uint64_t seed)
{
    Bytes out(n);
    for (auto& b : out) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        b = static_cast<std::byte>(seed >> 56);
    }
    return out;
}

Bytes repetitive(std::size_t n)
{
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::byte>("swarmstream "[i % 12]);
    return out;
}

void test_roundtrip()
{
    for (const sp::Codec codec : {sp::Codec::Zstd, sp::Codec::Lz4}) {
        for (const std::size_t n : {std::size_t{1}, std::size_t{100}, std::size_t{4096},
                                    std::size_t{131072}, sp::kMaxBlockRawSize}) {
            const Bytes raw = repetitive(n);
            Bytes packed, back;
            const auto r = sp::compress_block(raw, codec, packed);
            SW_CHECK(r.ok);
            if (n >= 100) {
                SW_CHECK(r.used == codec);
                SW_CHECK(packed.size() < raw.size());
            }
            SW_CHECK(sp::decompress_block(r.used, packed, n, back));
            SW_CHECK(back == raw);
        }
    }
}

void test_incompressible_falls_back_to_none()
{
    const Bytes raw = pseudo_random(65536, 7);
    for (const sp::Codec codec : {sp::Codec::Zstd, sp::Codec::Lz4}) {
        Bytes packed, back;
        const auto r = sp::compress_block(raw, codec, packed);
        SW_CHECK(r.ok && r.used == sp::Codec::None);
        SW_CHECK(packed == raw);
        SW_CHECK(sp::decompress_block(sp::Codec::None, packed, raw.size(), back));
        SW_CHECK(back == raw);
    }
    // Codec None pedido: cópia.
    Bytes packed;
    const auto r = sp::compress_block(raw, sp::Codec::None, packed);
    SW_CHECK(r.ok && r.used == sp::Codec::None && packed == raw);
}

void test_limits()
{
    Bytes packed;
    SW_CHECK(!sp::compress_block({}, sp::Codec::Zstd, packed).ok);
    SW_CHECK(packed.empty());
    const Bytes big(sp::kMaxBlockRawSize + 1);
    SW_CHECK(!sp::compress_block(big, sp::Codec::Lz4, packed).ok);
    Bytes back;
    SW_CHECK(!sp::decompress_block(sp::Codec::None, big, big.size(), back));
    SW_CHECK(!sp::decompress_block(sp::Codec::Zstd, Bytes{}, 10, back));
    SW_CHECK(!sp::decompress_block(sp::Codec::Zstd, big, 0, back));
}

void test_zstd_frame_properties()
{
    const Bytes raw = repetitive(50000);
    Bytes packed;
    const auto r = sp::compress_block(raw, sp::Codec::Zstd, packed);
    SW_CHECK(r.ok && r.used == sp::Codec::Zstd);
    SW_CHECK(ZSTD_getFrameContentSize(packed.data(), packed.size()) == raw.size());
    SW_CHECK(ZSTD_getDictID_fromFrame(packed.data(), packed.size()) == 0);
}

void test_wrong_size_and_corruption_rejected()
{
    const Bytes raw = repetitive(20000);
    for (const sp::Codec codec : {sp::Codec::Zstd, sp::Codec::Lz4}) {
        Bytes packed, back;
        const auto r = sp::compress_block(raw, codec, packed);
        SW_CHECK(r.ok && r.used == codec);
        SW_CHECK(!sp::decompress_block(codec, packed, raw.size() - 1, back));
        SW_CHECK(!sp::decompress_block(codec, packed, raw.size() + 1, back));
        // Frame truncado.
        const Bytes cut(packed.begin(), packed.end() - 3);
        SW_CHECK(!sp::decompress_block(codec, cut, raw.size(), back));
        SW_CHECK(back.empty());
    }
    // None com tamanho diferente.
    Bytes back;
    SW_CHECK(!sp::decompress_block(sp::Codec::None, raw, raw.size() - 1, back));
}

// Vetores de docs/03 (vectors-v0.json, caso zeros_4096): frames feitos pela implementação de
// referência, decodificados aqui pelas bibliotecas do C++.
void test_reference_vectors()
{
    const Bytes zeros(4096);
    Bytes back;
    const Bytes zstd = from_hex("28b52ffd60000f4d00001000000100fbf70116");
    SW_CHECK(sp::decompress_block(sp::Codec::Zstd, zstd, 4096, back));
    SW_CHECK(back == zeros);
    const Bytes lz4 = from_hex("1f000100fffffffffffffffffffffffffffffff6500000000000");
    SW_CHECK(sp::decompress_block(sp::Codec::Lz4, lz4, 4096, back));
    SW_CHECK(back == zeros);
}

void test_policy_by_extension()
{
    SW_CHECK(sp::codec_for_path("assets/tex/hero.PNG") == sp::Codec::None);
    SW_CHECK(sp::codec_for_path("a\\b\\music.ogg") == sp::Codec::None);
    SW_CHECK(sp::codec_for_path("scenes/level1.tscn") == sp::Codec::Zstd);
    SW_CHECK(sp::codec_for_path("data/items.json") == sp::Codec::Zstd);
    SW_CHECK(sp::codec_for_path("mesh/rock.mesh") == sp::Codec::Lz4);
    SW_CHECK(sp::codec_for_path("audio/step.wav") == sp::Codec::Lz4);
    SW_CHECK(sp::codec_for_path("LICENSE") == sp::Codec::Zstd);
    SW_CHECK(sp::codec_for_path("dir.png/arquivo") == sp::Codec::Zstd);
    SW_CHECK(sp::codec_for_path("x.") == sp::Codec::Zstd);
    SW_CHECK(sp::codec_for_extension(".Png") == sp::Codec::None);
    SW_CHECK(sp::codec_for_extension("extensaomuitolongademais") == sp::Codec::Zstd);
}

} // namespace

int main()
{
    test_roundtrip();
    test_incompressible_falls_back_to_none();
    test_limits();
    test_zstd_frame_properties();
    test_wrong_size_and_corruption_rejected();
    test_reference_vectors();
    test_policy_by_extension();
    std::puts("sw-pack-codec: OK");
    return 0;
}
