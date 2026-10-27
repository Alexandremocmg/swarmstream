// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ed25519 puro (RFC 8032 §5.1), sem dependências: assina e verifica o envelope do manifesto
// (docs/03 §7.1, ADR-0012). A aritmética segue o TweetNaCl (domínio público), com as constantes
// recalculadas das definições e conferida pelos vetores do RFC 8032 e do vectors-v0.json.
//
// Limites conhecidos: tempo constante só na aritmética de campo e na multiplicação escalar
// (sem desvios por segredo), sem garantia de limpeza de memória. É código de ferramenta de
// build e de leitor que verifica com chave pública; a chave privada só existe no `sw-pack`.
// A verificação é estrita: recusa S >= L (maleabilidade) e R não canônico.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace swarmstream::pack {

using Ed25519Seed = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

// A mensagem é a concatenação das partes, na ordem (evita copiar um payload de dezenas de MB).
using MessageParts = std::initializer_list<std::span<const std::byte>>;

[[nodiscard]] Ed25519PublicKey ed25519_public_key(const Ed25519Seed& seed) noexcept;

[[nodiscard]] Ed25519Signature ed25519_sign(const Ed25519Seed& seed, MessageParts message) noexcept;

// true só se a assinatura for válida para a chave e a mensagem.
[[nodiscard]] bool ed25519_verify(const Ed25519PublicKey& public_key,
                                  const Ed25519Signature& signature, MessageParts message) noexcept;

} // namespace swarmstream::pack
