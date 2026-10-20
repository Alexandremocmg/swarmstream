// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// SHA-256 (FIPS 180-4), pequeno e sem dependências. Usado hoje para derivar a tabela gear
// do chunker (docs/03 §11.1); o `sw-pack` o usará para os hashes de bloco e da Merkle da peça.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace swarmstream::pack {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256
{
public:
    Sha256() noexcept;

    void update(std::span<const std::byte> data) noexcept;
    void update(std::string_view text) noexcept;

    // Termina o cálculo e devolve o resumo. Depois disso o objeto não deve receber `update`.
    [[nodiscard]] Sha256Digest finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;

// Minúsculas, 2 dígitos por byte.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

} // namespace swarmstream::pack
