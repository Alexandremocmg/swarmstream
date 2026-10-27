// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-14: remoção CLOCK com marcas 95%/85% e conjunto fixo na CacheOwner (docs/04 §Política de
// remoção).
//
// Critério de aceite: "uso do teto nunca passa de 100%; core nunca sai". test_eviction_stress
// o prova sob carga: uma escritora joga bem mais peças do que cabem enquanto leitoras leem as
// peças fixadas (core) e uma peça presa por lease; nenhuma delas pode faltar nem mudar, e o
// número de peças nunca passa do teto. Os outros testes fixam as regras uma a uma: configuração
// inválida, marcas (sobe a 95%, para em 85%), fixado nunca sai (e solto pode sair), lease nunca
// sai, tudo fixado dá OutOfSlots sem passar do teto, segunda chance e peça fria, e a remoção
// sobrevive a uma queda (FREE_SLOT no journal). O job TSan do CI roda este executável.

#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(SW_EXPECT_TSAN)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SW_TSAN_ON 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SW_TSAN_ON 1
#endif
#if !defined(SW_TSAN_ON)
#error "SANITIZE_THREAD ligado, mas este executável não foi compilado com -fsanitize=thread"
#endif
#endif

namespace swarmstream {

namespace {

namespace fs = std::filesystem;

constexpr StoreLayout kLayout{.slots_per_file = 8, .slot_size = 64 * 1024};
constexpr size_t kBytes = 4096;

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_14_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    SW_CHECK(!ec);
}

PieceId piece(uint32_t i)
{
    PieceHash h{};
    for (size_t b = 0; b < h.bytes.size(); ++b) {
        h.bytes[b] = static_cast<uint8_t>(i * 7 + b + (i >> 8));
    }
    return PieceId(i, h);
}

std::byte content(uint32_t piece_index, size_t i)
{
    return static_cast<std::byte>(static_cast<uint8_t>(piece_index * 131 + i));
}

std::vector<std::byte> data_of(uint32_t i)
{
    std::vector<std::byte> v(kBytes);
    for (size_t k = 0; k < v.size(); ++k)
        v[k] = content(i, k);
    return v;
}

std::unique_ptr<CacheOwner> open_owner(const fs::path& dir, uint32_t total, bool evict = true)
{
    CacheOwnerConfig cfg{.dir = dir, .total_slots = total, .layout = kLayout};
    if (evict) {
        cfg.evict_high_percent = kDefaultEvictHighPercent;
        cfg.evict_low_percent = kDefaultEvictLowPercent;
    }
    auto started = CacheOwner::start(cfg);
    SW_CHECK(started.has_value());
    return std::move(*started);
}

Result<size_t, CacheErrorCode> put(CacheOwner& owner, uint32_t i, bool pinned = false,
                                   bool cold = false)
{
    const auto data = data_of(i);
    auto req = CacheRequest::put(piece(i), data);
    req.set_pinned(pinned);
    req.set_cold(cold);
    return owner.call(req);
}

// Devolve true se a peça está no cache com o conteúdo certo. Um Get conta como leitura.
bool present(CacheOwner& owner, uint32_t i)
{
    std::vector<std::byte> out(kLayout.slot_size);
    auto req = CacheRequest::get(piece(i), out);
    auto r = owner.call(req);
    if (!r.has_value()) {
        SW_CHECK(r.error() == CacheErrorCode::PieceNotFound);
        return false;
    }
    SW_CHECK(*r == kBytes);
    for (size_t k = 0; k < kBytes; ++k)
        SW_CHECK(out[k] == content(i, k));
    return true;
}

// Um pedido sem efeito: como a dona atende em ordem, quando ele volta tudo o que veio antes
// (inclusive a remoção que um Put dispara depois de responder) já terminou.
void settle(CacheOwner& owner)
{
    auto req = CacheRequest::remove(piece(0xFFFFFF));
    auto r = owner.call(req);
    SW_CHECK(!r.has_value() && r.error() == CacheErrorCode::PieceNotFound);
}

bool has_error(const Result<size_t, CacheErrorCode>& r, CacheErrorCode code)
{
    return !r.has_value() && r.error() == code;
}

void test_config_validation()
{
    std::cout << "[Test] remoção: marcas inválidas são recusadas\n";
    const fs::path dir = fresh_dir("config");
    const struct
    {
        uint32_t high, low;
    } bad[] = {{95, 0}, {0, 85}, {85, 85}, {80, 85}, {101, 85}};
    for (const auto& b : bad) {
        auto s = CacheOwner::start(CacheOwnerConfig{.dir = dir,
                                                    .total_slots = 8,
                                                    .layout = kLayout,
                                                    .evict_high_percent = b.high,
                                                    .evict_low_percent = b.low});
        SW_CHECK(!s.has_value() && s.error() == CacheErrorCode::InvalidSize);
    }
    cleanup(dir);
}

void test_watermarks()
{
    std::cout << "[Test] remoção: começa em 95% do teto e para em 85%\n";
    const fs::path dir = fresh_dir("marks");
    {
        auto owner = open_owner(dir, 20); // alta = 19, baixa = 17
        for (uint32_t i = 0; i < 18; ++i)
            SW_CHECK(put(*owner, i).has_value());
        settle(*owner);
        SW_CHECK(owner->pieces() == 18 && owner->evicted_pieces() == 0);
        SW_CHECK(put(*owner, 18).has_value()); // 19 = marca alta
        settle(*owner);
        SW_CHECK(owner->pieces() == 17); // parou na marca baixa
        SW_CHECK(owner->evicted_pieces() == 2 && owner->evicted_bytes() == 2 * kBytes);
        SW_CHECK(present(*owner, 18)); // a peça recém-gravada fica

        // Muito além do teto: todo Put dá certo e o uso nunca passa de 100%.
        for (uint32_t i = 100; i < 300; ++i) {
            SW_CHECK(put(*owner, i).has_value());
            SW_CHECK(owner->pieces() <= 20);
        }
        settle(*owner);
        SW_CHECK(owner->pieces() <= 19 && owner->pieces() >= 17);
        SW_CHECK(!owner->broken());
    }
    cleanup(dir);
}

void test_pinned_never_leaves()
{
    std::cout << "[Test] remoção: peça fixada (core) nunca sai; solta, pode sair\n";
    const fs::path dir = fresh_dir("pinned");
    {
        auto owner = open_owner(dir, 20);
        for (uint32_t i = 0; i < 3; ++i)
            SW_CHECK(put(*owner, i, /*pinned=*/true).has_value());
        SW_CHECK(put(*owner, 3).has_value());
        auto pin = CacheRequest::pin(piece(3)); // fixada depois de gravada
        SW_CHECK(owner->call(pin).has_value());
        auto missing = CacheRequest::pin(piece(999));
        SW_CHECK(has_error(owner->call(missing), CacheErrorCode::PieceNotFound));

        for (uint32_t i = 100; i < 400; ++i)
            SW_CHECK(put(*owner, i).has_value());
        settle(*owner);
        for (uint32_t i = 0; i < 4; ++i)
            SW_CHECK(present(*owner, i));
        SW_CHECK(owner->evicted_pieces() > 200);

        // Solta: volta a valer a política.
        auto unpin = CacheRequest::pin(piece(3), false);
        SW_CHECK(owner->call(unpin).has_value());
        for (uint32_t i = 400; i < 700 && present(*owner, 3); ++i)
            SW_CHECK(put(*owner, i).has_value());
        SW_CHECK(!present(*owner, 3));
        for (uint32_t i = 0; i < 3; ++i)
            SW_CHECK(present(*owner, i));
    }
    cleanup(dir);
}

void test_leased_never_leaves()
{
    std::cout << "[Test] remoção: peça com lease nunca sai\n";
    const fs::path dir = fresh_dir("leased");
    {
        auto owner = open_owner(dir, 20);
        SW_CHECK(put(*owner, 1).has_value());
        PieceLease lease;
        auto acq = CacheRequest::acquire(piece(1), lease);
        SW_CHECK(owner->call(acq).has_value());
        for (uint32_t i = 100; i < 400; ++i)
            SW_CHECK(put(*owner, i).has_value());
        settle(*owner);
        SW_CHECK(present(*owner, 1));
        lease.reset();
        for (uint32_t i = 400; i < 700 && present(*owner, 1); ++i)
            SW_CHECK(put(*owner, i).has_value());
        SW_CHECK(!present(*owner, 1)); // sem lease, a política volta a valer
    }
    cleanup(dir);
}

void test_all_pinned_is_out_of_slots()
{
    std::cout << "[Test] remoção: tudo fixado dá OutOfSlots e o uso não passa do teto\n";
    const fs::path dir = fresh_dir("allpinned");
    {
        auto owner = open_owner(dir, 8);
        for (uint32_t i = 0; i < 8; ++i)
            SW_CHECK(put(*owner, i, true).has_value());
        SW_CHECK(has_error(put(*owner, 50), CacheErrorCode::OutOfSlots));
        SW_CHECK(owner->pieces() == 8 && owner->evicted_pieces() == 0 && !owner->broken());
        for (uint32_t i = 0; i < 8; ++i)
            SW_CHECK(present(*owner, i));
        // Tudo preso por lease (sem fixar) também.
        auto unpin = CacheRequest::pin(piece(0), false);
        SW_CHECK(owner->call(unpin).has_value());
        PieceLease lease;
        auto acq = CacheRequest::acquire(piece(0), lease);
        SW_CHECK(owner->call(acq).has_value());
        SW_CHECK(has_error(put(*owner, 51), CacheErrorCode::OutOfSlots));
        SW_CHECK(present(*owner, 0));
    }
    cleanup(dir);
}

void test_second_chance_and_cold()
{
    std::cout << "[Test] remoção: lida ganha segunda chance; fria sem leitura sai primeiro\n";
    const fs::path dir = fresh_dir("clock");
    {
        auto owner = open_owner(dir, 20); // alta 19, baixa 17; o slot da peça i é i
        for (uint32_t i = 0; i < 18; ++i)
            SW_CHECK(put(*owner, i, false, /*cold=*/true).has_value());
        for (uint32_t i = 0; i < 9; ++i) { // 0..8 lidas (um Get acende a marca)
            std::vector<std::byte> out(kLayout.slot_size);
            auto g = CacheRequest::get(piece(i), out);
            SW_CHECK(owner->call(g).has_value());
        }
        SW_CHECK(put(*owner, 18).has_value()); // 19: dispara; precisa tirar 2
        settle(*owner);
        SW_CHECK(owner->evicted_pieces() == 2);
        for (uint32_t i = 0; i < 9; ++i)
            SW_CHECK(present(*owner, i));
        SW_CHECK(!present(*owner, 9) && !present(*owner, 10)); // as frias, na ordem do ponteiro
        for (uint32_t i = 11; i < 19; ++i)
            SW_CHECK(present(*owner, i));
    }
    cleanup(dir);
}

void test_eviction_survives_crash()
{
    std::cout << "[Test] remoção: peças removidas continuam removidas depois de uma queda\n";
    const fs::path dir = fresh_dir("crash");
    std::vector<uint32_t> kept;
    {
        auto owner = open_owner(dir, 20);
        for (uint32_t i = 0; i < 60; ++i)
            SW_CHECK(put(*owner, i, i < 2).has_value());
        settle(*owner);
        for (uint32_t i = 0; i < 60; ++i) {
            if (present(*owner, i)) kept.push_back(i);
        }
        SW_CHECK(kept.size() == owner->pieces() && kept.size() <= 19);
        CacheOwnerTestHook::skip_checkpoint_on_stop(*owner); // o journal fica com os FREE_SLOT
        SW_CHECK(owner->stop().has_value());
    }
    {
        auto owner = open_owner(dir, 20);
        SW_CHECK(owner->recovery().journal_records_applied > 0);
        SW_CHECK(owner->pieces() == kept.size());
        uint32_t found = 0;
        for (uint32_t i = 0; i < 60; ++i) {
            const bool was = std::find(kept.begin(), kept.end(), i) != kept.end();
            SW_CHECK(present(*owner, i) == was);
            found += was ? 1 : 0;
        }
        SW_CHECK(found == kept.size());
    }
    cleanup(dir);
}

// Critério de aceite sob carga. 1 escritora joga 600 peças num teto de 32; 4 leitoras leem sem
// parar as 4 peças fixadas e uma peça presa por lease; a observadora confere o teto.
void test_eviction_stress()
{
    std::cout << "[Test] remoção: estresse (core e lease nunca saem; uso <= teto)\n";
    const fs::path dir = fresh_dir("stress");
    {
        constexpr uint32_t kTotal = 32;
        auto owner = open_owner(dir, kTotal);
        for (uint32_t i = 0; i < 4; ++i)
            SW_CHECK(put(*owner, i, true).has_value());
        SW_CHECK(put(*owner, 4).has_value());
        PieceLease lease;
        auto acq = CacheRequest::acquire(piece(4), lease);
        SW_CHECK(owner->call(acq).has_value());

        std::atomic<bool> stop{false};
        std::atomic<uint32_t> bad{0};
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (uint32_t t = 0; t < 4; ++t) {
            readers.emplace_back([&, t] {
                std::vector<std::byte> out(kLayout.slot_size);
                for (uint32_t n = t; !stop.load(); ++n) {
                    const uint32_t i = n % 5; // 0..3 fixadas, 4 presa
                    auto req = CacheRequest::get(piece(i), out);
                    auto r = owner->call(req);
                    bool ok = r.has_value() && *r == kBytes;
                    for (size_t k = 0; ok && k < kBytes; ++k)
                        ok = out[k] == content(i, k);
                    if (!ok) bad.fetch_add(1);
                    reads.fetch_add(1);
                }
            });
        }
        std::thread watcher([&] {
            while (!stop.load()) {
                if (owner->pieces() > kTotal) bad.fetch_add(1);
                std::this_thread::yield();
            }
        });
        for (uint32_t i = 100; i < 700; ++i) {
            if (!put(*owner, i).has_value()) bad.fetch_add(1);
        }
        stop.store(true);
        for (auto& th : readers)
            th.join();
        watcher.join();
        settle(*owner);
        SW_CHECK(bad.load() == 0);
        SW_CHECK(reads.load() > 0 && owner->evicted_pieces() > 500 && !owner->broken());
        for (uint32_t i = 0; i < 5; ++i)
            SW_CHECK(present(*owner, i));
    }
    cleanup(dir);
}

} // namespace

void run_cache_eviction_tests()
{
    test_config_validation();
    test_watermarks();
    test_pinned_never_leaves();
    test_leased_never_leaves();
    test_all_pinned_is_out_of_slots();
    test_second_chance_and_cold();
    test_eviction_survives_crash();
    test_eviction_stress();
}

} // namespace swarmstream

int main()
{
    std::cout << "--- Cache eviction tests (F1-14) ---\n";
    swarmstream::run_cache_eviction_tests();
    std::cout << "-> Cache eviction tests passed.\n";
    return 0;
}
