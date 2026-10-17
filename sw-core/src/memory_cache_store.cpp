// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/memory_cache_store.hpp"

#include <algorithm>
#include <cstring>

namespace swarmstream {

namespace {

int64_t current_micros_utc() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

MemoryCacheStore::MemoryCacheStore(size_t slot_capacity, size_t slot_size)
    : slot_capacity_(slot_capacity),
      slot_size_(slot_size),
      memory_pool_(slot_capacity * slot_size, 0),
      slots_(slot_capacity) {
}

uint64_t MemoryCacheStore::compute_checksum(std::span<const uint8_t> data) noexcept {
    // 64-bit FNV-1a checksum
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : data) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

Result<SlotId, CacheErrorCode> MemoryCacheStore::alloc_slot() {
    for (size_t i = 0; i < slot_capacity_; ++i) {
        SlotState expected = SlotState::Free;
        if (slots_[i].state.compare_exchange_strong(
                expected, SlotState::Allocated,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            slots_[i].lease_count.store(0, std::memory_order_relaxed);
            slots_[i].piece_size = 0;
            slots_[i].checksum = 0;
            return static_cast<SlotId>(i);
        }
    }
    return Unexpected(CacheErrorCode::OutOfSlots);
}

Result<void, CacheErrorCode> MemoryCacheStore::write(
    SlotId slot,
    size_t offset,
    std::span<const uint8_t> data) {
    if (slot >= slot_capacity_) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }

    if (offset + data.size() > slot_size_) {
        return Unexpected(CacheErrorCode::InvalidSize);
    }

    SlotState current_state = slots_[slot].state.load(std::memory_order_acquire);
    if (current_state != SlotState::Allocated && current_state != SlotState::Committed) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }

    uint8_t* dest = memory_pool_.data() + (static_cast<size_t>(slot) * slot_size_) + offset;
    std::memcpy(dest, data.data(), data.size());
    stat_bytes_written_.fetch_add(data.size(), std::memory_order_relaxed);

    return Result<void, CacheErrorCode>();
}

Result<void, CacheErrorCode> MemoryCacheStore::commit(
    SlotId slot,
    const PieceId& piece,
    size_t total_size) {
    if (slot >= slot_capacity_) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }

    if (total_size > slot_size_) {
        return Unexpected(CacheErrorCode::InvalidSize);
    }

    const uint8_t* slot_data = memory_pool_.data() + (static_cast<size_t>(slot) * slot_size_);
    uint64_t sum = compute_checksum(std::span<const uint8_t>(slot_data, total_size));

    // 1. Write-Ahead Log (WAL) journal entry
    uint64_t assigned_lsn = lsn_counter_.fetch_add(1, std::memory_order_relaxed);
    JournalRecord record{
        .lsn = assigned_lsn,
        .type = JournalRecordType::CommitPiece,
        .timestamp_micros = current_micros_utc(),
        .piece_hash = piece.hash,
        .slot_id = slot,
        .piece_size = total_size,
        .checksum = sum
    };

    {
        std::lock_guard<std::mutex> lock(journal_mutex_);
        journal_.push_back(record);
    }

    // 2. Commit slot metadata
    slots_[slot].piece_id = piece;
    slots_[slot].piece_size = total_size;
    slots_[slot].checksum = sum;
    slots_[slot].state.store(SlotState::Committed, std::memory_order_release);

    // 3. Make visible in in-memory index
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        piece_to_slot_[piece] = slot;
    }

    return Result<void, CacheErrorCode>();
}

Result<Lease, CacheErrorCode> MemoryCacheStore::read(const PieceId& piece) {
    SlotId slot = kInvalidSlot;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        auto it = piece_to_slot_.find(piece);
        if (it != piece_to_slot_.end()) {
            slot = it->second;
        }
    }

    if (slot == kInvalidSlot) {
        stat_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        return Unexpected(CacheErrorCode::PieceNotFound);
    }

    // Ensure committed state
    SlotState state = slots_[slot].state.load(std::memory_order_acquire);
    if (state != SlotState::Committed) {
        stat_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        return Unexpected(CacheErrorCode::PieceNotFound);
    }

    // Increment active lease count
    slots_[slot].lease_count.fetch_add(1, std::memory_order_relaxed);

    size_t size = slots_[slot].piece_size;
    const uint8_t* slot_data = memory_pool_.data() + (static_cast<size_t>(slot) * slot_size_);

    // Runtime checksum verification per docs/04-cache-local.md
    uint64_t current_checksum = compute_checksum(std::span<const uint8_t>(slot_data, size));
    if (current_checksum != slots_[slot].checksum) {
        slots_[slot].lease_count.fetch_sub(1, std::memory_order_relaxed);
        return Unexpected(CacheErrorCode::CorruptedChecksum);
    }

    stat_cache_hits_.fetch_add(1, std::memory_order_relaxed);
    stat_bytes_read_.fetch_add(size, std::memory_order_relaxed);

    return Lease(
        slot_data,
        size,
        slot,
        piece,
        shared_from_this());
}

