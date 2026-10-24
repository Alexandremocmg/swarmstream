// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace swarmstream {

using SteadyTimePoint = std::chrono::steady_clock::time_point;
using SystemTimePoint = std::chrono::system_clock::time_point;

using SlotId = uint32_t;
constexpr SlotId kInvalidSlot = static_cast<SlotId>(~0U);

using RequestId = uint64_t;
constexpr RequestId kInvalidRequestId = 0;

// Standard piece size is 1 MiB per ADR-0002 and 03-formato-de-conteudo.md
constexpr size_t kDefaultPieceSize = 1024 * 1024; // 1 MiB

// 32-byte cryptographic hash (SHA-256 Merkle root)
struct PieceHash
{
    std::array<uint8_t, 32> bytes{};

    constexpr PieceHash() noexcept = default;

    constexpr explicit PieceHash(const std::array<uint8_t, 32>& b) noexcept : bytes(b) {}

    static PieceHash from_hex(std::string_view hex)
    {
        PieceHash result{};
        if (hex.size() < 64) {
            return result;
        }
        for (size_t i = 0; i < 32; ++i) {
            auto high = hex_char_to_val(hex[i * 2]);
            auto low = hex_char_to_val(hex[i * 2 + 1]);
            result.bytes[i] = static_cast<uint8_t>((high << 4) | low);
        }
        return result;
    }

    [[nodiscard]] std::string to_hex() const
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (uint8_t b : bytes) {
            oss << std::setw(2) << static_cast<unsigned int>(b);
        }
        return oss.str();
    }

    constexpr bool operator==(const PieceHash& other) const noexcept
    {
        return bytes == other.bytes;
    }

    constexpr bool operator!=(const PieceHash& other) const noexcept
    {
        return bytes != other.bytes;
    }

    constexpr bool operator<(const PieceHash& other) const noexcept { return bytes < other.bytes; }

private:
    static constexpr uint8_t hex_char_to_val(char c) noexcept
    {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    }
};

struct PieceId
{
    uint32_t index{0};
    PieceHash hash{};

    constexpr PieceId() noexcept = default;
    constexpr PieceId(uint32_t idx, const PieceHash& h) noexcept : index(idx), hash(h) {}

    constexpr bool operator==(const PieceId& other) const noexcept
    {
        return index == other.index && hash == other.hash;
    }

    constexpr bool operator!=(const PieceId& other) const noexcept { return !(*this == other); }

    constexpr bool operator<(const PieceId& other) const noexcept
    {
        if (index != other.index) {
            return index < other.index;
        }
        return hash < other.hash;
    }
};

enum class CacheErrorCode {
    Success = 0,
    OutOfSlots,
    SlotNotFound,
    SlotBusy, // Slot has active leases and cannot be freed or overwritten
    InvalidOffset,
    InvalidSize,
    PieceNotFound,
    CorruptedChecksum,
    IoError,
    DiskFull, // sem espaço no disco (pré-alocação ou escrita); F1-10
    Stopped   // a thread dona do cache parou; o pedido não foi atendido (F1-12)
};

[[nodiscard]] constexpr std::string_view to_string(CacheErrorCode code) noexcept
{
    switch (code) {
    case CacheErrorCode::Success: return "Success";
    case CacheErrorCode::OutOfSlots: return "OutOfSlots";
    case CacheErrorCode::SlotNotFound: return "SlotNotFound";
    case CacheErrorCode::SlotBusy: return "SlotBusy";
    case CacheErrorCode::InvalidOffset: return "InvalidOffset";
    case CacheErrorCode::InvalidSize: return "InvalidSize";
    case CacheErrorCode::PieceNotFound: return "PieceNotFound";
    case CacheErrorCode::CorruptedChecksum: return "CorruptedChecksum";
    case CacheErrorCode::IoError: return "IoError";
    case CacheErrorCode::DiskFull: return "DiskFull";
    case CacheErrorCode::Stopped: return "Stopped";
    }
    return "UnknownCacheError";
}

enum class SourceErrorCode {
    Success = 0,
    Canceled,
    Timeout,
    NotFound,
    NetworkError,
    CorruptedData
};

[[nodiscard]] constexpr std::string_view to_string(SourceErrorCode code) noexcept
{
    switch (code) {
    case SourceErrorCode::Success: return "Success";
    case SourceErrorCode::Canceled: return "Canceled";
    case SourceErrorCode::Timeout: return "Timeout";
    case SourceErrorCode::NotFound: return "NotFound";
    case SourceErrorCode::NetworkError: return "NetworkError";
    case SourceErrorCode::CorruptedData: return "CorruptedData";
    }
    return "UnknownSourceError";
}

struct SourceStats
{
    uint64_t requests_total{0};
    uint64_t requests_completed{0};
    uint64_t requests_canceled{0};
    uint64_t requests_failed{0};
    uint64_t bytes_received{0};
};

struct CacheStats
{
    size_t total_slots{0};
    size_t allocated_slots{0};
    size_t committed_pieces{0};
    size_t active_leases{0};
    uint64_t cache_hits{0};
    uint64_t cache_misses{0};
    uint64_t bytes_written{0};
    uint64_t bytes_read{0};
};

} // namespace swarmstream

namespace std {
template <>
struct hash<swarmstream::PieceHash>
{
    size_t operator()(const swarmstream::PieceHash& ph) const noexcept
    {
        // FNV-1a 64-bit on the 32 bytes
        uint64_t h = 14695981039346656037ULL;
        for (uint8_t b : ph.bytes) {
            h ^= static_cast<uint64_t>(b);
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};

template <>
struct hash<swarmstream::PieceId>
{
    size_t operator()(const swarmstream::PieceId& id) const noexcept
    {
        size_t h1 = std::hash<uint32_t>{}(id.index);
        size_t h2 = std::hash<swarmstream::PieceHash>{}(id.hash);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};
} // namespace std
