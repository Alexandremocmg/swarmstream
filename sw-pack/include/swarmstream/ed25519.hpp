// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ed25519 (RFC 8032 §5.1) sobre o libsodium: assina e verifica o envelope do manifesto
// (docs/03 §7.1, ADR-0012). Esta é só uma interface fina: a criptografia é do libsodium, não
// nossa (decisão do dono, 2026-10-27).
//
// A verificação é a ESTRITA do libsodium (`crypto_sign_verify_detached`: recusa S >= L, R e
// chave pública de ordem pequena ou não canônicos). Qualquer falha, inclusive a do
// `sodium_init()`, é "assinatura inválida"; nunca vira aviso nem é ignorada.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>

namespace swarmstream::pack {

using Ed25519Seed = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

// A mensagem é a concatenação das partes, na ordem. O libsodium assina uma mensagem contígua:
// com mais de uma parte, a implementação monta uma cópia temporária (ver docs/medicoes).
using MessageParts = std::initializer_list<std::span<const std::byte>>;

// nullopt se o libsodium não iniciar ou a chave não puder ser derivada.
[[nodiscard]] std::optional<Ed25519PublicKey> ed25519_public_key(const Ed25519Seed& seed) noexcept;

// nullopt se o libsodium não iniciar ou faltar memória; nunca devolve uma assinatura parcial.
[[nodiscard]] std::optional<Ed25519Signature> ed25519_sign(const Ed25519Seed& seed,
                                                           MessageParts message) noexcept;

// true só se a assinatura for válida para a chave e a mensagem.
[[nodiscard]] bool ed25519_verify(const Ed25519PublicKey& public_key,
                                  const Ed25519Signature& signature, MessageParts message) noexcept;

} // namespace swarmstream::pack
