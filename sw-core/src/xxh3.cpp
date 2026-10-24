// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// XXH3-64, semente 0, segredo padrão de 192 bytes. Escrita a partir da especificação do
// algoritmo (xxHash 0.8). Os nomes das etapas seguem a especificação para facilitar a revisão.

#include "swarmstream/xxh3.hpp"

#include <array>
#include <cstring>

namespace swarmstream {

namespace {

constexpr uint64_t kPrime32_1 = 0x9E3779B1U;
constexpr uint64_t kPrime32_2 = 0x85EBCA77U;
constexpr uint64_t kPrime32_3 = 0xC2B2AE3DU;
constexpr uint64_t kPrime64_1 = 0x9E3779B185EBCA87ULL;
constexpr uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4FULL;
constexpr uint64_t kPrime64_3 = 0x165667B19E3779F9ULL;
constexpr uint64_t kPrime64_4 = 0x85EBCA77C2B2AE63ULL;
constexpr uint64_t kPrime64_5 = 0x27D4EB2F165667C5ULL;
constexpr uint64_t kPrimeMx1 = 0x165667919E3779F9ULL;
constexpr uint64_t kPrimeMx2 = 0x9FB21C651E98DF25ULL;

// Segredo padrão (constante da especificação do XXH3).
constexpr std::array<uint8_t, 192> kSecret = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

constexpr size_t kSecretSize = kSecret.size();
constexpr size_t kStripeLen = 64;
constexpr size_t kSecretConsumeRate = 8;
constexpr size_t kAccNb = 8;
constexpr size_t kSecretLastAccStart = 7;
constexpr size_t kSecretMergeAccsStart = 11;
constexpr size_t kSecretSizeMin = 136;
constexpr size_t kMidSizeStartOffset = 3;
constexpr size_t kMidSizeLastOffset = 17;

// Leituras little-endian byte a byte: independem do alinhamento e da ordem da máquina.
uint32_t read32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read64(const uint8_t* p) noexcept
{
    return static_cast<uint64_t>(read32(p)) | (static_cast<uint64_t>(read32(p + 4)) << 32);
}

uint64_t rotl64(uint64_t x, int r) noexcept { return (x << r) | (x >> (64 - r)); }

uint32_t swap32(uint32_t x) noexcept
{
    return ((x << 24) & 0xff000000U) | ((x << 8) & 0x00ff0000U) | ((x >> 8) & 0x0000ff00U) |
           ((x >> 24) & 0x000000ffU);
}

uint64_t swap64(uint64_t x) noexcept
{
    return (static_cast<uint64_t>(swap32(static_cast<uint32_t>(x))) << 32) |
           swap32(static_cast<uint32_t>(x >> 32));
}

// Produto de 128 bits de a × b, devolvendo (parte baixa) XOR (parte alta). Portátil.
uint64_t mul128_fold64(uint64_t a, uint64_t b) noexcept
{
    const uint64_t a_lo = a & 0xFFFFFFFFULL;
    const uint64_t a_hi = a >> 32;
    const uint64_t b_lo = b & 0xFFFFFFFFULL;
    const uint64_t b_hi = b >> 32;
    const uint64_t lo_lo = a_lo * b_lo;
    const uint64_t hi_lo = a_hi * b_lo;
    const uint64_t lo_hi = a_lo * b_hi;
    const uint64_t hi_hi = a_hi * b_hi;
    const uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFULL) + lo_hi;
    const uint64_t upper = (hi_lo >> 32) + (cross >> 32) + hi_hi;
    const uint64_t lower = (cross << 32) | (lo_lo & 0xFFFFFFFFULL);
    return lower ^ upper;
}

uint64_t xxh64_avalanche(uint64_t h) noexcept
{
    h ^= h >> 33;
    h *= kPrime64_2;
    h ^= h >> 29;
    h *= kPrime64_3;
    h ^= h >> 32;
    return h;
}

uint64_t xxh3_avalanche(uint64_t h) noexcept
{
    h ^= h >> 37;
    h *= kPrimeMx1;
    h ^= h >> 32;
    return h;
}

uint64_t rrmxmx(uint64_t h, uint64_t len) noexcept
{
    h ^= rotl64(h, 49) ^ rotl64(h, 24);
    h *= kPrimeMx2;
    h ^= (h >> 35) + len;
    h *= kPrimeMx2;
    h ^= h >> 28;
    return h;
}

uint64_t mix16b(const uint8_t* in, const uint8_t* sec) noexcept
{
    return mul128_fold64(read64(in) ^ read64(sec), read64(in + 8) ^ read64(sec + 8));
}

uint64_t len_1to3(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    const uint32_t c1 = in[0];
    const uint32_t c2 = in[len >> 1];
    const uint32_t c3 = in[len - 1];
    const uint32_t combined =
        (c1 << 16) | (c2 << 24) | (c3 << 0) | (static_cast<uint32_t>(len) << 8);
    const uint64_t bitflip = read32(sec) ^ read32(sec + 4);
    return xxh64_avalanche(static_cast<uint64_t>(combined) ^ bitflip);
}

uint64_t len_4to8(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    const uint32_t input1 = read32(in);
    const uint32_t input2 = read32(in + len - 4);
    const uint64_t bitflip = read64(sec + 8) ^ read64(sec + 16);
    const uint64_t input64 = input2 + (static_cast<uint64_t>(input1) << 32);
    return rrmxmx(input64 ^ bitflip, len);
}

uint64_t len_9to16(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    const uint64_t bitflip1 = read64(sec + 24) ^ read64(sec + 32);
    const uint64_t bitflip2 = read64(sec + 40) ^ read64(sec + 48);
    const uint64_t input_lo = read64(in) ^ bitflip1;
    const uint64_t input_hi = read64(in + len - 8) ^ bitflip2;
    const uint64_t acc = len + swap64(input_lo) + input_hi + mul128_fold64(input_lo, input_hi);
    return xxh3_avalanche(acc);
}

uint64_t len_17to128(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    uint64_t acc = len * kPrime64_1;
    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc += mix16b(in + 48, sec + 96);
                acc += mix16b(in + len - 64, sec + 112);
            }
            acc += mix16b(in + 32, sec + 64);
            acc += mix16b(in + len - 48, sec + 80);
        }
        acc += mix16b(in + 16, sec + 32);
        acc += mix16b(in + len - 32, sec + 48);
    }
    acc += mix16b(in + 0, sec + 0);
    acc += mix16b(in + len - 16, sec + 16);
    return xxh3_avalanche(acc);
}

