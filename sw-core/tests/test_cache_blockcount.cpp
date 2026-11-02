// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-70: conferência de `block_count` na recuperação do cache (docs/04 §5 passo 3, F1-60 §6.3).
//
// - O vetor {hash da peça, block_count}: normalização, validação, busca e construção com falta
//   de memória (strong guarantee), com um `operator new` que falha na k-ésima alocação (como o
//   F1-63 fez no Vfs::build). A construção a partir do manifesto assinado (fixture do VFS) só roda
//   com o libsodium de verdade; com o stub local (assinatura sempre errada) o teste é PULADO em voz
//   alta, com SW_TEST_STUB_SODIUM=1. O CI nunca liga essa variável.
// - A conferência no CacheOwner::start: block_count diferente é descartado (índice e journal),
//   igual é mantido, peça fora do manifesto é mantida, sem o campo é o comportamento antigo, e
//   vetor inválido dá InvalidSize sem tocar no disco.
// - Queda no checkpoint do start(): a decisão se repete (idempotente) e nada não verificado é
//   aceito. A "queda" é simulada tirando uma cópia da pasta no instante do ponto de queda e abrindo
//   a cópia (o processo morto de verdade está em crash_test.py).
// - A busca e a validação do vetor não alocam: a recuperação não ganha ponto de falta de memória.
#include "cache_owner_testing.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"
#include "swarmstream/expected_block_counts.hpp"
#include "swarmstream/manifest_view.hpp"
#include "vfs_fixture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <new>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------- falha de alocação injetada
namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};
// < 0: sem injeção. >= 0: a alocação contada em que este valor chega a 0 lança bad_alloc.
std::atomic<std::int64_t> g_fail_countdown{-1};
} // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        if (g_fail_countdown.fetch_sub(1, std::memory_order_relaxed) == 0) {
            throw std::bad_alloc();
        }
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace swarmstream {

namespace {

namespace fs = std::filesystem;
namespace fx = swarmstream::test::vfs_fixture;

constexpr StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};
constexpr uint32_t kBlocks = 3; // toda peça do teste é gravada com 3 blocos

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_70_") + name + "_" +
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
    for (size_t b = 0; b < h.bytes.size(); ++b)
        h.bytes[b] = static_cast<uint8_t>(i * 13 + b + 1);
    return PieceId(i, h);
}

struct Content
{
    std::vector<std::byte> bytes;
    std::array<uint32_t, kBlocks> blocks{};
};

Content content(uint32_t i)
{
    Content c;
    const uint32_t size = 3000 + (i * 7919) % 200000;
    c.blocks = {size / 3, size / 5, size - size / 3 - size / 5};
    c.bytes.resize(size);
    for (size_t k = 0; k < size; ++k)
        c.bytes[k] = static_cast<std::byte>((i * 131 + k * 7) & 0xFF);
    return c;
}

// {peça i, block_count}, normalizado (ordenado). Os valores vêm de {i, count}.
std::vector<PieceBlockCount> expected(std::initializer_list<std::pair<uint32_t, uint32_t>> items)
{
    std::vector<PieceBlockCount> v;
    for (const auto& [i, count] : items)
        v.push_back(PieceBlockCount{piece(i).hash, count});
    normalize_expected_block_counts(v);
    return v;
}

Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
try_open(const fs::path& dir, uint32_t total_slots, std::span<const PieceBlockCount> exp = {})
{
    return CacheOwner::start(CacheOwnerConfig{
        .dir = dir, .total_slots = total_slots, .layout = kLayout, .expected_block_counts = exp});
}

std::unique_ptr<CacheOwner> open_owner(const fs::path& dir, uint32_t total_slots,
                                       std::span<const PieceBlockCount> exp = {})
{
    auto o = try_open(dir, total_slots, exp);
    SW_CHECK(o.has_value());
    return std::move(*o);
}

Result<size_t, CacheErrorCode> put(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    auto r = CacheRequest::put(piece(i), c.bytes, c.blocks);
    return o.call(r);
}

// Lê a peça i com os blocos "do manifesto" e confere o conteúdo byte a byte.
bool present(CacheOwner& o, uint32_t i)
{
    const Content c = content(i);
    std::vector<std::byte> out(kLayout.slot_size);
    auto r = CacheRequest::get(piece(i), out, c.blocks);
    auto res = o.call(r);
    if (res.has_value()) {
        SW_CHECK(*res == c.bytes.size());
        SW_CHECK(std::memcmp(out.data(), c.bytes.data(), c.bytes.size()) == 0);
        return true;
    }
    SW_CHECK(res.error() == CacheErrorCode::PieceNotFound);
    return false;
}

