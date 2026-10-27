// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/ed25519.hpp"

#include "swarmstream/sha512.hpp"

#include <cstring>

namespace swarmstream::pack {

namespace {

using i64 = std::int64_t;
using u8 = std::uint8_t;
// Elemento do corpo GF(2^255 - 19): 16 limbs de 16 bits (sinalizados, com folga para o vai-um).
using Gf = i64[16];

constexpr Gf kGf0 = {};
constexpr Gf kGf1 = {1};
// d = -121665/121666; D2 = 2d; B = (X, Y) com Y = 4/5 e X par; I = sqrt(-1) = 2^((p-1)/4).
// Todas recalculadas das definições (aritmética de precisão arbitrária) e conferidas pelo RFC.
constexpr Gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                   0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
constexpr Gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
constexpr Gf kBaseX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                       0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
constexpr Gf kBaseY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                       0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
constexpr Gf kSqrtM1 = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                        0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

// Ordem do subgrupo, L = 2^252 + 27742317777372353535851937790883648493, little-endian.
constexpr std::int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                 0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                 0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void set(Gf out, const Gf in) noexcept
{
    for (int i = 0; i < 16; ++i) {
        out[i] = in[i];
    }
}

void carry(Gf o) noexcept
{
    for (int i = 0; i < 16; ++i) {
        o[i] += (i64{1} << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * (i64{1} << 16);
    }
}

// Troca p e q se b == 1, sem desvio.
void select(Gf p, Gf q, i64 b) noexcept
{
    const i64 c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(u8 out[32], const Gf in) noexcept
{
    Gf t;
    Gf m;
    set(t, in);
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const i64 b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<u8>(t[i] & 0xff);
        out[2 * i + 1] = static_cast<u8>(t[i] >> 8);
    }
}

bool equal_32(const u8* x, const u8* y) noexcept
{
    unsigned diff = 0;
    for (int i = 0; i < 32; ++i) {
        diff |= static_cast<unsigned>(x[i] ^ y[i]);
    }
    return diff == 0;
}

bool field_equal(const Gf a, const Gf b) noexcept
{
    u8 c[32];
    u8 d[32];
    pack(c, a);
    pack(d, b);
    return equal_32(c, d);
}

int parity(const Gf a) noexcept
{
    u8 d[32];
    pack(d, a);
    return d[0] & 1;
}

void unpack(Gf out, const u8 in[32]) noexcept
{
    for (int i = 0; i < 16; ++i) {
        out[i] = in[2 * i] + (static_cast<i64>(in[2 * i + 1]) << 8);
    }
    out[15] &= 0x7fff;
}

void add(Gf o, const Gf a, const Gf b) noexcept
{
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] + b[i];
    }
}

void sub(Gf o, const Gf a, const Gf b) noexcept
{
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] - b[i];
    }
}

void mul(Gf o, const Gf a, const Gf b) noexcept
{
    i64 t[31] = {};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) {
        o[i] = t[i];
    }
    carry(o);
    carry(o);
}

void square(Gf o, const Gf a) noexcept { mul(o, a, a); }

void invert(Gf o, const Gf in) noexcept
{
    Gf c;
    set(c, in);
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) {
            mul(c, c, in);
        }
    }
    set(o, c);
}

// in^((p-5)/8) = in^(2^252 - 3)
void pow2523(Gf o, const Gf in) noexcept
{
    Gf c;
    set(c, in);
    for (int a = 250; a >= 0; --a) {
        square(c, c);
        if (a != 1) {
            mul(c, c, in);
        }
    }
    set(o, c);
}

// Ponto em coordenadas estendidas (X, Y, Z, T).
using Point = Gf[4];

void point_add(Point p, Point q) noexcept
{
    Gf a, b, c, d, t, e, f, g, h;
    sub(a, p[1], p[0]);
    sub(t, q[1], q[0]);
    mul(a, a, t);
    add(b, p[0], p[1]);
    add(t, q[0], q[1]);
    mul(b, b, t);
    mul(c, p[3], q[3]);
    mul(c, c, kD2);
    mul(d, p[2], q[2]);
    add(d, d, d);
    sub(e, b, a);
    sub(f, d, c);
    add(g, d, c);
    add(h, b, a);
    mul(p[0], e, f);
    mul(p[1], h, g);
    mul(p[2], g, f);
    mul(p[3], e, h);
}

void cswap(Point p, Point q, u8 b) noexcept
{
    for (int i = 0; i < 4; ++i) {
        select(p[i], q[i], b);
    }
}

void point_pack(u8 r[32], Point p) noexcept
{
    Gf tx, ty, zi;
    invert(zi, p[2]);
    mul(tx, p[0], zi);
    mul(ty, p[1], zi);
    pack(r, ty);
    r[31] ^= static_cast<u8>(parity(tx) << 7);
}

void scalar_mult(Point p, Point q, const u8 s[32]) noexcept
{
    set(p[0], kGf0);
    set(p[1], kGf1);
    set(p[2], kGf1);
    set(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        const auto b = static_cast<u8>((s[i / 8] >> (i & 7)) & 1);
        cswap(p, q, b);
        point_add(q, p);
        point_add(p, p);
        cswap(p, q, b);
    }
}

void scalar_base(Point p, const u8 s[32]) noexcept
{
    Point q;
    set(q[0], kBaseX);
    set(q[1], kBaseY);
    set(q[2], kGf1);
    mul(q[3], kBaseX, kBaseY);
    scalar_mult(p, q, s);
}

