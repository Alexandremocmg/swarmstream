// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

namespace swarmstream {

namespace {

enum class Kind : uint8_t { ReadMissStart, ReadMiss, Eviction, FrameStutter };

// Evento de tamanho fixo: copiar para o anel não aloca.
struct Event
{
    Kind kind = Kind::ReadMissStart;
    MissOutcome outcome = MissOutcome::Completed;
    MissSource source = MissSource::None;
    uint64_t ts_us = 0; // instante da emissão (eviction e frame_stutter)
    uint64_t request_id = 0;
    uint64_t t0_us = 0;
    uint64_t t1_us = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t a = 0; // eviction: peças; frame_stutter: duração em us
    uint64_t b = 0; // eviction: bytes
    uint32_t deadline_ms = 0;
    uint32_t timeout_ms = 0;
    char path[TelemetryLog::kMaxPath + 1] = {};
    char text1[TelemetryLog::kMaxText + 1] = {}; // call_result | cause
    char text2[TelemetryLog::kMaxText + 1] = {}; // error
};

template <size_t N>
void copy_text(char (&dst)[N], std::string_view s) noexcept
{
    const size_t n = std::min(s.size(), N - 1);
    if (n != 0) std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

// Acrescenta `s` entre aspas, escapado como JSON. Byte >= 0x80 vira \u00XX (válido, com perda
// se o texto era UTF-8: caminhos de jogo são ASCII).
void append_string(char*& p, const char* end, const char* s) noexcept
{
    auto put = [&](char c) {
        if (p < end) *p++ = c;
    };
    put('"');
    for (; *s != '\0'; ++s) {
        const auto c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            put('\\');
            put(static_cast<char>(c));
        } else if (c < 0x20 || c >= 0x80) {
            char tmp[8];
            const int n = std::snprintf(tmp, sizeof tmp, "\\u%04x", static_cast<unsigned>(c));
            for (int i = 0; i < n; ++i)
                put(tmp[i]);
        } else {
            put(static_cast<char>(c));
        }
    }
    put('"');
}

const char* outcome_name(MissOutcome o) noexcept
{
    switch (o) {
    case MissOutcome::Completed: return "completed";
    case MissOutcome::Failed: return "failed";
    case MissOutcome::Cancelled: return "cancelled";
    case MissOutcome::PendingAtEnd: return "pending_at_end";
    }
    return "failed";
}

const char* source_name(MissSource s) noexcept
{
    switch (s) {
    case MissSource::Cdn: return "cdn";
    case MissSource::Peer: return "peer";
    case MissSource::None: break;
    }
    return nullptr;
}

// Formata a linha (com \n) em buf; devolve o tamanho. Truncada só se buf for pequeno demais
// (o buffer do writer comporta o pior caso: 6 campos de texto escapados em \u00XX).
size_t format_line(const Event& e, char* buf, size_t cap) noexcept
{
    char* p = buf;
    char* const end = buf + cap - 1; // reserva o \n
    auto lit = [&](const char* s) {
        for (; *s != '\0' && p < end; ++s)
            *p++ = *s;
    };
    auto num = [&](const char* name, uint64_t v) {
        lit(",\"");
        lit(name);
        lit("\":");
        char tmp[24];
        std::snprintf(tmp, sizeof tmp, "%" PRIu64, v);
        lit(tmp);
    };
    auto str = [&](const char* name, const char* v) {
        lit(",\"");
        lit(name);
        lit("\":");
        append_string(p, end, v);
    };
    switch (e.kind) {
    case Kind::ReadMissStart:
        lit("{\"event\":\"read_miss_start\"");
        num("request_id", e.request_id);
        str("path", e.path);
        num("offset", e.offset);
        num("size", e.size);
        num("deadline_ms", e.deadline_ms);
        num("timeout_ms", e.timeout_ms);
        num("t0_us", e.t0_us);
        break;
    case Kind::ReadMiss:
        lit("{\"event\":\"read_miss\"");
        num("request_id", e.request_id);
        num("t1_us", e.t1_us);
        if (e.outcome == MissOutcome::Completed) {
            num("miss_wait_us", e.t1_us >= e.t0_us ? e.t1_us - e.t0_us : 0);
        }
        str("call_result", e.text1);
        str("outcome", outcome_name(e.outcome));
        if (const char* src = source_name(e.source)) str("source", src);
        if (e.outcome == MissOutcome::Failed) str("error", e.text2);
        break;
    case Kind::Eviction:
        lit("{\"event\":\"eviction\"");
        num("t_us", e.ts_us);
        num("pieces", e.a);
        num("bytes", e.b);
        break;
    case Kind::FrameStutter:
        lit("{\"event\":\"frame_stutter\"");
        num("t_us", e.ts_us);
        num("duration_us", e.a);
        str("cause", e.text1);
        break;
    }
    lit("}");
    *p++ = '\n';
    return static_cast<size_t>(p - buf);
}

} // namespace

struct TelemetryLog::Impl
{
    struct Cell
    {
        std::atomic<uint64_t> seq{0};
        Event ev;
    };