std::vector<bool> presence(CacheOwner& o, uint32_t n)
{
    std::vector<bool> v;
    for (uint32_t i = 0; i < n; ++i)
        v.push_back(present(o, i));
    return v;
}

void fill(const fs::path& dir, uint32_t total_slots, uint32_t n, bool checkpoint_on_stop)
{
    auto o = open_owner(dir, total_slots);
    for (uint32_t i = 0; i < n; ++i)
        SW_CHECK(put(*o, i).has_value());
    if (!checkpoint_on_stop) CacheOwnerTestHook::skip_checkpoint_on_stop(*o);
    SW_CHECK(o->stop().has_value());
}

// ---------------------------------------------------------------- o vetor

void test_normalize()
{
    std::cout << "[Test] vetor: ordena, junta repetidas iguais, tira ambíguas e block_count 0\n";
    std::vector<PieceBlockCount> v;
    normalize_expected_block_counts(v); // vazio
    SW_CHECK(v.empty());
    v = {{piece(5).hash, 3}, {piece(1).hash, 7}, {piece(5).hash, 3}, {piece(2).hash, 4},
         {piece(2).hash, 9}, {piece(3).hash, 0}, {piece(4).hash, 2}};
    normalize_expected_block_counts(v);
    // 5 repetida igual -> uma; 2 ambígua -> sai; 3 com 0 -> sai
    SW_CHECK(v.size() == 3);
    SW_CHECK(expected_block_counts_valid(v));
    SW_CHECK(expected_block_count_of(v, piece(1).hash) == 7);
    SW_CHECK(expected_block_count_of(v, piece(4).hash) == 2);
    SW_CHECK(expected_block_count_of(v, piece(5).hash) == 3);
    SW_CHECK(expected_block_count_of(v, piece(2).hash) == 0); // ambígua: não consta
    SW_CHECK(expected_block_count_of(v, piece(3).hash) == 0);
    SW_CHECK(expected_block_count_of(v, piece(9).hash) == 0);
    SW_CHECK(expected_block_count_of({}, piece(1).hash) == 0);
}

void test_valid()
{
    std::cout << "[Test] vetor: validação (ordem estrita, sem repetição, block_count >= 1)\n";
    SW_CHECK(expected_block_counts_valid({}));
    const auto good = expected({{1, 3}, {2, 4}, {3, 5}});
    SW_CHECK(expected_block_counts_valid(good));
    auto reversed = good;
    std::reverse(reversed.begin(), reversed.end());
    SW_CHECK(!expected_block_counts_valid(reversed));
    auto repeated = good;
    repeated.push_back(good.back());
    SW_CHECK(!expected_block_counts_valid(repeated));
    auto zero = good;
    zero[1].block_count = 0;
    SW_CHECK(!expected_block_counts_valid(zero));
}

// Fonte sintética: n peças com hashes distintos por id; a cada 7 (a partir de 14) a peça repete o
// hash da anterior, com o mesmo block_count (repetição consistente, que vira uma entrada só).
PieceBlockCount synthetic_at(const void*, size_t i) noexcept
{
    PieceBlockCount v;
    const size_t id = (i >= 10 && i % 7 == 0) ? i - 1 : i;
    for (size_t b = 0; b < v.piece.bytes.size(); ++b)
        v.piece.bytes[b] =
            b < 4 ? static_cast<uint8_t>(id >> (8 * b)) : static_cast<uint8_t>(b * 17 + 3);
    v.block_count = static_cast<uint32_t>(1 + id % 9);
    return v;
}

