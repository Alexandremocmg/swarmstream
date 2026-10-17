// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "swarmstream/i_cache_store.hpp"

namespace swarmstream {

enum class SlotState : uint8_t {
    Free = 0,
    Allocated = 1,
    Committed = 2
};

enum class JournalRecordType : uint16_t {
    CommitPiece = 1,
    FreeSlot = 2,
    Checkpoint = 3
};

struct JournalRecord {
    uint64_t lsn{0};
    JournalRecordType type{JournalRecordType::CommitPiece};
    int64_t timestamp_micros{0};
    PieceHash piece_hash{};
    SlotId slot_id{kInvalidSlot};
    size_t piece_size{0};
    uint64_t checksum{0};
};

// MemoryCacheStore: In-memory mock implementation of ICacheStore for testing and simulation.
// Conforms to ADR-0005: pre-allocated slots, RAII leases with atomic reference counting,
// and write-ahead journal record logging.
class MemoryCacheStore final : public ICacheStore,
                               public ILeaseReleaser,
                               public std::enable_shared_from_this<MemoryCacheStore> {
public:
    explicit MemoryCacheStore(
        size_t slot_capacity = 64,
        size_t slot_size = kDefaultPieceSize);

    ~MemoryCacheStore() override = default;

    // Disallow copy and assignment
    MemoryCacheStore(const MemoryCacheStore&) = delete;
    MemoryCacheStore& operator=(const MemoryCacheStore&) = delete;
    MemoryCacheStore(MemoryCacheStore&&) = delete;
    MemoryCacheStore& operator=(MemoryCacheStore&&) = delete;

    // ICacheStore API
    [[nodiscard]] Result<SlotId, CacheErrorCode> alloc_slot() override;

    [[nodiscard]] Result<void, CacheErrorCode> write(
        SlotId slot,
        size_t offset,
        std::span<const uint8_t> data) override;

    [[nodiscard]] Result<void, CacheErrorCode> commit(
        SlotId slot,
        const PieceId& piece,
        size_t total_size) override;

    [[nodiscard]] Result<Lease, CacheErrorCode> read(const PieceId& piece) override;

    [[nodiscard]] Result<void, CacheErrorCode> free_slot(SlotId slot) override;

    [[nodiscard]] Result<void, CacheErrorCode> free_piece(const PieceId& piece) override;

    [[nodiscard]] bool has_piece(const PieceId& piece) const override;

    [[nodiscard]] size_t slot_capacity() const noexcept override {
        return slot_capacity_;
    }

    [[nodiscard]] size_t slot_size() const noexcept override {
        return slot_size_;
    }

    [[nodiscard]] size_t allocated_slots() const noexcept override;

    [[nodiscard]] size_t active_leases_count() const noexcept override;

    [[nodiscard]] CacheStats stats() const override;

    // ILeaseReleaser callback
    void release_lease(SlotId slot_id, const PieceId& piece_id) noexcept override;

    // Journal inspection methods for tests (db-acid-in-practice verification)
    [[nodiscard]] std::vector<JournalRecord> journal_records() const;
    [[nodiscard]] uint64_t last_lsn() const noexcept;
    void trigger_checkpoint();

    // Helper to simulate corruption for error testing
    void corrupt_slot_byte(SlotId slot, size_t offset, uint8_t bad_value);

private:
    struct SlotMetadata {
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<uint32_t> lease_count{0};
        PieceId piece_id{};
        size_t piece_size{0};
        uint64_t checksum{0};
    };

    static uint64_t compute_checksum(std::span<const uint8_t> data) noexcept;

    const size_t slot_capacity_;
    const size_t slot_size_;

    // Pre-allocated contiguous memory buffer for all slots
    std::vector<uint8_t> memory_pool_;

    // Slot metadata array
    std::vector<SlotMetadata> slots_;

    // Thread-safe piece hash -> SlotId index
    mutable std::mutex index_mutex_;
    std::unordered_map<PieceId, SlotId> piece_to_slot_;

    // Journal logging (WAL)
    mutable std::mutex journal_mutex_;
    std::atomic<uint64_t> lsn_counter_{1};
    std::vector<JournalRecord> journal_;

    // Storage statistics (atomics)
    std::atomic<uint64_t> stat_cache_hits_{0};
    std::atomic<uint64_t> stat_cache_misses_{0};
    std::atomic<uint64_t> stat_bytes_written_{0};
    std::atomic<uint64_t> stat_bytes_read_{0};
};

} // namespace swarmstream
