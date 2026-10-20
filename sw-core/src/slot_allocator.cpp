// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/slot_allocator.hpp"

#include <bit>

namespace swarmstream {

namespace {

constexpr uint32_t kBits = 64;

constexpr size_t words_for(uint64_t bits) noexcept
{
    return static_cast<size_t>((bits + kBits - 1) / kBits);
}

} // namespace

SlotAllocator::SlotAllocator(uint32_t capacity)
    : capacity_(capacity), free_bits_(words_for(capacity), ~uint64_t{0}),
      has_free_(words_for(words_for(capacity)), 0)
{
    // A última palavra só tem livres os bits que existem: bits além da capacidade ficam 0,
    // para nunca serem entregues.
    if (const uint32_t tail = capacity % kBits; tail != 0) {
        free_bits_.back() = (uint64_t{1} << tail) - 1;
    }
    for (size_t w = 0; w < free_bits_.size(); ++w) {
        if (free_bits_[w] != 0) {
            has_free_[w / kBits] |= uint64_t{1} << (w % kBits);
        }
    }
}

void SlotAllocator::set_used(uint32_t slot) noexcept
{
    const size_t w = slot / kBits;
    free_bits_[w] &= ~(uint64_t{1} << (slot % kBits));
    if (free_bits_[w] == 0) {
        has_free_[w / kBits] &= ~(uint64_t{1} << (w % kBits));
    }
    ++used_;
}

void SlotAllocator::set_free(uint32_t slot) noexcept
{
    const size_t w = slot / kBits;
    free_bits_[w] |= uint64_t{1} << (slot % kBits);
    has_free_[w / kBits] |= uint64_t{1} << (w % kBits);
    --used_;
}

Result<SlotId, CacheErrorCode> SlotAllocator::allocate() noexcept
{
    for (size_t s = 0; s < has_free_.size(); ++s) {
        if (has_free_[s] == 0) {
            continue;
        }
        const size_t w = s * kBits + static_cast<size_t>(std::countr_zero(has_free_[s]));
        const auto slot =
            static_cast<uint32_t>(w * kBits + static_cast<size_t>(std::countr_zero(free_bits_[w])));
        set_used(slot);
        return slot;
    }
    return Unexpected(CacheErrorCode::OutOfSlots);
}

bool SlotAllocator::is_used(SlotId slot) const noexcept
{
    return slot < capacity_ && (free_bits_[slot / kBits] & (uint64_t{1} << (slot % kBits))) == 0;
}

Result<void, CacheErrorCode> SlotAllocator::release(SlotId slot) noexcept
{
    if (!is_used(slot)) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }
    set_free(slot);
    return {};
}

Result<void, CacheErrorCode> SlotAllocator::mark_used(SlotId slot) noexcept
{
    if (slot >= capacity_) {
        return Unexpected(CacheErrorCode::SlotNotFound);
    }
    if (is_used(slot)) {
        return Unexpected(CacheErrorCode::SlotBusy);
    }
    set_used(slot);
    return {};
}

uint32_t SlotAllocator::high_water() const noexcept
{
    for (size_t w = free_bits_.size(); w-- > 0;) {
        uint64_t valid = ~uint64_t{0};
        if (w == free_bits_.size() - 1 && capacity_ % kBits != 0) {
            valid = (uint64_t{1} << (capacity_ % kBits)) - 1;
        }
        const uint64_t used_bits = ~free_bits_[w] & valid;
        if (used_bits != 0) {
            return static_cast<uint32_t>(w * kBits + (kBits - 1) -
                                         static_cast<size_t>(std::countl_zero(used_bits))) +
                   1;
        }
    }
    return 0;
}

size_t SlotAllocator::memory_bytes() const noexcept
{
    return sizeof(*this) + free_bits_.capacity() * sizeof(uint64_t) +
           has_free_.capacity() * sizeof(uint64_t);
}

} // namespace swarmstream