void test_build_ok_and_oom_sweep()
{
    std::cout
        << "[Test] vetor: construção; falta de memória em cada alocação deixa `out` intacto\n";
    std::vector<PieceBlockCount> sentinel = expected({{40, 6}, {41, 7}, {42, 8}});
    const std::vector<PieceBlockCount> sentinel_copy = sentinel;

    std::vector<PieceBlockCount> out = sentinel;
    SW_CHECK(build_expected_block_counts(0, synthetic_at, nullptr, out) ==
             ExpectedBlockCountsStatus::Ok);
    SW_CHECK(out.empty()); // n = 0 publica um vetor vazio

    constexpr size_t kN = 64;
    out = sentinel;
    SW_CHECK(build_expected_block_counts(kN, synthetic_at, nullptr, out) ==
             ExpectedBlockCountsStatus::Ok);
    SW_CHECK(expected_block_counts_valid(out));
    for (size_t i = 0; i < kN; ++i) { // toda peça da fonte consta, com o seu block_count
        const PieceBlockCount want = synthetic_at(nullptr, i);
        SW_CHECK(expected_block_count_of(out, want.piece) == want.block_count);
    }

    // Varre a k-ésima alocação: cada falha devolve OutOfMemory com `out` exatamente como estava.
    uint64_t fails_seen = 0;
    uint64_t k = 0;
    for (;; ++k) {
        out = sentinel;
        g_allocations.store(0);
        g_fail_countdown.store(static_cast<std::int64_t>(k));
        g_counting.store(true);
        const ExpectedBlockCountsStatus st =
            build_expected_block_counts(kN, synthetic_at, nullptr, out);
        g_counting.store(false);
        g_fail_countdown.store(-1);
        if (st == ExpectedBlockCountsStatus::Ok) {
            SW_CHECK(expected_block_counts_valid(out) && out.size() == kN - 8);
            break;
        }
        SW_CHECK(st == ExpectedBlockCountsStatus::OutOfMemory);
        SW_CHECK(out.size() == sentinel_copy.size());
        for (size_t i = 0; i < out.size(); ++i)
            SW_CHECK(out[i].piece == sentinel_copy[i].piece &&
                     out[i].block_count == sentinel_copy[i].block_count);
        ++fails_seen;
        SW_CHECK(k < 100); // a construção faz poucas alocações; não pode varrer para sempre
    }
    std::cout << "    " << fails_seen << " alocação(ões) varrida(s), todas com `out` intacto\n";
    SW_CHECK(fails_seen >= 1); // a injeção enxergou a alocação do vetor
}

// ---------------------------------------------------------------- do manifesto (libsodium real)

bool stub_sodium()
{
    const char* v = std::getenv("SW_TEST_STUB_SODIUM");
    return v != nullptr && v[0] == '1';
}

void test_build_from_manifest()
{
    std::cout
        << "[Test] vetor: construído do manifesto assinado (piece_hash e piece_block_count)\n";
    if (stub_sodium()) {
        std::cout << "    PULADO (SW_TEST_STUB_SODIUM=1: stub local do libsodium, a assinatura do\n"
                     "    manifesto não confere; este teste só vale com o libsodium de verdade)\n";
        return;
    }
    static const std::array<pack::Ed25519PublicKey, 1> keys = {[] {
        pack::Ed25519PublicKey k{};
        std::copy(fx::kPublicKey.begin(), fx::kPublicKey.end(), k.begin());
        return k;
    }()};
    pack::ReadOptions options;
    options.trusted_keys = keys;
    for (const auto bytes : {std::span<const std::uint8_t>(fx::kMainManifest),
                             std::span<const std::uint8_t>(fx::kHighBytesManifest),
                             std::span<const std::uint8_t>(fx::kMaxBlockManifest)}) {
        pack::ManifestView view;
        const pack::ManifestStatus st =
            pack::ManifestView::open(std::as_bytes(bytes), options, view);
        if (!st.ok()) std::cerr << "ManifestView::open falhou: " << st.detail << "\n";
        SW_CHECK(st.ok());
        SW_CHECK(view.piece_count() >= 1);
        std::vector<PieceBlockCount> out;
        SW_CHECK(build_expected_block_counts(view, out) == ExpectedBlockCountsStatus::Ok);
        SW_CHECK(expected_block_counts_valid(out));
        for (size_t i = 0; i < view.piece_count(); ++i) {
            SW_CHECK(expected_block_count_of(out, PieceHash(view.piece_hash(i))) ==
                     view.piece_block_count(i));
        }
        SW_CHECK(out.size() <= view.piece_count());
        std::cout << "    manifesto com " << view.piece_count() << " peça(s): " << out.size()
                  << " no vetor\n";
    }
}

// ---------------------------------------------------------------- CacheOwner::start

