// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/ed25519.hpp"

#include "swarmstream/crypto.hpp"

#include <algorithm>
#include <sodium.h>
#include <vector>

namespace swarmstream::pack {

namespace {

static_assert(crypto_sign_SEEDBYTES == 32);
static_assert(crypto_sign_PUBLICKEYBYTES == 32);
static_assert(crypto_sign_BYTES == 64);

// A mensagem contígua que o libsodium exige. Com uma só parte não copia.
struct Message
{
    std::vector<unsigned char> storage;
    const unsigned char* data = nullptr;
    unsigned long long size = 0;

    explicit Message(MessageParts parts)
    {
        if (parts.size() == 1) {
            data = reinterpret_cast<const unsigned char*>(parts.begin()->data());
            size = parts.begin()->size();
            return;
        }
        std::size_t total = 0;
        for (const auto part : parts) {
            total += part.size();
        }
        storage.reserve(total);
        for (const auto part : parts) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(part.data());
            storage.insert(storage.end(), bytes, bytes + part.size());
        }
        data = storage.data();
        size = storage.size();
    }
};

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
        const Message m(message);
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

bool ed25519_verify(const Ed25519PublicKey& public_key, const Ed25519Signature& signature,
                    MessageParts message) noexcept
{
    if (!crypto_ready()) {
        return false;
    }
    try {
        const Message m(message);
        return crypto_sign_verify_detached(signature.data(), m.data, m.size, public_key.data()) ==
               0;
    } catch (...) {
        return false;
    }
}

} // namespace swarmstream::pack
