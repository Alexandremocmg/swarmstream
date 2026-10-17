// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <span>
#include <utility>
#include <memory>
#include "swarmstream/types.hpp"

namespace swarmstream {

class ILeaseReleaser {
public:
    virtual ~ILeaseReleaser() = default;
    virtual void release_lease(SlotId slot_id, const PieceId& piece_id) noexcept = 0;
};

// RAII Read Lease for a piece in cache.
// As mandated by ADR-0005: a piece with an active lease is NEVER evicted or overwritten.
class Lease {
public:
    constexpr Lease() noexcept = default;

    Lease(const uint8_t* data,
          size_t size,
          SlotId slot_id,
          const PieceId& piece_id,
          std::shared_ptr<ILeaseReleaser> releaser) noexcept
        : data_(data),
          size_(size),
          slot_id_(slot_id),
          piece_id_(piece_id),
          releaser_(std::move(releaser)) {}

    ~Lease() {
        reset();
    }

    // Move-only semantics: leases cannot be accidentally copied without explicit intent
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Lease(Lease&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          slot_id_(std::exchange(other.slot_id_, kInvalidSlot)),
          piece_id_(other.piece_id_),
          releaser_(std::move(other.releaser_)) {}

    Lease& operator=(Lease&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            slot_id_ = std::exchange(other.slot_id_, kInvalidSlot);
            piece_id_ = other.piece_id_;
            releaser_ = std::move(other.releaser_);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return data_ != nullptr && size_ > 0 && releaser_ != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] std::span<const uint8_t> span() const noexcept {
        return valid() ? std::span<const uint8_t>(data_, size_) : std::span<const uint8_t>{};
    }

    [[nodiscard]] const uint8_t* data() const noexcept {
        return data_;
    }

    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] SlotId slot_id() const noexcept {
        return slot_id_;
    }

    [[nodiscard]] const PieceId& piece_id() const noexcept {
        return piece_id_;
    }

    void reset() noexcept {
        if (releaser_ && slot_id_ != kInvalidSlot) {
            releaser_->release_lease(slot_id_, piece_id_);
            releaser_.reset();
        }
        data_ = nullptr;
        size_ = 0;
        slot_id_ = kInvalidSlot;
    }

private:
    const uint8_t* data_{nullptr};
    size_t size_{0};
    SlotId slot_id_{kInvalidSlot};
    PieceId piece_id_{};
    std::shared_ptr<ILeaseReleaser> releaser_{nullptr};
};

} // namespace swarmstream
