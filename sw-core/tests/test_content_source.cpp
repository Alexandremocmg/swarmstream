// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "check.hpp"
#include "swarmstream/memory_content_source.hpp"

#include <chrono>
#include <iostream>
#include <vector>

namespace swarmstream {

void test_content_source_basic_request()
{
    std::cout << "[Test] ContentSource: Basic request\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    h.bytes[0] = 0xAA;
    PieceId id(1, h);

    std::vector<uint8_t> expected_data = {1, 2, 3, 4, 5, 6, 7, 8};
    source.set_piece(id, expected_data);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);
    SW_CHECK(ticket.request_id != kInvalidRequestId);

    PieceResponse resp = ticket.future.get();
    SW_CHECK(resp.ok());
    SW_CHECK(resp.piece == id);
    SW_CHECK(resp.data == expected_data);

    SourceStats stats = source.stats();
    SW_CHECK(stats.requests_total == 1);
    SW_CHECK(stats.requests_completed == 1);
    SW_CHECK(stats.bytes_received == expected_data.size());
    SW_CHECK(stats.requests_failed == 0);
    SW_CHECK(stats.requests_canceled == 0);
}

void test_content_source_not_found()
{
    std::cout << "[Test] ContentSource: Piece not found\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    PieceId id(999, h);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);

    PieceResponse resp = ticket.future.get();
    SW_CHECK(!resp.ok());
    SW_CHECK(resp.error == SourceErrorCode::NotFound);

    SourceStats stats = source.stats();
    SW_CHECK(stats.requests_total == 1);
    SW_CHECK(stats.requests_failed == 1);
}

void test_content_source_cancellation()
{
    std::cout << "[Test] ContentSource: Cancellation\n";
    // Set 200ms latency to allow time for cancel() to take effect
    MemoryContentSource source("test_source", std::chrono::milliseconds(200));

    PieceHash h{};
    PieceId id(2, h);
    source.set_piece(id, {10, 20, 30});

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ticket = source.request(id, deadline);

    bool canceled = source.cancel(ticket.request_id);
    SW_CHECK(canceled);

    PieceResponse resp = ticket.future.get();
    SW_CHECK(!resp.ok());
    SW_CHECK(resp.error == SourceErrorCode::Canceled);

    SourceStats stats = source.stats();
    SW_CHECK(stats.requests_canceled >= 1);
}

void test_content_source_deadline_timeout()
{
    std::cout << "[Test] ContentSource: Expired deadline is priority, not a limit (F1-56)\n";
    const std::vector<uint8_t> data = {1, 2, 3};
    const auto past = std::chrono::steady_clock::now() - std::chrono::milliseconds(10);

    // Sem e com latência simulada, um prazo já vencido ainda recebe os dados.
    for (const auto latency : {std::chrono::milliseconds(0), std::chrono::milliseconds(50)}) {
        MemoryContentSource source("test_source", latency);
        PieceHash h{};
        PieceId id(3, h);
        source.set_piece(id, data);

        auto start = std::chrono::steady_clock::now();
        auto ticket = source.request(id, past);
        PieceResponse resp = ticket.future.get();
        SW_CHECK(resp.ok());
        SW_CHECK(resp.error == SourceErrorCode::Success);
        SW_CHECK(resp.data == data);
        // A latência simulada continua valendo (o prazo não a encurta).
        SW_CHECK(std::chrono::steady_clock::now() - start >= latency);

        SourceStats stats = source.stats();
        SW_CHECK(stats.requests_completed == 1);
        SW_CHECK(stats.requests_failed == 0);
    }

    // Prazo vencido não mascara NotFound.
    MemoryContentSource source("test_source");
    PieceHash h{};
    auto ticket = source.request(PieceId(77, h), past);
    SW_CHECK(ticket.future.get().error == SourceErrorCode::NotFound);
}

void test_content_source_injected_error()
{
    std::cout << "[Test] ContentSource: Injected error\n";
    MemoryContentSource source("test_source");

    PieceHash h{};
    PieceId id(4, h);
    source.set_piece(id, {5, 6, 7});
    source.set_injected_error(id, SourceErrorCode::NetworkError);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto ticket = source.request(id, deadline);

    PieceResponse resp = ticket.future.get();
    SW_CHECK(!resp.ok());
    SW_CHECK(resp.error == SourceErrorCode::NetworkError);
}

} // namespace swarmstream

void run_content_source_tests()
{
    swarmstream::test_content_source_basic_request();
    swarmstream::test_content_source_not_found();
    swarmstream::test_content_source_cancellation();
    swarmstream::test_content_source_deadline_timeout();
    swarmstream::test_content_source_injected_error();
}