uint64_t len_129to240(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    const size_t rounds = len / 16;
    uint64_t acc = len * kPrime64_1;
    for (size_t i = 0; i < 8; ++i) {
        acc += mix16b(in + 16 * i, sec + 16 * i);
    }
    uint64_t acc_end = mix16b(in + len - 16, sec + kSecretSizeMin - kMidSizeLastOffset);
    acc = xxh3_avalanche(acc);
    for (size_t i = 8; i < rounds; ++i) {
        acc_end += mix16b(in + 16 * i, sec + 16 * (i - 8) + kMidSizeStartOffset);
    }
    return xxh3_avalanche(acc + acc_end);
}

void accumulate_512(std::array<uint64_t, kAccNb>& acc, const uint8_t* in,
                    const uint8_t* sec) noexcept
{
    for (size_t i = 0; i < kAccNb; ++i) {
        const uint64_t data_val = read64(in + 8 * i);
        const uint64_t data_key = data_val ^ read64(sec + 8 * i);
        acc[i ^ 1] += data_val;
        acc[i] += (data_key & 0xFFFFFFFFULL) * (data_key >> 32);
    }
}

void scramble(std::array<uint64_t, kAccNb>& acc, const uint8_t* sec) noexcept
{
    for (size_t i = 0; i < kAccNb; ++i) {
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= read64(sec + 8 * i);
        a *= kPrime32_1;
        acc[i] = a;
    }
}

uint64_t hash_long(const uint8_t* in, size_t len, const uint8_t* sec) noexcept
{
    std::array<uint64_t, kAccNb> acc = {kPrime32_3, kPrime64_1, kPrime64_2, kPrime64_3,
                                        kPrime64_4, kPrime32_2, kPrime64_5, kPrime32_1};
    const size_t stripes_per_block = (kSecretSize - kStripeLen) / kSecretConsumeRate;
    const size_t block_len = kStripeLen * stripes_per_block;
    const size_t blocks = (len - 1) / block_len;
    for (size_t n = 0; n < blocks; ++n) {
        for (size_t s = 0; s < stripes_per_block; ++s) {
            accumulate_512(acc, in + n * block_len + s * kStripeLen, sec + s * kSecretConsumeRate);
        }
        scramble(acc, sec + kSecretSize - kStripeLen);
    }
    const size_t stripes = ((len - 1) - block_len * blocks) / kStripeLen;
    for (size_t s = 0; s < stripes; ++s) {
        accumulate_512(acc, in + blocks * block_len + s * kStripeLen, sec + s * kSecretConsumeRate);
    }
    accumulate_512(acc, in + len - kStripeLen,
                   sec + kSecretSize - kStripeLen - kSecretLastAccStart);

    uint64_t result = len * kPrime64_1;
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t* s = sec + kSecretMergeAccsStart + 16 * i;
        result += mul128_fold64(acc[2 * i] ^ read64(s), acc[2 * i + 1] ^ read64(s + 8));
    }
    return xxh3_avalanche(result);
}

} // namespace

uint64_t xxh3_64(std::span<const std::byte> data) noexcept
{
    const auto* in = reinterpret_cast<const uint8_t*>(data.data());
    const size_t len = data.size();
    const uint8_t* sec = kSecret.data();
    if (len == 0) return xxh64_avalanche(read64(sec + 56) ^ read64(sec + 64));
    if (len <= 3) return len_1to3(in, len, sec);
    if (len <= 8) return len_4to8(in, len, sec);
    if (len <= 16) return len_9to16(in, len, sec);
    if (len <= 128) return len_17to128(in, len, sec);
    if (len <= 240) return len_129to240(in, len, sec);
    return hash_long(in, len, sec);
}

} // namespace swarmstream
