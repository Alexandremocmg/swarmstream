// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/memory_content_source.hpp"

#include <utility>

namespace swarmstream {

MemoryContentSource::MemoryContentSource(std::string name,
                                         std::chrono::milliseconds simulated_latency)
    : name_(std::move(name)), simulated_latency_(simulated_latency)
{
}

MemoryContentSource::~MemoryContentSource()
{
    shutting_down_.store(true, std::memory_order_release);

    // Cancel all remaining pending requests
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        for (auto& [id, req] : active_requests_) {
            req->canceled.store(true, std::memory_order_release);
        }
    }

    // Join all background worker threads
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        worker_threads_.clear();
    }
}

RequestTicket MemoryContentSource::request(const PieceId& piece, SteadyTimePoint deadline)
{
    RequestId req_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    requests_total_.fetch_add(1, std::memory_order_relaxed);

    auto req = std::make_shared<PendingRequest>();
    req->id = req_id;
    req->piece = piece;
    req->deadline = deadline;

    std::future<PieceResponse> future = req->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        active_requests_[req_id] = req;
    }

    // Spawn an asynchronous worker to fulfill the request
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        // Clean up joinable threads that already finished
        auto it = worker_threads_.begin();
        while (it != worker_threads_.end()) {
            if (it->joinable()) {
                // We keep active threads and join on destruction,
                // or let them run to completion
                ++it;
            } else {
                it = worker_threads_.erase(it);
            }
        }

        worker_threads_.emplace_back(&MemoryContentSource::process_request, this, req);
    }

    return RequestTicket{.request_id = req_id, .future = std::move(future)};
}

void MemoryContentSource::process_request(std::shared_ptr<PendingRequest> req)
{
    auto latency = simulated_latency_.load(std::memory_order_relaxed);

    if (latency.count() > 0) {
        auto start = std::chrono::steady_clock::now();
        auto target = start + latency;
        while (std::chrono::steady_clock::now() < target) {
            if (req->canceled.load(std::memory_order_acquire) ||
                shutting_down_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Remove from active requests
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        active_requests_.erase(req->id);
    }

    // 1. Check for cancellation
    if (req->canceled.load(std::memory_order_acquire)) {
        requests_canceled_.fetch_add(1, std::memory_order_relaxed);
        req->promise.set_value(PieceResponse{.piece = req->piece,
                                             .data = {},
                                             .error = SourceErrorCode::Canceled,
                                             .error_message = "Request canceled"});
        return;
    }

    // 2. O prazo é só prioridade (swarmstream.h): um prazo vencido não gera Timeout
    //    (F1-56). O mock não tem limite de espera; quem lê desiste por conta própria.

    // 3. Check injected errors
    {
        std::lock_guard<std::mutex> lock(pieces_mutex_);
        auto err_it = injected_errors_.find(req->piece);
        if (err_it != injected_errors_.end()) {
            requests_failed_.fetch_add(1, std::memory_order_relaxed);
            req->promise.set_value(PieceResponse{.piece = req->piece,
                                                 .data = {},
                                                 .error = err_it->second,
                                                 .error_message = "Injected error for testing"});
            return;
        }

        // 4. Check piece availability
        auto it = pieces_.find(req->piece);
        if (it != pieces_.end()) {
            size_t bytes = it->second.size();
            bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
            requests_completed_.fetch_add(1, std::memory_order_relaxed);

            req->promise.set_value(PieceResponse{.piece = req->piece,
                                                 .data = it->second,
                                                 .error = SourceErrorCode::Success,
                                                 .error_message = ""});
            return;
        }
    }

    // Piece not found
    requests_failed_.fetch_add(1, std::memory_order_relaxed);
    req->promise.set_value(PieceResponse{.piece = req->piece,
                                         .data = {},
                                         .error = SourceErrorCode::NotFound,
                                         .error_message = "Piece not found in memory source"});
}

bool MemoryContentSource::cancel(RequestId request_id)
{
    std::shared_ptr<PendingRequest> req;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        auto it = active_requests_.find(request_id);
        if (it != active_requests_.end()) {
            req = it->second;
        }
    }

    if (!req) {
        return false;
    }

    bool expected = false;
    if (req->canceled.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return true;
    }

    return false;
}

SourceStats MemoryContentSource::stats() const
{
    SourceStats s{};
    s.requests_total = requests_total_.load(std::memory_order_relaxed);
    s.requests_completed = requests_completed_.load(std::memory_order_relaxed);
    s.requests_canceled = requests_canceled_.load(std::memory_order_relaxed);
    s.requests_failed = requests_failed_.load(std::memory_order_relaxed);
    s.bytes_received = bytes_received_.load(std::memory_order_relaxed);
    return s;
}

void MemoryContentSource::set_piece(const PieceId& piece, std::vector<uint8_t> data)
{
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    pieces_[piece] = std::move(data);
}

void MemoryContentSource::remove_piece(const PieceId& piece)
{
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    pieces_.erase(piece);
}

bool MemoryContentSource::has_piece(const PieceId& piece) const
{
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return pieces_.find(piece) != pieces_.end();
}

void MemoryContentSource::set_simulated_latency(std::chrono::milliseconds latency) noexcept
{
    simulated_latency_.store(latency, std::memory_order_release);
}

std::chrono::milliseconds MemoryContentSource::simulated_latency() const noexcept
{
    return simulated_latency_.load(std::memory_order_acquire);
}

void MemoryContentSource::set_injected_error(const PieceId& piece, SourceErrorCode error)
{
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    injected_errors_[piece] = error;
}

void MemoryContentSource::clear_injected_errors()
{
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    injected_errors_.clear();
}

} // namespace swarmstream
