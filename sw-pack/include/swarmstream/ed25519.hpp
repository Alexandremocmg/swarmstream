// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Assinatura Ed25519 (RFC 8032 §5.1) sobre o libsodium, para o manifesto (docs/03 §7.1,
// ADR-0012). Ferramenta de build (`sw-pack`): o cliente só verifica
// (`swarmstream/ed25519_verify.hpp`, do `sw-format`, incluído aqui). Esta é só uma interface fina:
// a criptografia é do libsodium, não nossa (decisão do dono, 2026-10-27).
#pragma once

#include "swarmstream/ed25519_verify.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace swarmstream::pack {

using Ed25519Seed = std::array<std::uint8_t, 32>;

// nullopt se o libsodium não iniciar ou a chave não puder ser derivada.
[[nodiscard]] std::optional<Ed25519PublicKey> ed25519_public_key(const Ed25519Seed& seed) noexcept;

// nullopt se o libsodium não iniciar ou faltar memória; nunca devolve uma assinatura parcial.
[[nodiscard]] std::optional<Ed25519Signature> ed25519_sign(const Ed25519Seed& seed,
                                                           MessageParts message) noexcept;

} // namespace swarmstream::pack