Result<void, CacheErrorCode> MemoryCacheStore::free_slot(SlotId slot) {
    if (slot >= slot_capacity_) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }

    // Critical invariant: a slot with an active lease MUST NOT be freed or overwritten!
    uint32_t active_leases = slots_[slot].lease_count.load(std::memory_order_acquire);
    if (active_leases > 0) {
        return Unexpected(CacheErrorCode::SlotBusy);
    }

    PieceId piece = slots_[slot].piece_id;

    // Remove from index
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        piece_to_slot_.erase(piece);
    }

    // Record FREE_SLOT in WAL journal
    uint64_t assigned_lsn = lsn_counter_.fetch_add(1, std::memory_order_relaxed);
    JournalRecord record{
        .lsn = assigned_lsn,
        .type = JournalRecordType::FreeSlot,
        .timestamp_micros = current_micros_utc(),
        .piece_hash = piece.hash,
        .slot_id = slot,
        .piece_size = 0,
        .checksum = 0
    };

    {
        std::lock_guard<std::mutex> lock(journal_mutex_);
        journal_.push_back(record);
    }

    // Mark slot as Free
    slots_[slot].piece_size = 0;
    slots_[slot].checksum = 0;
    slots_[slot].state.store(SlotState::Free, std::memory_order_release);

    return Result<void, CacheErrorCode>();
}

Result<void, CacheErrorCode> MemoryCacheStore::free_piece(const PieceId& piece) {
    SlotId slot = kInvalidSlot;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        auto it = piece_to_slot_.find(piece);
        if (it != piece_to_slot_.end()) {
            slot = it->second;
        }
    }

    if (slot == kInvalidSlot) {
        return Unexpected(CacheErrorCode::PieceNotFound);
    }

    return free_slot(slot);
}

bool MemoryCacheStore::has_piece(const PieceId& piece) const {
    std::lock_guard<std::mutex> lock(index_mutex_);
    auto it = piece_to_slot_.find(piece);
    if (it == piece_to_slot_.end()) {
        return false;
    }
    SlotId slot = it->second;
    return slots_[slot].state.load(std::memory_order_acquire) == SlotState::Committed;
}

size_t MemoryCacheStore::allocated_slots() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < slot_capacity_; ++i) {
        if (slots_[i].state.load(std::memory_order_relaxed) != SlotState::Free) {
            ++count;
        }
    }
    return count;
}

size_t MemoryCacheStore::active_leases_count() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < slot_capacity_; ++i) {
        count += slots_[i].lease_count.load(std::memory_order_relaxed);
    }
    return count;
}

CacheStats MemoryCacheStore::stats() const {
    CacheStats s{};
    s.total_slots = slot_capacity_;
    s.allocated_slots = allocated_slots();

    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        s.committed_pieces = piece_to_slot_.size();
    }

    s.active_leases = active_leases_count();
    s.cache_hits = stat_cache_hits_.load(std::memory_order_relaxed);
    s.cache_misses = stat_cache_misses_.load(std::memory_order_relaxed);
    s.bytes_written = stat_bytes_written_.load(std::memory_order_relaxed);
    s.bytes_read = stat_bytes_read_.load(std::memory_order_relaxed);
    return s;
}

void MemoryCacheStore::release_lease(SlotId slot_id, [[maybe_unused]] const PieceId& piece_id) noexcept {
    if (slot_id < slot_capacity_) {
        slots_[slot_id].lease_count.fetch_sub(1, std::memory_order_acq_rel);
    }
}

std::vector<JournalRecord> MemoryCacheStore::journal_records() const {
    std::lock_guard<std::mutex> lock(journal_mutex_);
    return journal_;
}

uint64_t MemoryCacheStore::last_lsn() const noexcept {
    return lsn_counter_.load(std::memory_order_relaxed) - 1;
}

void MemoryCacheStore::trigger_checkpoint() {
    uint64_t assigned_lsn = lsn_counter_.fetch_add(1, std::memory_order_relaxed);
    JournalRecord record{
        .lsn = assigned_lsn,
        .type = JournalRecordType::Checkpoint,
        .timestamp_micros = current_micros_utc(),
        .piece_hash = {},
        .slot_id = kInvalidSlot,
        .piece_size = 0,
        .checksum = 0
    };

    std::lock_guard<std::mutex> lock(journal_mutex_);
    journal_.push_back(record);
}

void MemoryCacheStore::corrupt_slot_byte(SlotId slot, size_t offset, uint8_t bad_value) {
    if (slot < slot_capacity_ && offset < slot_size_) {
        memory_pool_[(static_cast<size_t>(slot) * slot_size_) + offset] = bad_value;
    }
}

} // namespace swarmstream