void test_differing_block_count_dropped_from_index()
{
    std::cout << "[Test] start: block_count diferente é descartado (entradas do index.bin)\n";
    const fs::path dir = fresh_dir("index");
    fill(dir, 6, 6, /*checkpoint_on_stop=*/true);
    // 1 e 3 diferem; 2 é igual; 4 e 5 não constam; 0 consta e é igual.
    const auto exp = expected({{0, kBlocks}, {1, 4}, {2, kBlocks}, {3, 2}, {9, 5}});
    {
        auto o = open_owner(dir, 6, exp);
        const CacheRecovery& rec = o->recovery();
        SW_CHECK(rec.index_loaded && rec.journal_records_applied == 0);
        SW_CHECK(rec.dropped_block_count == 2 && rec.pieces == 4 && o->pieces() == 4);
        SW_CHECK(rec.dropped_ceiling == 0 && rec.dropped_conflict == 0 &&
                 rec.dropped_missing_file == 0);
        const std::vector<bool> want = {true, false, true, false, true, true};
        SW_CHECK(presence(*o, 6) == want); // o que ficou vem com os bytes e checksums certos
        // Os dois slots ficaram livres: duas peças novas cabem num cache de 6 slots cheio.
        SW_CHECK(put(*o, 6).has_value() && put(*o, 7).has_value());
        SW_CHECK(!put(*o, 8).has_value());
        SW_CHECK(present(*o, 6) && present(*o, 7));
        SW_CHECK(o->stop().has_value());
    }
    {
        // A decisão foi gravada no checkpoint: sem a lista, nada volta e nada é descartado de novo.
        auto o = open_owner(dir, 6);
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 6);
        const std::vector<bool> want = {true, false, true, false, true, true, true, true};
        SW_CHECK(presence(*o, 8) == want);
    }
    cleanup(dir);
}

void test_differing_block_count_dropped_from_journal()
{
    std::cout << "[Test] start: block_count diferente é descartado (registros do journal)\n";
    const fs::path dir = fresh_dir("journal");
    fill(dir, 6, 6, /*checkpoint_on_stop=*/false); // tudo só no journal
    const auto exp = expected({{1, 4}, {2, kBlocks}, {3, 2}});
    {
        auto o = open_owner(dir, 6, exp);
        const CacheRecovery& rec = o->recovery();
        SW_CHECK(rec.journal_records_applied == 6 && !rec.index_invalid);
        SW_CHECK(rec.dropped_block_count == 2 && rec.pieces == 4);
        const std::vector<bool> want = {true, false, true, false, true, true};
        SW_CHECK(presence(*o, 6) == want);
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 6);
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 4);
    }
    cleanup(dir);
}

void test_journal_and_index_mixed()
{
    std::cout << "[Test] start: índice e journal juntos (peça no índice, outra só no journal)\n";
    const fs::path dir = fresh_dir("mixed");
    {
        auto o = open_owner(dir, 6);
        for (uint32_t i = 0; i < 3; ++i)
            SW_CHECK(put(*o, i).has_value());
        SW_CHECK(o->stop().has_value()); // 0..2 no index.bin
    }
    {
        auto o = open_owner(dir, 6);
        for (uint32_t i = 3; i < 6; ++i)
            SW_CHECK(put(*o, i).has_value());
        CacheOwnerTestHook::skip_checkpoint_on_stop(*o);
        SW_CHECK(o->stop().has_value()); // 3..5 só no journal
    }
    const auto exp = expected({{1, 9}, {4, 9}});
    {
        auto o = open_owner(dir, 6, exp);
        SW_CHECK(o->recovery().index_loaded && o->recovery().journal_records_applied == 3);
        SW_CHECK(o->recovery().dropped_block_count == 2 && o->recovery().pieces == 4);
        const std::vector<bool> want = {true, false, true, true, false, true};
        SW_CHECK(presence(*o, 6) == want);
    }
    cleanup(dir);
}

void test_equal_and_unlisted_are_kept()
{
    std::cout << "[Test] start: block_count igual e peça fora do manifesto são mantidos\n";
    const fs::path dir = fresh_dir("kept");
    fill(dir, 6, 6, true);
    {
        const auto all_equal = expected(
            {{0, kBlocks}, {1, kBlocks}, {2, kBlocks}, {3, kBlocks}, {4, kBlocks}, {5, kBlocks}});
        auto o = open_owner(dir, 6, all_equal);
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 6);
        SW_CHECK(presence(*o, 6) == std::vector<bool>(6, true));
        SW_CHECK(o->stop().has_value());
    }
    {
        const auto unrelated = expected({{20, 1}, {21, 2}, {22, 3}}); // nenhuma é do cache
        auto o = open_owner(dir, 6, unrelated);
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 6);
        SW_CHECK(presence(*o, 6) == std::vector<bool>(6, true));
    }
    cleanup(dir);
}

