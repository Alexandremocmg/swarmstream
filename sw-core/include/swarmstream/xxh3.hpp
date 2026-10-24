// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// XXH3-64 (semente 0, segredo padrão): o checksum local do cache (ADR-0013, docs/04). Protege
// contra o disco, NÃO contra atacante: nada vindo da rede é aceito por XXH3.
//
// Implementação própria, escalar, escrita a partir da especificação pública do algoritmo
// (xxHash 0.8). Os testes conferem a saída contra a biblioteca de referência em todos os
// ramos de tamanho (0, 1–3, 4–8, 9–16, 17–128, 129–240 e > 240 bytes).
// Não aloca e não usa estado global mutável: pode ser chamada de qualquer thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace swarmstream {

[[nodiscard]] uint64_t xxh3_64(std::span<const std::byte> data) noexcept;

[[nodiscard]] inline uint64_t xxh3_64(const void* data, size_t size) noexcept
{
    return xxh3_64(std::span<const std::byte>(static_cast<const std::byte*>(data), size));
}

} // namespace swarmstream
