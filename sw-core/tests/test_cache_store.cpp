// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "check.hpp"
#include "swarmstream/memory_cache_store.hpp"

#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace swarmstream {

void test_cache_store_alloc_write_commit_read()
{
    std::cout << "[Test] CacheStore: Alloc, write, commit, read\n";
    auto store = std::make_shared<MemoryCacheStore>(16, 1024);

    auto alloc_res = store->alloc_slot();
    SW_CHECK(alloc_res.has_value());
    SlotId slot = alloc_res.value();
    SW_CHECK(slot < 16);

    std::vector<uint8_t> test_data = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    auto write_res = store->write(slot, 0, test_data);
    SW_CHECK(write_res.has_value());

    PieceHash h{};
    h.bytes[0] = 0x42;
    PieceId piece(10, h);

    auto commit_res = store->commit(slot, piece, test_data.size());
    SW_CHECK(commit_res.has_value());
    SW_CHECK(store->has_piece(piece));

    // Read piece
    auto read_res = store->read(piece);
    SW_CHECK(read_res.has_value());
    Lease lease = std::move(read_res.value());
    SW_CHECK(lease.valid());
    SW_CHECK(lease.size() == test_data.size());
    SW_CHECK(std::memcmp(lease.data(), test_data.data(), test_data.size()) == 0);
    SW_CHECK(lease.slot_id() == slot);
    SW_CHECK(lease.piece_id() == piece);

    SW_CHECK(store->active_leases_count() == 1);
}

void test_cache_store_lease_prevents_free()
{
    std::cout << "[Test] CacheStore: Active lease prevents free (ADR-0005)\n";
    auto store = std::make_shared<MemoryCacheStore>(8, 1024);

    auto alloc_res = store->alloc_slot();
    SW_CHECK(alloc_res.has_value());
    SlotId slot = alloc_res.value();

    std::vector<uint8_t> data = {1, 2, 3, 4};
    SW_CHECK(store->write(slot, 0, data).has_value());

    PieceHash h{};
    h.bytes[0] = 0x77;
    PieceId piece(20, h);
    SW_CHECK(store->commit(slot, piece, data.size()).has_value());

    // Acquire lease
    auto read_res = store->read(piece);
    SW_CHECK(read_res.has_value());
    Lease lease = std::move(read_res.value());
    SW_CHECK(lease.valid());

    // Attempting to free slot or piece while lease is alive MUST FAIL with SlotBusy
    auto free_slot_res = store->free_slot(slot);
    SW_CHECK(!free_slot_res.has_value());
    SW_CHECK(free_slot_res.error() == CacheErrorCode::SlotBusy);

    auto free_piece_res = store->free_piece(piece);
    SW_CHECK(!free_piece_res.has_value());
    SW_CHECK(free_piece_res.error() == CacheErrorCode::SlotBusy);

    // Release lease
    lease.reset();
    SW_CHECK(!lease.valid());
    SW_CHECK(store->active_leases_count() == 0);

    // Freeing now MUST SUCCEED
    auto free_ok = store->free_piece(piece);
    SW_CHECK(free_ok.has_value());
    SW_CHECK(!store->has_piece(piece));
}

void test_cache_store_out_of_slots()
{
    std::cout << "[Test] CacheStore: Out of slots capacity\n";
    auto store = std::make_shared<MemoryCacheStore>(3, 512);

    auto s0 = store->alloc_slot();
    auto s1 = store->alloc_slot();
    auto s2 = store->alloc_slot();
    SW_CHECK(s0.has_value() && s1.has_value() && s2.has_value());

    // 4th allocation must fail
    auto s3 = store->alloc_slot();
    SW_CHECK(!s3.has_value());
    SW_CHECK(s3.error() == CacheErrorCode::OutOfSlots);

    // Free s1 and allocate again
    SW_CHECK(store->free_slot(s1.value()).has_value());
    auto s_realloc = store->alloc_slot();
    SW_CHECK(s_realloc.has_value());
    SW_CHECK(s_realloc.value() == s1.value());
}

