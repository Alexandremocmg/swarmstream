// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-30: eventos de telemetria gravados em arquivo (docs/08 §Eventos de telemetria).
//
// Critério de aceite: "eventos com os campos de 08-metricas-e-testes.md". Os testes leem o
// arquivo e conferem os campos de read_miss_start, read_miss (completed, failed e
// pending_at_end), eviction e frame_stutter; o escape do JSON; a contagem de descarte com o
// anel cheio (nada some em silêncio: gravados + descartados = emitidos, e o resumo final diz
// quanto); o estresse com várias threads emitindo; e o evento `eviction` vindo da CacheOwner,
// cuja soma bate com evicted_pieces()/evicted_bytes(). O job TSan do CI roda este executável.

#include "check.hpp"
#include "swarmstream/cache_owner.hpp"
#include "swarmstream/telemetry.hpp"
#include "telemetry_testing.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
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

// Contador de alocações da thread que chama (F1-30: "emit_* não aloca"). Substitui o operator new
// do executável. Limitação: só conta operator new/new[] sem alinhamento especial; new alinhado
// (std::align_val_t), malloc direto e alocações de outras threads não entram.
namespace {
thread_local bool g_count_allocs = false;
thread_local uint64_t g_allocs = 0;

void* counted_alloc(std::size_t n)
{
    if (g_count_allocs) ++g_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace swarmstream {

namespace {

namespace fs = std::filesystem;

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_30_") + name + "_" +
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

std::vector<std::string> read_lines(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    SW_CHECK(in.is_open());
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l))
        lines.push_back(l);
    return lines;
}

bool has(const std::string& line, const std::string& piece)
{
    return line.find(piece) != std::string::npos;
}

// Valor numérico de "name":N na linha (falha se o campo não existe).
uint64_t num(const std::string& line, const std::string& name)
{
    const std::string key = "\"" + name + "\":";
    const size_t at = line.find(key);
    SW_CHECK(at != std::string::npos);
    return std::stoull(line.substr(at + key.size()));
}

void test_open_failure()
{
    const fs::path dir = fresh_dir("open");
    SW_CHECK(TelemetryLog::open(dir / "nao_existe" / "t.jsonl") == nullptr);
    cleanup(dir);
}

void test_fields()
{
    const fs::path dir = fresh_dir("fields");
    const fs::path file = dir / "t.jsonl";
    {
        auto log = TelemetryLog::open(file);
        SW_CHECK(log != nullptr);
        SW_CHECK(log->emit_read_miss_start({.request_id = 7,
                                            .path = "core/mapa\"1\\.bin",
                                            .offset = 4096,
                                            .size = 65536,
                                            .deadline_ms = 50,
                                            .timeout_ms = 0,
                                            .t0_us = 1000}));
        SW_CHECK(log->emit_read_miss({.request_id = 7,
                                      .t0_us = 1000,
                                      .t1_us = 251000,
                                      .call_result = "SW_E_NOT_LOCAL",
                                      .outcome = MissOutcome::Completed,
                                      .source = MissSource::Cdn}));
        SW_CHECK(log->emit_read_miss({.request_id = 8,
                                      .t0_us = 10,
                                      .t1_us = 20,
                                      .call_result = "SW_E_TIMEOUT",
                                      .outcome = MissOutcome::Failed,
                                      .source = MissSource::Peer,
                                      .error = "SW_E_CORRUPT"}));
        SW_CHECK(log->emit_read_miss({.request_id = 9,
                                      .call_result = "SW_E_NOT_LOCAL",
                                      .outcome = MissOutcome::PendingAtEnd}));
        SW_CHECK(log->emit_eviction(12, 786432));
        SW_CHECK(log->emit_frame_stutter(48000, "io_wait"));
        log->flush();
        SW_CHECK(log->written() == 6);
        SW_CHECK(log->dropped() == 0 && log->write_errors() == 0);
    }
    const auto lines = read_lines(file);
    SW_CHECK(lines.size() == 7); // 6 eventos + resumo
    SW_CHECK(has(lines[0], "\"event\":\"read_miss_start\""));
    SW_CHECK(num(lines[0], "request_id") == 7 && num(lines[0], "offset") == 4096);
    SW_CHECK(num(lines[0], "size") == 65536 && num(lines[0], "deadline_ms") == 50);
    SW_CHECK(num(lines[0], "timeout_ms") == 0 && num(lines[0], "t0_us") == 1000);
    SW_CHECK(has(lines[0], "\"path\":\"core/mapa\\\"1\\\\.bin\"")); // aspas e barra escapadas
    SW_CHECK(has(lines[1], "\"event\":\"read_miss\"") && num(lines[1], "request_id") == 7);
    SW_CHECK(num(lines[1], "t1_us") == 251000 && num(lines[1], "miss_wait_us") == 250000);
    SW_CHECK(has(lines[1], "\"call_result\":\"SW_E_NOT_LOCAL\""));
    SW_CHECK(has(lines[1], "\"outcome\":\"completed\"") && has(lines[1], "\"source\":\"cdn\""));
    SW_CHECK(!has(lines[1], "\"error\""));
    SW_CHECK(has(lines[2], "\"outcome\":\"failed\"") && has(lines[2], "\"source\":\"peer\""));
    SW_CHECK(has(lines[2], "\"error\":\"SW_E_CORRUPT\"") && !has(lines[2], "miss_wait_us"));
    SW_CHECK(has(lines[3], "\"outcome\":\"pending_at_end\"") && !has(lines[3], "\"source\""));
    SW_CHECK(has(lines[4], "\"event\":\"eviction\"") && num(lines[4], "pieces") == 12);
    SW_CHECK(num(lines[4], "bytes") == 786432 && has(lines[4], "\"t_us\":"));
    SW_CHECK(has(lines[5], "\"event\":\"frame_stutter\"") && num(lines[5], "duration_us") == 48000);
    SW_CHECK(has(lines[5], "\"cause\":\"io_wait\""));
    SW_CHECK(has(lines[6], "\"event\":\"telemetry_summary\"") && num(lines[6], "written") == 6);
    SW_CHECK(num(lines[6], "dropped") == 0 && num(lines[6], "write_errors") == 0);
    cleanup(dir);
}

void test_path_truncated_and_control_bytes()
{
    const fs::path dir = fresh_dir("path");
    const fs::path file = dir / "t.jsonl";
    {
        auto log = TelemetryLog::open(file);
        SW_CHECK(log != nullptr);
        std::string longp(1000, 'a');
        longp[3] = '\n';
        longp[4] = static_cast<char>(0xC3);
        SW_CHECK(log->emit_read_miss_start({.request_id = 1, .path = longp}));
    }
    const auto lines = read_lines(file);
    SW_CHECK(lines.size() == 2); // o \n do caminho não quebra a linha
    SW_CHECK(has(lines[0], "aaa\\u000a\\u00c3a"));
    SW_CHECK(lines[0].size() < 400);
    cleanup(dir);
}

// Anel cheio: o evento é descartado e contado, nunca bloqueia. Nada some em silêncio.
void test_drop_is_counted()
{
    const fs::path dir = fresh_dir("drop");
    const fs::path file = dir / "t.jsonl";
    constexpr uint64_t kEmitted = 200000;
    uint64_t dropped = 0, written = 0;
    {
        auto log = TelemetryLog::open(file, 8);
        SW_CHECK(log != nullptr);
        uint64_t ok = 0;
        for (uint64_t i = 0; i < kEmitted; ++i)
            ok += log->emit_eviction(i, i) ? 1 : 0;
        log->flush();
        dropped = log->dropped();
        written = log->written();
        SW_CHECK(ok == written);
        SW_CHECK(written + dropped == kEmitted);
        SW_CHECK(dropped > 0); // com 8 posições e emissão sem pausa, o anel enche
    }
    const auto lines = read_lines(file);
    SW_CHECK(lines.size() == written + 1);
    SW_CHECK(num(lines.back(), "dropped") == dropped && num(lines.back(), "written") == written);
    cleanup(dir);
}

void test_multi_producer_stress()
{
    const fs::path dir = fresh_dir("stress");
    const fs::path file = dir / "t.jsonl";
    constexpr int kThreads = 6;
    constexpr uint64_t kPer = 5000;
    std::atomic<uint64_t> ok{0};
    {
        auto log = TelemetryLog::open(file, 1024);
        SW_CHECK(log != nullptr);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t] {
                for (uint64_t i = 0; i < kPer; ++i) {
                    if (log->emit_read_miss_start(
                            {.request_id = uint64_t(t) * kPer + i, .path = "a.bin"})) {
                        ok.fetch_add(1);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& t : ts)
            t.join();
        log->flush();
        SW_CHECK(log->written() == ok.load());
        SW_CHECK(log->written() + log->dropped() == uint64_t{kThreads} * kPer);
    }
    const auto lines = read_lines(file);
    SW_CHECK(lines.size() == ok.load() + 1);
    std::vector<bool> seen(kThreads * kPer, false); // sem linha repetida nem corrompida
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        SW_CHECK(has(lines[i], "\"event\":\"read_miss_start\"") && lines[i].back() == '}');
        const uint64_t id = num(lines[i], "request_id");
        SW_CHECK(id < seen.size() && !seen[id]);
        seen[id] = true;
    }
    cleanup(dir);
}

// Disco cheio: o erro aparece no fflush, não no fwrite. Os eventos do buffer têm de virar
// write_errors (e não written), e o resumo do arquivo tem de dizer isso.
void test_flush_failure_is_counted()
{
    const fs::path dir = fresh_dir("flushfail");
    const fs::path file = dir / "t.jsonl";
    uint64_t errors = 0, written = 0;
    {
        auto log = TelemetryLog::open(file);
        SW_CHECK(log != nullptr);
        TelemetryLogTestHook::fail_next_flush(*log);
        for (uint64_t i = 0; i < 5; ++i)
            SW_CHECK(log->emit_eviction(i, i));
        log->flush();
        errors = log->write_errors();
        written = log->written();
        SW_CHECK(errors >= 1);                // a falha foi vista, não engolida
        SW_CHECK(errors + written == 5);      // cada evento conta em uma coluna só
        SW_CHECK(log->emit_eviction(99, 99)); // depois da falha, volta a gravar
        log->flush();
        SW_CHECK(log->write_errors() == errors && log->written() == written + 1);
        written = log->written();
    }
    const auto lines = read_lines(file);
    SW_CHECK(num(lines.back(), "write_errors") == errors);
    SW_CHECK(num(lines.back(), "written") == written);
    cleanup(dir);
}

// "Não aloca": 80 mil emits dos 4 tipos, com e sem anel cheio, sem nenhuma alocação na thread
// que emite. O controle positivo prova que o contador funciona.
void test_emit_does_not_allocate()
{
    const fs::path dir = fresh_dir("alloc");
    auto log = TelemetryLog::open(dir / "t.jsonl", 64); // anel pequeno: também enche
    SW_CHECK(log != nullptr);

    g_allocs = 0;
    g_count_allocs = true;
    auto* control = new int(1); // controle positivo
    g_count_allocs = false;
    delete control;
    SW_CHECK(g_allocs == 1);

    g_allocs = 0;
    g_count_allocs = true;
    for (uint64_t i = 0; i < 20000; ++i) {
        (void)log->emit_read_miss_start({.request_id = i, .path = "core/mapa.bin", .size = 4096});
        (void)log->emit_read_miss({.request_id = i, .call_result = "SW_OK"});
        (void)log->emit_eviction(i, i);
        (void)log->emit_frame_stutter(i, "io_wait");
    }
    g_count_allocs = false;
    SW_CHECK(g_allocs == 0);
    SW_CHECK(log->dropped() > 0); // o caminho de anel cheio também foi exercitado
    log.reset();
    cleanup(dir);
}

// A dona do cache emite `eviction` por lote; a soma bate com os contadores.
void test_cache_owner_eviction_events()
{
    const fs::path dir = fresh_dir("owner");
    const fs::path file = dir / "t.jsonl";
    constexpr size_t kBytes = 4096;
    uint64_t pieces = 0, bytes = 0;
    {
        auto log = TelemetryLog::open(file);
        SW_CHECK(log != nullptr);
        fs::create_directories(dir / "cache");
        CacheOwnerConfig cfg{.dir = dir / "cache",
                             .total_slots = 16,
                             .layout = {.slots_per_file = 8, .slot_size = 64 * 1024}};
        cfg.evict_high_percent = kDefaultEvictHighPercent;
        cfg.evict_low_percent = kDefaultEvictLowPercent;
        cfg.telemetry = log.get();
        auto started = CacheOwner::start(cfg);
        SW_CHECK(started.has_value());
        auto& owner = **started;
        std::vector<std::byte> data(kBytes, std::byte{0x5a});
        for (uint32_t i = 0; i < 100; ++i) {
            PieceHash h{};
            for (size_t b = 0; b < sizeof i; ++b)
                h.bytes[b] = static_cast<uint8_t>(i >> (8 * b));
            auto req = CacheRequest::put(PieceId(i, h), data);
            SW_CHECK(owner.call(req).has_value());
        }
        (void)owner.stop();
        pieces = owner.evicted_pieces();
        bytes = owner.evicted_bytes();
        SW_CHECK(pieces > 0);
        log->flush();
        SW_CHECK(log->dropped() == 0);
    }
    uint64_t sum_p = 0, sum_b = 0;
    for (const auto& l : read_lines(dir / "t.jsonl")) {
        if (!has(l, "\"event\":\"eviction\"")) continue;
        sum_p += num(l, "pieces");
        sum_b += num(l, "bytes");
    }
    SW_CHECK(sum_p == pieces && sum_b == bytes);
    SW_CHECK(sum_b == pieces * kBytes);
    cleanup(dir);
}

} // namespace

} // namespace swarmstream

int main()
{
    using namespace swarmstream;
    test_open_failure();
    test_fields();
    test_path_truncated_and_control_bytes();
    test_drop_is_counted();
    test_multi_producer_stress();
    test_flush_failure_is_counted();
    test_emit_does_not_allocate();
    test_cache_owner_eviction_events();
    std::cout << "telemetry: ok\n";
    return 0;
}
