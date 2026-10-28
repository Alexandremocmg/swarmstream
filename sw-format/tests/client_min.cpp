// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Binário mínimo de CLIENTE (F1-61): liga SÓ o `sw-format` e usa o que um cliente usa, o leitor do
// manifesto (`ManifestView::open`) e `decompress_block`. Existe para o teste `sw-format-symbols`
// inspecionar os símbolos do executável; por isso não inclui nada do `sw-pack`.
//
// Roda também como teste de fumaça: descomprime dois quadros reais (Zstd e LZ4, gerados pelo
// `compress_block` do sw-pack e colados aqui) e confere que o leitor recusa lixo.

#include "swarmstream/block_codec.hpp"
#include "swarmstream/crypto.hpp"
#include "swarmstream/manifest_view.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace sf = swarmstream::pack;

namespace {

constexpr std::size_t kRawSize = 192; // "swarmstream-block-" repetido e cortado em 192 bytes

// Quadro Zstd (Frame_Content_Size = 192, sem dicionário) do bloco acima.
constexpr std::array<std::uint8_t, 34> kZstdFrame = {
    0x28, 0xb5, 0x2f, 0xfd, 0x20, 0xc0, 0xcd, 0x00, 0x00, 0x90, 0x73, 0x77,
    0x61, 0x72, 0x6d, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2d, 0x62, 0x6c,
    0x6f, 0x63, 0x6b, 0x2d, 0x01, 0x00, 0x56, 0x45, 0xcd, 0x30};

// O mesmo bloco em LZ4 (block format).
constexpr std::array<std::uint8_t, 29> kLz4Block = {
    0xff, 0x03, 0x73, 0x77, 0x61, 0x72, 0x6d, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2d, 0x62,
    0x6c, 0x6f, 0x63, 0x6b, 0x2d, 0x12, 0x00, 0x96, 0x50, 0x72, 0x65, 0x61, 0x6d, 0x2d};

bool expected_block(const std::vector<std::byte>& got)
{
    static constexpr char kUnit[] = "swarmstream-block-";
    if (got.size() != kRawSize) {
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (static_cast<char>(got[i]) != kUnit[i % (sizeof kUnit - 1)]) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
std::span<const std::byte> as_bytes(const std::array<std::uint8_t, N>& a)
{
    return std::as_bytes(std::span<const std::uint8_t>(a));
}

bool check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FALHOU", what);
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= check(sf::crypto_ready(), "crypto_ready");

    std::vector<std::byte> out;
    ok &= check(sf::decompress_block(sf::Codec::Zstd, as_bytes(kZstdFrame), kRawSize, out) &&
                    expected_block(out),
                "decompress_block Zstd");
    ok &= check(sf::decompress_block(sf::Codec::Lz4, as_bytes(kLz4Block), kRawSize, out) &&
                    expected_block(out),
                "decompress_block LZ4");
    ok &= check(!sf::decompress_block(sf::Codec::Zstd, as_bytes(kLz4Block), kRawSize, out),
                "decompress_block recusa quadro invalido");

    // Lixo no lugar do manifesto: o leitor tem de recusar (nunca aceitar, nunca travar).
    std::array<std::byte, 256> junk{};
    for (std::size_t i = 0; i < junk.size(); ++i) {
        junk[i] = static_cast<std::byte>(i * 7 + 1);
    }
    sf::ManifestView view;
    const sf::ReadOptions options{};
    const sf::ManifestStatus status = sf::ManifestView::open(junk, options, view);
    ok &= check(status.code == sf::ManifestError::Invalid, "ManifestView::open recusa lixo");

    std::printf("%s\n", ok ? "SW-FORMAT CLIENTE MINIMO: OK" : "SW-FORMAT CLIENTE MINIMO: FALHOU");
    return ok ? 0 : 1;
}