void test_without_field_is_old_behavior()
{
    std::cout << "[Test] start: sem o campo, o comportamento é o de antes\n";
    const fs::path dir = fresh_dir("nofield");
    fill(dir, 6, 6, false);
    {
        auto o = open_owner(dir, 6); // campo omitido
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 6);
        SW_CHECK(presence(*o, 6) == std::vector<bool>(6, true));
        SW_CHECK(o->stop().has_value());
    }
    {
        auto o = open_owner(dir, 6, std::span<const PieceBlockCount>{}); // vazio explícito
        SW_CHECK(o->recovery().dropped_block_count == 0 && o->recovery().pieces == 6);
    }
    cleanup(dir);
}

void test_dropped_piece_can_be_put_again_with_new_blocks()
{
    std::cout << "[Test] start: a peça descartada volta a ser gravada com a nova partição\n";
    const fs::path dir = fresh_dir("reput");
    fill(dir, 4, 3, true);
    const auto exp = expected({{1, 4}});
    {
        auto o = open_owner(dir, 4, exp);
        SW_CHECK(o->recovery().dropped_block_count == 1);
        // Mesma peça (mesmo hash), agora em 4 blocos: o Put calcula os checksums dos novos blocos.
        const Content c = content(1);
        const std::array<uint32_t, 4> blocks4 = {c.blocks[0], c.blocks[1], c.blocks[2] / 2,
                                                 c.blocks[2] - c.blocks[2] / 2};
        auto put_req = CacheRequest::put(piece(1), c.bytes, blocks4);
        SW_CHECK(o->call(put_req).has_value());
        std::vector<std::byte> out(kLayout.slot_size);
        auto get_req = CacheRequest::get(piece(1), out, blocks4);
        auto got = o->call(get_req);
        SW_CHECK(got.has_value() && *got == c.bytes.size());
        SW_CHECK(std::memcmp(out.data(), c.bytes.data(), c.bytes.size()) == 0);
    }
    cleanup(dir);
}

void test_invalid_vector_is_refused_without_touching_disk()
{
    std::cout
        << "[Test] start: vetor fora de ordem, repetido ou com block_count 0 dá InvalidSize\n";
    const fs::path dir = fresh_dir("invalid");
    const auto good = expected({{1, 3}, {2, 3}});
    auto reversed = good;
    std::reverse(reversed.begin(), reversed.end());
    auto repeated = good;
    repeated.push_back(good.back());
    auto zero = good;
    zero[0].block_count = 0;
    for (const auto* bad : {&reversed, &repeated, &zero}) {
        auto o = try_open(dir, 4, *bad);
        SW_CHECK(!o.has_value() && o.error() == CacheErrorCode::InvalidSize);
        SW_CHECK(fs::is_empty(dir)); // nenhum arquivo foi criado
    }
    cleanup(dir);
}

// ---------------------------------------------------------------- queda no checkpoint do start()

struct SnapshotHook
{
    fs::path dir;
    std::array<fs::path, 3>
        copies; // CheckpointIndexTmp, CheckpointIndexRenamed, CheckpointJournalTmp
    std::array<bool, 3> taken{};
    std::atomic<bool> armed{false};
};

void snapshot_hook(detail::CrashPoint point, void* context)
{
    auto* h = static_cast<SnapshotHook*>(context);
    if (!h->armed.load()) return;
    int slot = -1;
    switch (point) {
    case detail::CrashPoint::CheckpointIndexTmp: slot = 0; break;
    case detail::CrashPoint::CheckpointIndexRenamed: slot = 1; break;
    case detail::CrashPoint::CheckpointJournalTmp: slot = 2; break;
    default: return;
    }
    std::error_code ec;
    fs::remove_all(h->copies[slot], ec);
    fs::copy(h->dir, h->copies[slot], fs::copy_options::recursive, ec);
    SW_CHECK(!ec);
    h->taken[slot] = true;
}