void test_cache_store_bounds_checking()
{
    std::cout << "[Test] CacheStore: Bounds checking\n";
    auto store = std::make_shared<MemoryCacheStore>(4, 100);

    auto alloc_res = store->alloc_slot();
    SlotId slot = alloc_res.value();

    std::vector<uint8_t> too_large(101, 0xFF);
    auto write_err = store->write(slot, 0, too_large);
    SW_CHECK(!write_err.has_value());
    SW_CHECK(write_err.error() == CacheErrorCode::InvalidSize);

    std::vector<uint8_t> ok_data(50, 0x11);
    auto write_offset_err = store->write(slot, 60, ok_data);
    SW_CHECK(!write_offset_err.has_value());
    SW_CHECK(write_offset_err.error() == CacheErrorCode::InvalidSize);
}

void test_cache_store_checksum_corruption()
{
    std::cout << "[Test] CacheStore: Checksum corruption detection\n";
    auto store = std::make_shared<MemoryCacheStore>(4, 1024);

    auto alloc_res = store->alloc_slot();
    SlotId slot = alloc_res.value();

    std::vector<uint8_t> data = {10, 20, 30, 40, 50};
    SW_CHECK(store->write(slot, 0, data).has_value());

    PieceHash h{};
    PieceId piece(33, h);
    SW_CHECK(store->commit(slot, piece, data.size()).has_value());

    // Corrupt one byte directly in storage
    store->corrupt_slot_byte(slot, 2, 0x99);

    // Read should detect checksum mismatch
    auto read_res = store->read(piece);
    SW_CHECK(!read_res.has_value());
    SW_CHECK(read_res.error() == CacheErrorCode::CorruptedChecksum);
}

void test_cache_store_journal_and_checkpoint()
{
    std::cout << "[Test] CacheStore: WAL Journal records and checkpoint\n";
    auto store = std::make_shared<MemoryCacheStore>(8, 1024);

    auto alloc_res = store->alloc_slot();
    SlotId slot = alloc_res.value();

    std::vector<uint8_t> data = {1, 2, 3};
    SW_CHECK(store->write(slot, 0, data).has_value());

    PieceHash h{};
    PieceId piece(100, h);
    SW_CHECK(store->commit(slot, piece, data.size()).has_value());

    store->trigger_checkpoint();

    SW_CHECK(store->free_slot(slot).has_value());

    auto records = store->journal_records();
    SW_CHECK(records.size() == 3);
    SW_CHECK(records[0].type == JournalRecordType::CommitPiece);
    SW_CHECK(records[0].lsn == 1);
    SW_CHECK(records[1].type == JournalRecordType::Checkpoint);
    SW_CHECK(records[1].lsn == 2);
    SW_CHECK(records[2].type == JournalRecordType::FreeSlot);
    SW_CHECK(records[2].lsn == 3);
    SW_CHECK(store->last_lsn() == 3);
}

void test_cache_store_concurrent_readers()
{
    std::cout << "[Test] CacheStore: Concurrent multithreaded readers\n";
    auto store = std::make_shared<MemoryCacheStore>(16, 4096);

    auto alloc_res = store->alloc_slot();
    SlotId slot = alloc_res.value();

    std::vector<uint8_t> data(2048, 0xAB);
    SW_CHECK(store->write(slot, 0, data).has_value());

    PieceHash h{};
    PieceId piece(500, h);
    SW_CHECK(store->commit(slot, piece, data.size()).has_value());

    constexpr int kNumThreads = 8;
    constexpr int kIterations = 200;
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([store, piece, &data]() {
            for (int i = 0; i < kIterations; ++i) {
                auto read_res = store->read(piece);
                SW_CHECK(read_res.has_value());
                Lease lease = std::move(read_res.value());
                SW_CHECK(lease.valid());
                SW_CHECK(lease.size() == data.size());
                SW_CHECK(std::memcmp(lease.data(), data.data(), data.size()) == 0);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    SW_CHECK(store->active_leases_count() == 0);
    SW_CHECK(store->free_slot(slot).has_value());
}

} // namespace swarmstream

void run_cache_store_tests()
{
    swarmstream::test_cache_store_alloc_write_commit_read();
    swarmstream::test_cache_store_lease_prevents_free();
    swarmstream::test_cache_store_out_of_slots();
    swarmstream::test_cache_store_bounds_checking();
    swarmstream::test_cache_store_checksum_corruption();
    swarmstream::test_cache_store_journal_and_checkpoint();
    swarmstream::test_cache_store_concurrent_readers();
}
