// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// Formatos provisórios do portão da F1-20, legíveis a partir de bytes (sem
// FileAccess). São o que o sw-pack exportará para os tipos pesados (ADR-0016,
// "materialização"); o formato definitivo entra com o sw-pack. Little-endian.
//
// .swtex   "SWTX" | u32 versão=1 | u32 largura | u32 altura | u32 Image::Format
//          | u32 tem_mipmaps | u64 tamanho dos dados | dados (cadeia de mips)
// .swmesh  "SWMS" | u32 versão=1 | u32 vértices | u32 índices
//          | f32 posição[3·v] | f32 normal[3·v] | f32 uv[2·v] | u32 índice[i]

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace swgate {

struct TexBlob
{
    uint32_t width = 0, height = 0, format = 0, mipmaps = 0;
    const uint8_t* data = nullptr;
    uint64_t size = 0;
};

struct MeshBlob
{
    uint32_t vertices = 0, indices = 0;
    const float* pos = nullptr;
    const float* nrm = nullptr;
    const float* uv = nullptr;
    const uint32_t* idx = nullptr;
};

namespace detail {
inline bool rd32(const std::vector<uint8_t>& b, size_t off, uint32_t& v)
{
    if (off + 4 > b.size()) return false;
    std::memcpy(&v, b.data() + off, 4);
    return true;
}
inline bool rd64(const std::vector<uint8_t>& b, size_t off, uint64_t& v)
{
    if (off + 8 > b.size()) return false;
    std::memcpy(&v, b.data() + off, 8);
    return true;
}
} // namespace detail

// Confere cada limite; nunca lê fora do buffer.
inline bool parse_tex(const std::vector<uint8_t>& b, TexBlob& t)
{
    uint32_t version = 0;
    if (b.size() < 32 || std::memcmp(b.data(), "SWTX", 4) != 0) return false;
    if (!detail::rd32(b, 4, version) || version != 1) return false;
    if (!detail::rd32(b, 8, t.width) || !detail::rd32(b, 12, t.height) ||
        !detail::rd32(b, 16, t.format) || !detail::rd32(b, 20, t.mipmaps) ||
        !detail::rd64(b, 24, t.size)) {
        return false;
    }
    if (t.width == 0 || t.height == 0 || t.width > 16384 || t.height > 16384) return false;
    if (t.size == 0 || t.size != b.size() - 32) return false;
    t.data = b.data() + 32;
    return true;
}

inline bool parse_mesh(const std::vector<uint8_t>& b, MeshBlob& m)
{
    uint32_t version = 0;
    if (b.size() < 16 || std::memcmp(b.data(), "SWMS", 4) != 0) return false;
    if (!detail::rd32(b, 4, version) || version != 1) return false;
    if (!detail::rd32(b, 8, m.vertices) || !detail::rd32(b, 12, m.indices)) return false;
    if (m.vertices == 0 || m.indices == 0 || m.indices % 3 != 0) return false;
    if (m.vertices > (1u << 24) || m.indices > (1u << 26)) return false;
    const uint64_t need = 16 + uint64_t(m.vertices) * (3 + 3 + 2) * 4 + uint64_t(m.indices) * 4;
    if (need != b.size()) return false;
    const uint8_t* p = b.data() + 16;
    m.pos = reinterpret_cast<const float*>(p);
    m.nrm = reinterpret_cast<const float*>(p + size_t(m.vertices) * 12);
    m.uv = reinterpret_cast<const float*>(p + size_t(m.vertices) * 24);
    m.idx = reinterpret_cast<const uint32_t*>(p + size_t(m.vertices) * 32);
    for (uint32_t i = 0; i < m.indices; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, m.idx + i, 4);
        if (v >= m.vertices) return false;
    }
    return true;
}

} // namespace swgate
