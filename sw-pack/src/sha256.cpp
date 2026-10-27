// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <sodium.h>

namespace swarmstream::pack {

namespace {

static_assert(sizeof(crypto_hash_sha256_state) <= sizeof(Sha256Digest) * 4);
static_assert(alignof(crypto_hash_sha256_state) <= 16);
static_assert(crypto_hash_sha256_BYTES == 32);

crypto_hash_sha256_state* state_of(unsigned char* storage) noexcept
{
    return reinterpret_cast<crypto_hash_sha256_state*>(storage);
}

// sodium_init() é seguro de chamar várias vezes e de várias threads; devolve -1 se falhar.
void require_sodium() noexcept
{
    static const bool ready = sodium_init() >= 0;
    if (!ready) {
        std::fputs("sw-pack: sodium_init() falhou; sem SHA-256 confiavel, abortando\n", stderr);
        std::abort();
    }
}

} // namespace

Sha256::Sha256() noexcept : state_{}
{
    require_sodium();
    crypto_hash_sha256_init(state_of(state_));
}

void Sha256::update(std::span<const std::byte> data) noexcept
{
    crypto_hash_sha256_update(state_of(state_), reinterpret_cast<const unsigned char*>(data.data()),
                              data.size());
}

void Sha256::update(std::string_view text) noexcept
{
    crypto_hash_sha256_update(state_of(state_), reinterpret_cast<const unsigned char*>(text.data()),
                              text.size());
}

Sha256Digest Sha256::finish() noexcept
{
    Sha256Digest digest{};
    crypto_hash_sha256_final(state_of(state_), digest.data());
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) noexcept
{
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

std::string to_hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

} // namespace swarmstream::pack
