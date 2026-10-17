// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

#include "swarmstream/memory_content_source.hpp"

namespace swarmstream {

void test_content_source_basic_request() {
    std::cout << "[Test] ContentSource: Basic request\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    h.bytes[0] = 0xAA;
    PieceId id(1, h);

    std::vector<uint8_t> expected_data = {1, 2, 3, 4, 5, 6, 7, 8};
    source.set_piece(id, expected_data);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);
    assert(ticket.request_id != kInvalidRequestId);

    PieceResponse resp = ticket.future.get();
    assert(resp.ok());
    assert(resp.piece == id);
    assert(resp.data == expected_data);

    SourceStats stats = source.stats();
    assert(stats.requests_total == 1);
    assert(stats.requests_completed == 1);
    assert(stats.bytes_received == expected_data.size());
    assert(stats.requests_failed == 0);
    assert(stats.requests_canceled == 0);
}

void test_content_source_not_found() {
    std::cout << "[Test] ContentSource: Piece not found\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    PieceId id(999, h);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);

    PieceResponse resp = ticket.future.get();
    assert(!resp.ok());
    assert(resp.error == SourceErrorCode::NotFound);

    SourceStats stats = source.stats();
    assert(stats.requests_total == 1);
    assert(stats.requests_failed == 1);
}

void test_content_source_cancellation() {
    std::cout << "[Test] ContentSource: Cancellation\n";
    // Set 200ms latency to allow time for cancel() to take effect
    MemoryContentSource source("test_source", std::chrono::milliseconds(200));

    PieceHash h{};
    PieceId id(2, h);
    source.set_piece(id, {10, 20, 30});

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);

    bool canceled = source.cancel(ticket.request_id);
    assert(canceled);

    PieceResponse resp = ticket.future.get();
    assert(!resp.ok());
    assert(resp.error == SourceErrorCode::Canceled);

    SourceStats stats = source.stats();
    assert(stats.requests_canceled >= 1);
}

void test_content_source_deadline_timeout() {
    std::cout << "[Test] ContentSource: Deadline timeout\n";
    MemoryContentSource source("test_source", std::chrono::milliseconds(100));

    PieceHash h{};
    PieceId id(3, h);
    source.set_piece(id, {1, 2, 3});

    // Deadline is in the past
    auto deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(10);
    auto ticket = source.request(id, deadline);

    PieceResponse resp = ticket.future.get();
    assert(!resp.ok());
    assert(resp.error == SourceErrorCode::Timeout);
}

void test_content_source_injected_error() {
    std::cout << "[Test] ContentSource: Injected error\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    PieceId id(4, h);
    source.set_piece(id, {5, 6, 7});
    source.set_injected_error(id, SourceErrorCode::NetworkError);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto ticket = source.request(id, deadline);

    PieceResponse resp = ticket.future.get();
    assert(!resp.ok());
    assert(resp.error == SourceErrorCode::NetworkError);
}

} // namespace swarmstream

void run_content_source_tests() {
    swarmstream::test_content_source_basic_request();
    swarmstream::test_content_source_not_found();
    swarmstream::test_content_source_cancellation();
    swarmstream::test_content_source_deadline_timeout();
    swarmstream::test_content_source_injected_error();
}
