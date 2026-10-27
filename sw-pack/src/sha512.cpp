// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/sha512.hpp"

#include <algorithm>
#include <cstring>

namespace swarmstream::pack {

namespace {

// K[i] = 64 primeiros bits da parte fracionária da raiz cúbica do i-ésimo primo (FIPS 180-4
// §4.2.3), calculados com inteiros de precisão arbitrária e conferidos pelos vetores de teste.
constexpr std::array<std::uint64_t, 80> kRoundConstants = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

constexpr std::uint64_t rotr(std::uint64_t value, unsigned count) noexcept
{
    return (value >> count) | (value << (64U - count));
}

} // namespace

// Valor inicial: parte fracionária da raiz quadrada dos 8 primeiros primos (FIPS 180-4 §5.3.5).
Sha512::Sha512() noexcept
    : state_{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
             0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
             0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL}
{
}

void Sha512::compress(const std::uint8_t* block) noexcept
{
    std::uint64_t w[80];
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint64_t word = 0;
        for (std::size_t j = 0; j < 8; ++j) {
            word = (word << 8) | block[8 * i + j];
        }
        w[i] = word;
    }
    for (std::size_t i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const std::uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0; i < 80; ++i) {
        const std::uint64_t big_s1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const std::uint64_t choose = (e & f) ^ (~e & g);
        const std::uint64_t t1 = h + big_s1 + choose + kRoundConstants[i] + w[i];
        const std::uint64_t big_s0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const std::uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t t2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha512::update(std::span<const std::byte> data) noexcept
{
    total_bytes_ += data.size();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t remaining = data.size();

    if (buffered_ > 0) {
        const std::size_t take = std::min(remaining, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        remaining -= take;
        if (buffered_ < buffer_.size()) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (remaining >= buffer_.size()) {
        compress(bytes);
        bytes += buffer_.size();
        remaining -= buffer_.size();
    }
    if (remaining > 0) {
        std::memcpy(buffer_.data(), bytes, remaining);
        buffered_ = remaining;
    }
}

Sha512Digest Sha512::finish() noexcept
{
    // O comprimento em bits ocupa 128 bits; os 64 de cima são sempre 0 (total_bytes_ cabe em 64).
    const std::uint64_t bit_length = total_bytes_ * 8U;

    buffer_[buffered_++] = 0x80;
    if (buffered_ > 112) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
        compress(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[120 + i] = static_cast<std::uint8_t>(bit_length >> (56U - 8U * i));
    }
    compress(buffer_.data());

    Sha512Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 8; ++j) {
            digest[8 * i + j] = static_cast<std::uint8_t>(state_[i] >> (56U - 8U * j));
        }
    }
    return digest;
}

Sha512Digest sha512(std::span<const std::byte> data) noexcept
{
    Sha512 hasher;
    hasher.update(data);
    return hasher.finish();
}

} // namespace swarmstream::pack
