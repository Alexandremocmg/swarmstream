// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/block_ram_cache.hpp"

#include <algorithm>
#include <bit>
#include <functional>

namespace swarmstream {

void BlockLease::reset() noexcept
{
    if (cache_ != nullptr) {
        cache_->release(index_);
        cache_ = nullptr;
    }
    data_ = nullptr;
    size_ = 0;
}

BlockLease BlockFill::commit() noexcept
{
    if (cache_ == nullptr) return {};
    BlockRamCache* c = std::exchange(cache_, nullptr);
    data_ = nullptr;
    size_ = 0;
    return c->publish(index_);
}

void BlockFill::abort() noexcept
{
    if (cache_ != nullptr) {
        cache_->discard(index_);
        cache_ = nullptr;
    }
    data_ = nullptr;
    size_ = 0;
}

BlockRamCache::BlockRamCache(size_t capacity_bytes, size_t buffer_size)
    : buffer_size_(std::max<size_t>(buffer_size, 1)),
      capacity_blocks_(std::max<size_t>(capacity_bytes / buffer_size_, 1)),
      pool_(new uint8_t[capacity_blocks_ * buffer_size_]), entries_(capacity_blocks_),
      buckets_(std::bit_ceil(capacity_blocks_ * 2), kNone)
{
    free_list_.reserve(capacity_blocks_);
    for (size_t i = capacity_blocks_; i > 0; --i) {
        free_list_.push_back(static_cast<uint32_t>(i - 1));
    }
}

size_t BlockRamCache::bucket_of(const BlockKey& key) const noexcept
{
    const size_t h =
        std::hash<PieceId>{}(key.piece) ^ (static_cast<size_t>(key.block) * 0x9e3779b97f4a7c15ULL);
    return (h ^ (h >> 29)) & (buckets_.size() - 1);
}

uint32_t BlockRamCache::find_locked(const BlockKey& key) const noexcept
{
    for (uint32_t i = buckets_[bucket_of(key)]; i != kNone; i = entries_[i].next) {
        if (entries_[i].key == key) return i;
    }
    return kNone;
}

void BlockRamCache::unlink_locked(uint32_t index) noexcept
{
    uint32_t* link = &buckets_[bucket_of(entries_[index].key)];
    while (*link != kNone && *link != index)
        link = &entries_[*link].next;
    if (*link == index) *link = entries_[index].next;
    entries_[index].next = kNone;
}

// Livre, ou então a primeira entrada Ready sem lease e sem a marca "lida" (CLOCK). Duas voltas
// bastam: a primeira apaga as marcas. kNone se tudo está preso.
uint32_t BlockRamCache::take_victim_locked() noexcept
{
    if (!free_list_.empty()) {
        const uint32_t i = free_list_.back();
        free_list_.pop_back();
        return i;
    }
    for (size_t step = 0; step < 2 * capacity_blocks_; ++step) {
        const size_t i = hand_;
        hand_ = (hand_ + 1) % capacity_blocks_;
        Entry& e = entries_[i];
        if (e.state != State::Ready || e.refs != 0) continue;
        if (e.referenced) {
            e.referenced = false;
            continue;
        }
        unlink_locked(static_cast<uint32_t>(i));
        e.state = State::Free;
        --resident_;
        ++evictions_;
        return static_cast<uint32_t>(i);
    }
    return kNone;
}

BlockLease BlockRamCache::acquire(const BlockKey& key) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t i = find_locked(key);
    if (i == kNone || entries_[i].state != State::Ready) {
        ++misses_;
        return {};
    }
    Entry& e = entries_[i];
    if (e.refs++ == 0) ++leased_;
    e.referenced = true;
    ++hits_;
    return BlockLease(this, i, buffer_at(i), e.size);
}

Result<BlockFill, BlockCacheError> BlockRamCache::reserve(const BlockKey& key, size_t size) noexcept
{
    if (size == 0 || size > buffer_size_) return Unexpected(BlockCacheError::InvalidSize);
    std::lock_guard<std::mutex> lock(mutex_);
    if (const uint32_t at = find_locked(key); at != kNone) {
        return Unexpected(entries_[at].state == State::Ready ? BlockCacheError::Exists
                                                             : BlockCacheError::Filling);
    }
    const uint32_t i = take_victim_locked();
    if (i == kNone) return Unexpected(BlockCacheError::OutOfBuffers);
    Entry& e = entries_[i];
    e.key = key;
    e.size = static_cast<uint32_t>(size);
    e.refs = 0;
    e.state = State::Filling;
    e.referenced = false;
    uint32_t& head = buckets_[bucket_of(key)];
    e.next = head;
    head = i;
    return BlockFill(this, i, buffer_at(i), size);
}

BlockLease BlockRamCache::publish(uint32_t index) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[index];
    e.state = State::Ready;
    e.refs = 1;
    e.referenced = true;
    ++resident_;
    ++leased_;
    return BlockLease(this, index, buffer_at(index), e.size);
}

void BlockRamCache::discard(uint32_t index) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    unlink_locked(index);
    entries_[index].state = State::Free;
    free_list_.push_back(index);
}

void BlockRamCache::release(uint32_t index) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[index];
    if (--e.refs == 0) --leased_;
}

BlockCacheStats BlockRamCache::stats() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {capacity_blocks_, resident_, leased_, hits_, misses_, evictions_};
}

} // namespace swarmstream
