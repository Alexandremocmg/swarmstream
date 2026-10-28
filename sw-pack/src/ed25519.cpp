// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/ed25519.hpp"

#include "swarmstream/crypto.hpp"

#include <sodium.h>

namespace swarmstream::pack {

namespace {

static_assert(crypto_sign_SEEDBYTES == 32);
static_assert(crypto_sign_PUBLICKEYBYTES == 32);
static_assert(crypto_sign_BYTES == 64);

} // namespace

std::optional<Ed25519PublicKey> ed25519_public_key(const Ed25519Seed& seed) noexcept
{
    if (!crypto_ready()) {
        return std::nullopt;
    }
    Ed25519PublicKey public_key{};
    unsigned char secret_key[crypto_sign_SECRETKEYBYTES];
    const int rc = crypto_sign_seed_keypair(public_key.data(), secret_key, seed.data());
    sodium_memzero(secret_key, sizeof secret_key);
    if (rc != 0) {
        return std::nullopt;
    }
    return public_key;
}

std::optional<Ed25519Signature> ed25519_sign(const Ed25519Seed& seed, MessageParts message) noexcept
{
    if (!crypto_ready()) {
        return std::nullopt;
    }
    try {
        const detail::ContiguousMessage m(message);
        unsigned char public_key[crypto_sign_PUBLICKEYBYTES];
        unsigned char secret_key[crypto_sign_SECRETKEYBYTES];
        if (crypto_sign_seed_keypair(public_key, secret_key, seed.data()) != 0) {
            sodium_memzero(secret_key, sizeof secret_key);
            return std::nullopt;
        }
        Ed25519Signature signature{};
        unsigned long long length = 0;
        const int rc = crypto_sign_detached(signature.data(), &length, m.data, m.size, secret_key);
        sodium_memzero(secret_key, sizeof secret_key);
        if (rc != 0 || length != signature.size()) {
            return std::nullopt;
        }
        return signature;
    } catch (...) { // falta de memória na cópia da mensagem
        return std::nullopt;
    }
}

} // namespace swarmstream::pack
