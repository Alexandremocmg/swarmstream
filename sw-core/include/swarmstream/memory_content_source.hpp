// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "swarmstream/i_content_source.hpp"

namespace swarmstream {

// MemoryContentSource: In-memory mock implementation of IContentSource for testing and simulation.
// Allows pre-seeding pieces, simulating network latency, cancellation, and error conditions.
class MemoryContentSource final : public IContentSource {
public:
    explicit MemoryContentSource(
        std::string name = "memory_source",
        std::chrono::milliseconds simulated_latency = std::chrono::milliseconds(0));

    ~MemoryContentSource() override;

    // Disallow copy and assignment
    MemoryContentSource(const MemoryContentSource&) = delete;
    MemoryContentSource& operator=(const MemoryContentSource&) = delete;
    MemoryContentSource(MemoryContentSource&&) = delete;
    MemoryContentSource& operator=(MemoryContentSource&&) = delete;

    // IContentSource API
    [[nodiscard]] RequestTicket request(
        const PieceId& piece,
        SteadyTimePoint deadline) override;

    bool cancel(RequestId request_id) override;

    [[nodiscard]] SourceStats stats() const override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    // Mock management API for tests
    void set_piece(const PieceId& piece, std::vector<uint8_t> data);
    void remove_piece(const PieceId& piece);
    [[nodiscard]] bool has_piece(const PieceId& piece) const;

    void set_simulated_latency(std::chrono::milliseconds latency) noexcept;
    [[nodiscard]] std::chrono::milliseconds simulated_latency() const noexcept;

    void set_injected_error(const PieceId& piece, SourceErrorCode error);
    void clear_injected_errors();

private:
    struct PendingRequest {
        RequestId id{kInvalidRequestId};
        PieceId piece{};
        SteadyTimePoint deadline{};
        std::promise<PieceResponse> promise;
        std::atomic<bool> canceled{false};
    };

    void process_request(std::shared_ptr<PendingRequest> req);

    std::string name_;
    std::atomic<std::chrono::milliseconds> simulated_latency_{std::chrono::milliseconds(0)};

    // Pre-seeded pieces storage
    mutable std::mutex pieces_mutex_;
    std::unordered_map<PieceId, std::vector<uint8_t>> pieces_;
    std::unordered_map<PieceId, SourceErrorCode> injected_errors_;

    // Pending in-flight requests for cancellation support
    mutable std::mutex requests_mutex_;
    std::unordered_map<RequestId, std::shared_ptr<PendingRequest>> active_requests_;
    std::atomic<RequestId> next_request_id_{1};

    // Tracking active async threads
    std::mutex threads_mutex_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> shutting_down_{false};

    // Statistics
    std::atomic<uint64_t> requests_total_{0};
    std::atomic<uint64_t> requests_completed_{0};
    std::atomic<uint64_t> requests_canceled_{0};
    std::atomic<uint64_t> requests_failed_{0};
    std::atomic<uint64_t> bytes_received_{0};
};

} // namespace swarmstream
