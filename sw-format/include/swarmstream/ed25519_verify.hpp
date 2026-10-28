// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Verificação Ed25519 (RFC 8032 §5.1) sobre o libsodium, para o envelope do manifesto
// (docs/03 §7.1, ADR-0012). Parte do `sw-format` (cliente): o cliente só VERIFICA; assinar é do
// `sw-pack` (`swarmstream/ed25519.hpp`). A criptografia é do libsodium, não nossa (decisão do
// dono, 2026-10-27).
//
// A verificação é a ESTRITA do libsodium (`crypto_sign_verify_detached`: recusa S >= L, R e
// chave pública de ordem pequena ou não canônicos). Qualquer falha, inclusive a do
// `sodium_init()`, é "assinatura inválida"; nunca vira aviso nem é ignorada.
#pragma once

#include "swarmstream/message_parts.hpp"

#include <array>
#include <cstdint>

namespace swarmstream::pack {

using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

// true só se a assinatura for válida para a chave e a mensagem.
[[nodiscard]] bool ed25519_verify(const Ed25519PublicKey& public_key,
                                  const Ed25519Signature& signature, MessageParts message) noexcept;

} // namespace swarmstream::pack
