// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <span>
#include "swarmstream/types.hpp"
#include "swarmstream/result.hpp"
#include "swarmstream/lease.hpp"

namespace swarmstream {

// ICacheStore: Abstract interface between cache orchestration and underlying storage.
// Minimal interface without any disk or file-system details (see docs/02-arquitetura.md).
class ICacheStore {
public:
    virtual ~ICacheStore() = default;

    // Allocates an unused pre-allocated slot.
    // Returns OutOfSlots if capacity is reached.
    [[nodiscard]] virtual Result<SlotId, CacheErrorCode> alloc_slot() = 0;

    // Writes raw piece data into the allocated slot at the specified offset.
    // Checks that offset + data.size() does not exceed the slot size.
    [[nodiscard]] virtual Result<void, CacheErrorCode> write(
        SlotId slot,
        size_t offset,
        std::span<const uint8_t> data) = 0;

    // Commits the slot, associating it with piece metadata and making it visible to readers.
    // In accordance with ACID principles: written data must be durable before commit becomes visible.
    [[nodiscard]] virtual Result<void, CacheErrorCode> commit(
        SlotId slot,
        const PieceId& piece,
        size_t total_size) = 0;

    // Acquires an active read lease for the requested piece.
    // Leased pieces are protected from eviction and overwrites.
    [[nodiscard]] virtual Result<Lease, CacheErrorCode> read(const PieceId& piece) = 0;

    // Frees a previously committed or allocated slot.
    // Returns SlotBusy if there are active read leases holding this slot.
    [[nodiscard]] virtual Result<void, CacheErrorCode> free_slot(SlotId slot) = 0;

    // Frees the slot associated with the specified piece.
    // Returns SlotBusy if there are active read leases holding this piece.
    [[nodiscard]] virtual Result<void, CacheErrorCode> free_piece(const PieceId& piece) = 0;

    // Checks if the piece is currently committed and available in the store.
    [[nodiscard]] virtual bool has_piece(const PieceId& piece) const = 0;

    // Total number of pre-allocated slots supported by this store.
    [[nodiscard]] virtual size_t slot_capacity() const noexcept = 0;

    // Size of each individual slot in bytes (typically 1 MiB).
    [[nodiscard]] virtual size_t slot_size() const noexcept = 0;

    // Number of currently allocated/committed slots.
    [[nodiscard]] virtual size_t allocated_slots() const noexcept = 0;

    // Total number of active leases across all slots.
    [[nodiscard]] virtual size_t active_leases_count() const noexcept = 0;

    // Returns runtime storage statistics.
    [[nodiscard]] virtual CacheStats stats() const = 0;
};

} // namespace swarmstream