    std::FILE* file = nullptr;
    Cell* cells = nullptr;
    uint64_t mask = 0;
    alignas(64) std::atomic<uint64_t> enq{0}; // produtoras
    alignas(64) uint64_t deq = 0;             // só a gravadora
    alignas(64) std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> written{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<bool> stop{false};
    std::thread writer;

    // Fila limitada de Vyukov: muitas produtoras, uma consumidora. Sem alocação, sem trava.
    bool push(const Event& e) noexcept
    {
        uint64_t pos = enq.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells[pos & mask];
            const uint64_t seq = c.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<int64_t>(seq - pos);
            if (dif == 0) {
                if (enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.ev = e;
                    c.seq.store(pos + 1, std::memory_order_release);
                    accepted.fetch_add(1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                dropped.fetch_add(1, std::memory_order_relaxed); // anel cheio
                return false;
            } else {
                pos = enq.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(Event& out) noexcept
    {
        Cell& c = cells[deq & mask];
        if (c.seq.load(std::memory_order_acquire) != deq + 1) return false;
        out = c.ev;
        c.seq.store(deq + mask + 1, std::memory_order_release);
        ++deq;
        return true;
    }

    void write_event(const Event& e) noexcept
    {
        char buf[1536]; // pior caso: path 160 x 6 bytes + campos
        const size_t n = format_line(e, buf, sizeof buf);
        if (std::fwrite(buf, 1, n, file) == n) {
            written.fetch_add(1, std::memory_order_relaxed);
        } else {
            write_errors.fetch_add(1, std::memory_order_relaxed);
        }
        processed.fetch_add(1, std::memory_order_release);
    }

    void drain() noexcept
    {
        Event e;
        while (pop(e))
            write_event(e);
    }

    void run() noexcept
    {
        for (;;) {
            const bool stopping = stop.load(std::memory_order_acquire);
            drain();
            if (stopping) return; // o drain depois de ver `stop` pega tudo o que já entrou
            std::fflush(file);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

TelemetryLog::TelemetryLog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

uint64_t TelemetryLog::now_us() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::unique_ptr<TelemetryLog> TelemetryLog::open(const std::filesystem::path& file,
                                                 size_t capacity) noexcept
{
    try {
        uint64_t cap = 2;
        while (cap < capacity && cap < (uint64_t{1} << 24))
            cap <<= 1;
        auto impl = std::make_unique<Impl>();
        impl->cells = new Impl::Cell[cap];
        for (uint64_t i = 0; i < cap; ++i)
            impl->cells[i].seq.store(i, std::memory_order_relaxed);
        impl->mask = cap - 1;
        impl->file = std::fopen(file.string().c_str(), "wb");
        if (impl->file == nullptr) {
            delete[] impl->cells;
            return nullptr;
        }
        Impl* raw = impl.get();
        raw->writer = std::thread([raw] { raw->run(); });
        return std::unique_ptr<TelemetryLog>(new TelemetryLog(std::move(impl)));
    } catch (...) {
        return nullptr;
    }
}

TelemetryLog::~TelemetryLog()
{
    Impl& m = *impl_;
    m.stop.store(true, std::memory_order_release);
    m.writer.join();
    // Resumo: perda e erro de escrita não podem passar em silêncio (docs/08).
    char buf[200];
    const int n = std::snprintf(buf, sizeof buf,
                                "{\"event\":\"telemetry_summary\",\"written\":%" PRIu64
                                ",\"dropped\":%" PRIu64 ",\"write_errors\":%" PRIu64 "}\n",
                                m.written.load(), m.dropped.load(), m.write_errors.load());
    if (n > 0) (void)std::fwrite(buf, 1, static_cast<size_t>(n), m.file);
    (void)std::fclose(m.file);
    delete[] m.cells;
}

bool TelemetryLog::emit_read_miss_start(const ReadMissStart& s) noexcept
{
    Event e;
    e.kind = Kind::ReadMissStart;
    e.request_id = s.request_id;
    copy_text(e.path, s.path);
    e.offset = s.offset;
    e.size = s.size;
    e.deadline_ms = s.deadline_ms;
    e.timeout_ms = s.timeout_ms;
    e.t0_us = s.t0_us;
    return impl_->push(e);
}

bool TelemetryLog::emit_read_miss(const ReadMissEnd& s) noexcept
{
    Event e;
    e.kind = Kind::ReadMiss;
    e.request_id = s.request_id;
    e.t0_us = s.t0_us;
    e.t1_us = s.t1_us;
    e.outcome = s.outcome;
    e.source = s.source;
    copy_text(e.text1, s.call_result);
    copy_text(e.text2, s.error);
    return impl_->push(e);
}

bool TelemetryLog::emit_eviction(uint64_t pieces, uint64_t bytes) noexcept
{
    Event e;
    e.kind = Kind::Eviction;
    e.ts_us = now_us();
    e.a = pieces;
    e.b = bytes;
    return impl_->push(e);
}

bool TelemetryLog::emit_frame_stutter(uint64_t duration_us, std::string_view cause) noexcept
{
    Event e;
    e.kind = Kind::FrameStutter;
    e.ts_us = now_us();
    e.a = duration_us;
    copy_text(e.text1, cause);
    return impl_->push(e);
}

uint64_t TelemetryLog::dropped() const noexcept
{
    return impl_->dropped.load(std::memory_order_relaxed);
}
uint64_t TelemetryLog::written() const noexcept
{
    return impl_->written.load(std::memory_order_relaxed);
}
uint64_t TelemetryLog::write_errors() const noexcept
{
    return impl_->write_errors.load(std::memory_order_relaxed);
}

void TelemetryLog::flush() noexcept
{
    Impl& m = *impl_;
    while (m.processed.load(std::memory_order_acquire) < m.accepted.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::fflush(m.file); // o stdio trava o FILE* por dentro: seguro com a thread gravadora
}

} // namespace swarmstream
