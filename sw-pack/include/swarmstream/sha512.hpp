// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// SHA-512 (FIPS 180-4), pequeno e sem dependências. Existe só porque o Ed25519 (RFC 8032) o usa;
// o formato de conteúdo continua com SHA-256 (docs/03 §2).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace swarmstream::pack {

using Sha512Digest = std::array<std::uint8_t, 64>;

class Sha512
{
public:
    Sha512() noexcept;

    void update(std::span<const std::byte> data) noexcept;

    // Termina o cálculo e devolve o resumo. Depois disso o objeto não deve receber `update`.
    [[nodiscard]] Sha512Digest finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint64_t, 8> state_;
    std::array<std::uint8_t, 128> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] Sha512Digest sha512(std::span<const std::byte> data) noexcept;

} // namespace swarmstream::pack
