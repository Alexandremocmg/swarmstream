// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/ed25519_verify.hpp"

#include "swarmstream/crypto.hpp"

#include <sodium.h>

namespace swarmstream::pack {

namespace {

static_assert(crypto_sign_PUBLICKEYBYTES == 32);
static_assert(crypto_sign_BYTES == 64);

} // namespace

bool ed25519_verify(const Ed25519PublicKey& public_key, const Ed25519Signature& signature,
                    MessageParts message) noexcept
{
    if (!crypto_ready()) {
        return false;
    }
    try {
        const detail::ContiguousMessage m(message);
        return crypto_sign_verify_detached(signature.data(), m.data, m.size, public_key.data()) ==
               0;
    } catch (...) {
        return false;
    }
}

} // namespace swarmstream::pack
