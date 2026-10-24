// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-11: XXH3-64 própria contra a biblioteca de referência (python-xxhash 4.0.2, libxxhash
// 0.8.3, 2026-10-24). Os tamanhos cobrem todos os ramos do algoritmo e as bordas de cada um.
// Entrada: byte i = ((i * 31 + 7) ^ (i >> 8)) & 0xFF.

#include "check.hpp"
#include "swarmstream/xxh3.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Vector
{
    size_t len;
    uint64_t hash;
};

constexpr Vector kVectors[] = {
    {0u, 0x2d06800538d394c2ull},       {1u, 0x4c5cca45d0f4811full},
    {2u, 0xa7e250c97710ff27ull},       {3u, 0x15f7093b173d005cull},
    {4u, 0xdca012f95811b6b9ull},       {5u, 0xb290cafc7b254345ull},
    {7u, 0x7561869c23da3c1bull},       {8u, 0xdec6a9a43575982eull},
    {9u, 0xcbe393399f17ffbdull},       {15u, 0x545e19990471dc37ull},
    {16u, 0x7e484c18d74895d0ull},      {17u, 0x208bde5ee2bed407ull},
    {31u, 0xa937652b0119ca11ull},      {32u, 0x03df0ac5255d1446ull},
    {33u, 0x199a362122d71f46ull},      {63u, 0x76d4eec1f092847full},
    {64u, 0xdd30702ab46b3745ull},      {65u, 0xfab36b851b94ce20ull},
    {95u, 0x9857e7a5a42c6fe9ull},      {96u, 0xd245cd2541582982ull},
    {97u, 0x60e3e1d0d43785b3ull},      {127u, 0xa915ed6396db8cc0ull},
    {128u, 0xf92b70eaa21a6288ull},     {129u, 0xf8f76713f2bb60faull},
    {130u, 0x303a2c3823c3c34aull},     {200u, 0x12fdb864685f344dull},
    {239u, 0xcaa9b7a588464745ull},     {240u, 0xccc7375172c41f03ull},
    {241u, 0x0b3b630948ce4a00ull},     {255u, 0x89932170686cdd9aull},
    {256u, 0xec85b75bafe6ca74ull},     {257u, 0x115d82c4590b05b1ull},
    {1023u, 0x22ff72bf7a712151ull},    {1024u, 0xd218d699d62a6d8bull},
    {1025u, 0x38f5f1f86ddfa599ull},    {1087u, 0x9ba9cf664f00786full},
    {1088u, 0x9df03fc840d44bf8ull},    {2048u, 0xe29245b34db354a9ull},
    {4095u, 0x8ecf9a2d5383599dull},    {4096u, 0x99aa8007ca97e119ull},
    {10000u, 0x001125d6f7a5bb65ull},   {100000u, 0x3af21d5f4869b6ecull},
    {1048576u, 0x521bcb191a08d0a4ull},
};

} // namespace

int main()
{
    std::puts("--- XXH3-64 (F1-11) ---");
    std::vector<uint8_t> buf(1048576);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>(((i * 31 + 7) ^ (i >> 8)) & 0xFF);
    }
    for (const Vector& v : kVectors) {
        const uint64_t got = swarmstream::xxh3_64(buf.data(), v.len);
        if (got != v.hash) {
            std::printf("len %zu: esperado %016llx, veio %016llx\n", v.len,
                        static_cast<unsigned long long>(v.hash),
                        static_cast<unsigned long long>(got));
        }
        SW_CHECK(got == v.hash);
    }
    SW_CHECK(swarmstream::xxh3_64("abc", 3) == 0x78af5f94892f3950ULL);
    // Não depende do alinhamento: o mesmo conteúdo em endereço ímpar dá o mesmo hash.
    std::vector<uint8_t> odd(4097);
    for (size_t i = 0; i < 4096; ++i)
        odd[i + 1] = buf[i];
    SW_CHECK(swarmstream::xxh3_64(odd.data() + 1, 4096) == swarmstream::xxh3_64(buf.data(), 4096));
    std::printf("-> %zu vetores conferem com a referência.\n", std::size(kVectors) + 1);
    return 0;
}