void crash_case(const char* name, bool data_in_journal)
{
    std::cout << "[Test] queda: checkpoint do start() interrompido em 3 pontos (" << name << ")\n";
    const fs::path dir = fresh_dir(name);
    fill(dir, 6, 6, !data_in_journal);
    const auto exp = expected({{1, 4}, {3, 2}, {4, kBlocks}});
    const std::vector<bool> want = {true, false, true, false, true, true};

    SnapshotHook hook;
    hook.dir = dir;
    for (size_t i = 0; i < hook.copies.size(); ++i)
        hook.copies[i] = fresh_dir((std::string(name) + "_snap" + std::to_string(i)).c_str());
    hook.armed.store(true);
    {
        auto started = CacheOwnerTestHook::start_with_crash_hook(
            CacheOwnerConfig{
                .dir = dir, .total_slots = 6, .layout = kLayout, .expected_block_counts = exp},
            snapshot_hook, &hook);
        hook.armed.store(false);
        SW_CHECK(started.has_value());
        CacheOwner& o = **started;
        SW_CHECK(o.recovery().dropped_block_count == 2 && o.recovery().pieces == 4);
        SW_CHECK(presence(o, 6) == want);
        SW_CHECK(o.stop().has_value());
    }
    SW_CHECK(hook.taken[0] && hook.taken[1] && hook.taken[2]);

    for (size_t i = 0; i < hook.copies.size(); ++i) {
        // Cada cópia é o disco no instante da "queda". Reabre duas vezes, com a lista: a decisão
        // é a mesma, nada descartado volta, e tudo o que é servido confere byte a byte.
        for (int round = 0; round < 2; ++round) {
            auto o = open_owner(hook.copies[i], 6, exp);
            const CacheRecovery& rec = o->recovery();
            SW_CHECK(presence(*o, 6) == want);
            SW_CHECK(rec.pieces == 4);
            if (i == 0 && round == 0) {
                // O índice novo ainda não existia: a mesma decisão é tomada de novo (e informada).
                SW_CHECK(rec.dropped_block_count == 2);
            } else {
                // Índice novo já no lugar (ou a reabertura anterior já gravou o checkpoint): as
                // peças descartadas não estão nele, e os registros antigos do journal (LSN <=
                // checkpoint) são pulados e não as ressuscitam.
                SW_CHECK(rec.dropped_block_count == 0);
            }
            SW_CHECK(o->stop().has_value());
        }
        // E sem a lista nenhuma peça descartada reaparece (o checkpoint da reabertura gravou).
        auto o = open_owner(hook.copies[i], 6);
        SW_CHECK(o->recovery().pieces == 4 && presence(*o, 6) == want);
    }
    for (const auto& c : hook.copies)
        cleanup(c);
    cleanup(dir);
}

void test_crash_idempotent()
{
    crash_case("crash_index", false);
    crash_case("crash_journal", true);
}

// ---------------------------------------------------------------- a conferência não aloca

void test_check_does_not_allocate()
{
    std::cout
        << "[Test] busca e validação do vetor não alocam (a recuperação não ganha falha nova)\n";
    const auto exp = expected({{1, 3}, {2, 4}, {3, 5}, {7, 6}});
    uint64_t found = 0;
    g_allocations.store(0);
    g_fail_countdown.store(-1);
    g_counting.store(true);
    const bool valid = expected_block_counts_valid(exp);
    for (uint32_t i = 0; i < 12; ++i)
        found += expected_block_count_of(exp, piece(i).hash) != 0 ? 1 : 0;
    g_counting.store(false);
    SW_CHECK(valid && found == 4);
    SW_CHECK(g_allocations.load() == 0);
}

} // namespace

} // namespace swarmstream

int main()
{
    std::cout << std::unitbuf; // a saída aparece mesmo se um teste terminar o processo
    std::cout << "--- Conferência de block_count na recuperação (F1-70) ---\n";
    swarmstream::test_normalize();
    swarmstream::test_valid();
    swarmstream::test_build_ok_and_oom_sweep();
    swarmstream::test_build_from_manifest();
    swarmstream::test_differing_block_count_dropped_from_index();
    swarmstream::test_differing_block_count_dropped_from_journal();
    swarmstream::test_journal_and_index_mixed();
    swarmstream::test_equal_and_unlisted_are_kept();
    swarmstream::test_without_field_is_old_behavior();
    swarmstream::test_dropped_piece_can_be_put_again_with_new_blocks();
    swarmstream::test_invalid_vector_is_refused_without_touching_disk();
    swarmstream::test_crash_idempotent();
    swarmstream::test_check_does_not_allocate();
    std::cout << "-> Block-count tests passed.\n";
    return 0;
}
