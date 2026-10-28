// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// SHA-256 sobre o libsodium (`crypto_hash_sha256_*`): a criptografia é do libsodium, não nossa
// (decisão do dono, 2026-10-27). Usado para derivar a tabela gear do chunker (docs/03 §11.1), para
// o key_id e o manifest_id do manifesto, e pelos hashes de bloco e da Merkle da peça.
//
// Se o libsodium não iniciar (`sodium_init() < 0`) não há como calcular hash algum: o programa
// termina com uma mensagem em vez de devolver um resumo errado.
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
    // Guarda o crypto_hash_sha256_state do libsodium (104 bytes; o .cpp confere o tamanho).
    alignas(16) unsigned char state_[128];
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;

// Minúsculas, 2 dígitos por byte.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

} // namespace swarmstream::pack
