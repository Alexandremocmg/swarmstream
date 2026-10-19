// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "check.hpp"
#include "swarmstream/memory_cache_store.hpp"
#include "swarmstream/memory_content_source.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace swarmstream {

// Acceptance criterion test for F0-07:
// "Um teste usa os dois mocks juntos (pedido → fonte falsa → cache falso → leitura)"
void test_pipeline_request_source_cache_read()
{
    std::cout
        << "[Integration Test] Pipeline: Request -> Mock Source -> Mock Cache -> Lease Read\n";

    // 1. Setup mock source and mock cache store
    auto source = std::make_unique<MemoryContentSource>("cdn_mock", std::chrono::milliseconds(5));
    auto cache = std::make_shared<MemoryCacheStore>(32, kDefaultPieceSize);

    // 2. Prepare mock piece data (128 KiB simulated game texture chunk)
    constexpr size_t kPieceSize = 128 * 1024;
    std::vector<uint8_t> piece_payload(kPieceSize);
    for (size_t i = 0; i < kPieceSize; ++i) {
        piece_payload[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }

    PieceHash expected_hash =
        PieceHash::from_hex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    PieceId target_piece(42, expected_hash);

    // Seed the piece into the content source
    source->set_piece(target_piece, piece_payload);

    // 3. Request piece with a 500ms deadline (simulating cache miss handling)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    RequestTicket ticket = source->request(target_piece, deadline);
    SW_CHECK(ticket.request_id != kInvalidRequestId);

    // 4. Await download completion from the source
    PieceResponse response = ticket.future.get();
    SW_CHECK(response.ok());
    SW_CHECK(response.piece == target_piece);
    SW_CHECK(response.data.size() == kPieceSize);

    // 5. Allocate an empty slot in the local cache store
    auto alloc_res = cache->alloc_slot();
    SW_CHECK(alloc_res.has_value());
    SlotId slot = alloc_res.value();

    // 6. Write data into the allocated slot
    auto write_res = cache->write(slot, 0, response.data);
    SW_CHECK(write_res.has_value());

    // 7. Commit the slot (data persisted, WAL journal record written, published to index)
    auto commit_res = cache->commit(slot, target_piece, response.data.size());
    SW_CHECK(commit_res.has_value());
    SW_CHECK(cache->has_piece(target_piece));

    // 8. Game engine reads the piece from cache, obtaining an RAII Lease
    auto read_res = cache->read(target_piece);
    SW_CHECK(read_res.has_value());
    Lease lease = std::move(read_res.value());

    // 9. Verify the leased content matches the downloaded piece bit-for-bit
    SW_CHECK(lease.valid());
    SW_CHECK(lease.size() == kPieceSize);
    SW_CHECK(std::memcmp(lease.data(), piece_payload.data(), kPieceSize) == 0);
    SW_CHECK(lease.slot_id() == slot);
    SW_CHECK(lease.piece_id() == target_piece);

    // 10. Verify active lease guarantees protection from eviction/freeing (ADR-0005)
    auto premature_free = cache->free_piece(target_piece);
    SW_CHECK(!premature_free.has_value());
    SW_CHECK(premature_free.error() == CacheErrorCode::SlotBusy);

    // 11. Engine finishes reading the asset block and drops the lease
    lease.reset();
    SW_CHECK(!lease.valid());
    SW_CHECK(cache->active_leases_count() == 0);

    // 12. Freeing the piece now succeeds
    auto clean_free = cache->free_piece(target_piece);
    SW_CHECK(clean_free.has_value());
    SW_CHECK(!cache->has_piece(target_piece));

    std::cout << "[Integration Test] Pipeline: SUCCESS!\n";
}

void test_pipeline_multi_piece_streaming()
{
    std::cout << "[Integration Test] Pipeline: Multi-piece concurrent streaming\n";

    auto source = std::make_unique<MemoryContentSource>("p2p_mock", std::chrono::milliseconds(2));
    auto cache = std::make_shared<MemoryCacheStore>(16, 64 * 1024);

    constexpr int kPieceCount = 5;
    std::vector<PieceId> piece_ids;
    std::vector<std::vector<uint8_t>> payloads;

    for (int i = 0; i < kPieceCount; ++i) {
        PieceHash h{};
        h.bytes[0] = static_cast<uint8_t>(i + 1);
        PieceId pid(static_cast<uint32_t>(i), h);
        piece_ids.push_back(pid);

        std::vector<uint8_t> data(32 * 1024, static_cast<uint8_t>(i * 10));
        payloads.push_back(data);
        source->set_piece(pid, data);
    }

    // Submit requests in parallel
    std::vector<RequestTicket> tickets;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (int i = 0; i < kPieceCount; ++i) {
        tickets.push_back(source->request(piece_ids[i], deadline));
    }

    // Process responses and populate cache
    for (int i = 0; i < kPieceCount; ++i) {
        PieceResponse resp = tickets[i].future.get();
        SW_CHECK(resp.ok());

        auto slot_res = cache->alloc_slot();
        SW_CHECK(slot_res.has_value());
        SlotId slot = slot_res.value();

        SW_CHECK(cache->write(slot, 0, resp.data).has_value());
        SW_CHECK(cache->commit(slot, resp.piece, resp.data.size()).has_value());
    }

    // Verify all pieces in cache
    for (int i = 0; i < kPieceCount; ++i) {
        auto read_res = cache->read(piece_ids[i]);
        SW_CHECK(read_res.has_value());
        Lease l = std::move(read_res.value());
        SW_CHECK(l.valid());
        SW_CHECK(l.size() == payloads[i].size());
        SW_CHECK(std::memcmp(l.data(), payloads[i].data(), l.size()) == 0);
    }

    std::cout << "[Integration Test] Multi-piece: SUCCESS!\n";
}

} // namespace swarmstream

void run_pipeline_integration_tests()
{
    swarmstream::test_pipeline_request_source_cache_read();
    swarmstream::test_pipeline_multi_piece_streaming();
}