// x mod L, com x de até 512 bits em 64 limbs de 8 bits (destrói x).
void mod_l(u8 r[32], i64 x[64]) noexcept
{
    i64 c;
    int i;
    int j;
    for (i = 63; i >= 32; --i) {
        c = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += c - 16 * x[i] * kL[j - (i - 32)];
            c = (x[j] + 128) >> 8;
            x[j] -= c * 256;
        }
        x[j] += c;
        x[i] = 0;
    }
    c = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += c - (x[31] >> 4) * kL[j];
        c = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) {
        x[j] -= c * kL[j];
    }
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<u8>(x[i] & 255);
    }
}

// Reduz um resumo de 64 bytes módulo L; o resultado (32 bytes) fica no começo de h.
void reduce(u8 h[64]) noexcept
{
    i64 x[64];
    for (int i = 0; i < 64; ++i) {
        x[i] = h[i];
        h[i] = 0;
    }
    mod_l(h, x);
}

// Decodifica A e devolve -A em `r`; false se não for um ponto válido da curva.
bool unpack_negated(Point r, const u8 p[32]) noexcept
{
    Gf t, chk, num, den, den2, den4, den6;
    set(r[2], kGf1);
    unpack(r[1], p);
    square(num, r[1]);
    mul(den, num, kD);
    sub(num, num, r[2]);
    add(den, r[2], den);

    square(den2, den);
    square(den4, den2);
    mul(den6, den4, den2);
    mul(t, den6, num);
    mul(t, t, den);

    pow2523(t, t);
    mul(t, t, num);
    mul(t, t, den);
    mul(t, t, den);
    mul(r[0], t, den);

    square(chk, r[0]);
    mul(chk, chk, den);
    if (!field_equal(chk, num)) {
        mul(r[0], r[0], kSqrtM1);
    }
    square(chk, r[0]);
    mul(chk, chk, den);
    if (!field_equal(chk, num)) {
        return false;
    }
    if (parity(r[0]) == (p[31] >> 7)) {
        sub(r[0], kGf0, r[0]);
    }
    mul(r[3], r[0], r[1]);
    return true;
}

bool scalar_is_canonical(const u8 s[32]) noexcept
{
    // s < L, comparando do byte mais significativo para o menos.
    for (int i = 31; i >= 0; --i) {
        const i64 byte = s[i];
        if (byte < kL[i]) {
            return true;
        }
        if (byte > kL[i]) {
            return false;
        }
    }
    return false; // s == L
}

void hash_parts(Sha512& hasher, MessageParts message) noexcept
{
    for (const auto part : message) {
        hasher.update(part);
    }
}

void expand_seed(const Ed25519Seed& seed, u8 scalar[32], u8 prefix[32]) noexcept
{
    const auto digest = sha512(std::as_bytes(std::span<const u8>(seed)));
    std::memcpy(scalar, digest.data(), 32);
    std::memcpy(prefix, digest.data() + 32, 32);
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
}

} // namespace

Ed25519PublicKey ed25519_public_key(const Ed25519Seed& seed) noexcept
{
    u8 scalar[32];
    u8 prefix[32];
    expand_seed(seed, scalar, prefix);
    Point p;
    scalar_base(p, scalar);
    Ed25519PublicKey key{};
    point_pack(key.data(), p);
    return key;
}

Ed25519Signature ed25519_sign(const Ed25519Seed& seed, MessageParts message) noexcept
{
    u8 scalar[32];
    u8 prefix[32];
    expand_seed(seed, scalar, prefix);
    Ed25519PublicKey public_key{};
    {
        Point a;
        scalar_base(a, scalar);
        point_pack(public_key.data(), a);
    }

    // r = H(prefix || M) mod L ; R = r*B
    u8 r[64];
    {
        Sha512 hasher;
        hasher.update(std::as_bytes(std::span<const u8>(prefix, 32)));
        hash_parts(hasher, message);
        const auto digest = hasher.finish();
        std::memcpy(r, digest.data(), 64);
    }
    reduce(r);
    Point point;
    scalar_base(point, r);
    Ed25519Signature signature{};
    point_pack(signature.data(), point);

    // h = H(R || A || M) mod L ; S = (r + h*a) mod L
    u8 h[64];
    {
        Sha512 hasher;
        hasher.update(std::as_bytes(std::span<const u8>(signature.data(), 32)));
        hasher.update(std::as_bytes(std::span<const u8>(public_key)));
        hash_parts(hasher, message);
        const auto digest = hasher.finish();
        std::memcpy(h, digest.data(), 64);
    }
    reduce(h);

    i64 x[64] = {};
    for (int i = 0; i < 32; ++i) {
        x[i] = r[i];
    }
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += static_cast<i64>(h[i]) * static_cast<i64>(scalar[j]);
        }
    }
    mod_l(signature.data() + 32, x);
    return signature;
}

bool ed25519_verify(const Ed25519PublicKey& public_key, const Ed25519Signature& signature,
                    MessageParts message) noexcept
{
    if (!scalar_is_canonical(signature.data() + 32)) {
        return false;
    }
    Point negated_a;
    if (!unpack_negated(negated_a, public_key.data())) {
        return false;
    }

    u8 h[64];
    {
        Sha512 hasher;
        hasher.update(std::as_bytes(std::span<const u8>(signature.data(), 32)));
        hasher.update(std::as_bytes(std::span<const u8>(public_key)));
        hash_parts(hasher, message);
        const auto digest = hasher.finish();
        std::memcpy(h, digest.data(), 64);
    }
    reduce(h);

    // R' = S*B - h*A ; confere R' == R pelos bytes (rejeita R não canônico).
    Point p;
    Point q;
    scalar_mult(p, negated_a, h);
    scalar_base(q, signature.data() + 32);
    point_add(p, q);
    u8 recomputed[32];
    point_pack(recomputed, p);
    return equal_32(signature.data(), recomputed);
}

} // namespace swarmstream::pack
