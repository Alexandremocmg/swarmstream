// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Binário mínimo de CLIENTE (F1-61): liga SÓ o `sw-format` e faz o que um cliente faz: abre um
// manifesto assinado (`ManifestView::open`), descomprime um bloco (`decompress_block`) e confere o
// hash do bloco contra o manifesto. Existe para o teste `sw-format-symbols` inspecionar os símbolos
// do executável; por isso não inclui nada do `sw-pack`.
//
// Também é teste de fumaça, e agora cobre o caminho de SUCESSO e não só a recusa:
//   - abre um manifesto VÁLIDO e assinado com Ed25519 de verdade (client_min_fixture.hpp);
//   - descomprime o bloco que o manifesto descreve (Zstd) e confere SHA-256 == block_hash;
//   - descomprime um quadro LZ4;
//   - recusa: byte alterado (assinatura), chave errada e lixo.

#include "client_min_fixture.hpp"
#include "swarmstream/block_codec.hpp"
#include "swarmstream/crypto.hpp"
#include "swarmstream/manifest_view.hpp"
#include "swarmstream/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace sf = swarmstream::pack;

namespace {

constexpr std::size_t kRawSize = 192; // "swarmstream-block-" repetido e cortado em 192 bytes

// Quadro Zstd (Frame_Content_Size = 192, sem dicionário) do bloco acima: é o bloco 0 do manifesto.
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

std::vector<std::byte> manifest_copy()
{
    const auto bytes = as_bytes(swarmstream::test::kClientMinManifest);
    return std::vector<std::byte>(bytes.begin(), bytes.end());
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

    // --- caminho de SUCESSO: abre o manifesto válido e usa o que ele descreve
    const std::array<sf::Ed25519PublicKey, 1> trusted = {swarmstream::test::kClientMinPublicKey};
    sf::ReadOptions options;
    options.trusted_keys = trusted;

    const std::vector<std::byte> manifest = manifest_copy();
    sf::ManifestView view;
    const sf::ManifestStatus opened = sf::ManifestView::open(manifest, options, view);
    ok &= check(opened.ok(), "ManifestView::open abre o manifesto valido e assinado");
    if (opened.ok()) {
        ok &= check(view.game_id() == "swarmstream.clientmin" && view.build_seq() == 1,
                    "ManifestView: game_id e build_seq");
        ok &= check(view.block_count() == 1 && view.piece_count() == 1 && view.file_count() == 1 &&
                        view.group_count() == 1,
                    "ManifestView: 1 bloco, 1 peca, 1 arquivo, 1 grupo");
        ok &= check(view.file_path(0) == "dados/bloco.bin" && view.file_size(0) == kRawSize,
                    "ManifestView: arquivo dados/bloco.bin com 192 bytes");
        ok &= check(view.block_codec(0) == sf::Codec::Zstd && view.block_raw_size(0) == kRawSize &&
                        view.block_packed_size(0) == kZstdFrame.size(),
                    "ManifestView: bloco 0 e Zstd, 192 -> 34 bytes");

        // O que um cliente faz com o bloco: descomprimir como o manifesto manda e conferir o hash.
        std::vector<std::byte> raw;
        const bool decoded = sf::decompress_block(view.block_codec(0), as_bytes(kZstdFrame),
                                                  view.block_raw_size(0), raw);
        ok &= check(decoded && expected_block(raw),
                    "decompress_block Zstd (codec e tamanho do manifesto)");
        ok &= check(decoded && sf::sha256(raw) == view.block_hash(0),
                    "SHA-256 do bloco descomprimido == block_hash do manifesto");
    }

    // --- LZ4 e recusas da descompressão
    std::vector<std::byte> out;
    ok &= check(sf::decompress_block(sf::Codec::Lz4, as_bytes(kLz4Block), kRawSize, out) &&
                    expected_block(out),
                "decompress_block LZ4");
    ok &= check(!sf::decompress_block(sf::Codec::Zstd, as_bytes(kLz4Block), kRawSize, out),
                "decompress_block recusa quadro invalido");

    // --- recusas do leitor: nunca aceitar, nunca travar
    {
        std::vector<std::byte> tampered = manifest_copy();
        tampered[tampered.size() - 1] ^= std::byte{0x01}; // um byte do payload
        sf::ManifestView v;
        ok &= check(sf::ManifestView::open(tampered, options, v).code ==
                        sf::ManifestError::SignatureInvalid,
                    "ManifestView::open recusa payload alterado (assinatura)");
    }
    {
        sf::Ed25519PublicKey wrong = swarmstream::test::kClientMinPublicKey;
        wrong[0] ^= 0x01;
        const std::array<sf::Ed25519PublicKey, 1> other = {wrong};
        sf::ReadOptions wrong_options;
        wrong_options.trusted_keys = other;
        sf::ManifestView v;
        ok &= check(sf::ManifestView::open(manifest, wrong_options, v).code ==
                        sf::ManifestError::SignatureInvalid,
                    "ManifestView::open recusa chave nao confiavel");
    }
    {
        std::array<std::byte, 256> junk{};
        for (std::size_t i = 0; i < junk.size(); ++i) {
            junk[i] = static_cast<std::byte>(i * 7 + 1);
        }
        sf::ManifestView v;
        ok &= check(sf::ManifestView::open(junk, options, v).code == sf::ManifestError::Invalid,
                    "ManifestView::open recusa lixo");
    }

    std::printf("%s\n", ok ? "SW-FORMAT CLIENTE MINIMO: OK" : "SW-FORMAT CLIENTE MINIMO: FALHOU");
    return ok ? 0 : 1;
}
