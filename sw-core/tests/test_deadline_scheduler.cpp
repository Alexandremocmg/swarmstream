// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-28: agendador por prazo v1. Fonte falsa controlada pelo teste (cada resposta sai quando o
// teste manda), para a ordem de saída ser determinística. Espera positiva tem limite de 5 s;
// "não aconteceu" é conferido depois de uma pausa curta. O job TSan do CI roda este teste.
#include "check.hpp"
#include "swarmstream/deadline_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using swarmstream::DeadlineScheduler;
using swarmstream::IContentSource;
using swarmstream::kInvalidRequestId;
using swarmstream::PieceHash;
using swarmstream::PieceId;
using swarmstream::PieceResponse;
using swarmstream::RequestId;
using swarmstream::RequestKind;
using swarmstream::RequestTicket;
using swarmstream::SchedulerConfig;
using swarmstream::SourceErrorCode;
using swarmstream::SourceStats;
using swarmstream::SteadyTimePoint;
using Clock = std::chrono::steady_clock;

constexpr auto kWait = 5s;    // limite das esperas positivas
constexpr auto kQuiet = 60ms; // pausa para conferir que algo NÃO aconteceu

PieceId piece(uint32_t i)
{
    PieceHash hash;
    hash.bytes[0] = static_cast<uint8_t>(i);
    hash.bytes[1] = static_cast<uint8_t>(i >> 8);
    hash.bytes[31] = 0xA5;
    return PieceId(i, hash);
}

std::vector<uint8_t> bytes_of(const PieceId& p)
{
    return {static_cast<uint8_t>(p.index), static_cast<uint8_t>(p.index >> 8), 0x5A, 0x01};
}

SteadyTimePoint in(std::chrono::milliseconds ms) { return Clock::now() + ms; }

// Fonte falsa. Modo manual: a resposta só sai com complete()/fail(). Modo automático: uma thread
// responde cada pedido depois de uma pausa curta. cancel() responde Canceled se ainda pendente
// (como a HttpRangeSource).
class FakeSource final : public IContentSource
{
public:
    struct Call
    {
        RequestId id;
        PieceId piece;
        SteadyTimePoint deadline;
    };

    explicit FakeSource(bool automatic = false) : automatic_(automatic)
    {
        if (automatic_) {
            completer_ = std::thread([this] { auto_loop(); });
        }
    }

    ~FakeSource() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            gate_closed_ = false;
        }
        cv_.notify_all();
        if (completer_.joinable()) completer_.join();
    }

    RequestTicket request(const PieceId& p, SteadyTimePoint deadline) override
    {
        std::unique_lock lock(mutex_);
        ++entered_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !gate_closed_; });
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error("falha de teste em request()");
        }
        const RequestId id = next_id_++;
        calls_.push_back(Call{id, p, deadline});
        piece_by_id_.emplace(id, p);
        if (invalid_future_next_) {
            invalid_future_next_ = false;
            cv_.notify_all();
            return RequestTicket{id, {}};
        }
        auto& promise = promises_[id];
        RequestTicket ticket{id, promise.get_future()};
        if (automatic_) auto_queue_.push_back(id);
        cv_.notify_all();
        return ticket;
    }

    bool cancel(RequestId id) override
    {
        std::lock_guard lock(mutex_);
        cancels_.push_back(id);
        cv_.notify_all();
        if (ignore_cancel_) return false;
        const auto it = promises_.find(id);
        if (it == promises_.end()) return false;
        it->second.set_value(
            PieceResponse{piece_of(id), {}, SourceErrorCode::Canceled, "cancelado"});
        promises_.erase(it);
        cv_.notify_all();
        return true;
    }

    SourceStats stats() const override { return {}; }
    std::string_view name() const noexcept override { return "fake"; }

    void complete(RequestId id)
    {
        std::lock_guard lock(mutex_);
        const auto it = promises_.find(id);
        SW_CHECK(it != promises_.end());
        const PieceId p = piece_of(id);
        it->second.set_value(PieceResponse{p, bytes_of(p), SourceErrorCode::Success, ""});
        promises_.erase(it);
    }

    void fail(RequestId id, SourceErrorCode code)
    {
        std::lock_guard lock(mutex_);
        const auto it = promises_.find(id);
        SW_CHECK(it != promises_.end());
        it->second.set_value(PieceResponse{piece_of(id), {}, code, "falha de teste"});
        promises_.erase(it);
    }

    // Completa o pedido número `n` (ordem de chegada, a partir de 0).
    void complete_nth(std::size_t n) { complete(call(n).id); }

    [[nodiscard]] bool wait_calls(std::size_t n)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, kWait, [&] { return calls_.size() >= n; });
    }

    [[nodiscard]] bool wait_entered(std::size_t n)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, kWait, [&] { return entered_ >= n; });
    }

    [[nodiscard]] std::size_t call_count() const
    {
        std::lock_guard lock(mutex_);
        return calls_.size();
    }

    [[nodiscard]] Call call(std::size_t n) const
    {
        std::lock_guard lock(mutex_);
        SW_CHECK(n < calls_.size());
        return calls_[n];
    }

    // Espera `n` cancelamentos e confere, depois de uma pausa, que não veio mais nenhum.
    [[nodiscard]] std::vector<RequestId> wait_cancels(std::size_t n)
    {
        {
            std::unique_lock lock(mutex_);
            SW_CHECK(cv_.wait_for(lock, kWait, [&] { return cancels_.size() >= n; }));
        }
        std::this_thread::sleep_for(kQuiet);
        return cancels();
    }

    [[nodiscard]] std::vector<RequestId> cancels() const
    {
        std::lock_guard lock(mutex_);
        return cancels_;
    }

    void close_gate()
    {
        std::lock_guard lock(mutex_);
        gate_closed_ = true;
    }

    void open_gate()
    {
        {
            std::lock_guard lock(mutex_);
            gate_closed_ = false;
        }
        cv_.notify_all();
    }

    // cancel() é registrado mas não responde: a resposta só sai com complete()/fail().
    void ignore_cancel()
    {
        std::lock_guard lock(mutex_);
        ignore_cancel_ = true;
    }

    void throw_next()
    {
        std::lock_guard lock(mutex_);
        throw_next_ = true;
    }

    // Responde o pedido com uma exceção que não deriva de std::exception (um int).
    void fail_with_int(RequestId id)
    {
        std::lock_guard lock(mutex_);
        const auto it = promises_.find(id);
        SW_CHECK(it != promises_.end());
        it->second.set_exception(std::make_exception_ptr(42));
        promises_.erase(it);
    }

    void invalid_future_next()
    {
        std::lock_guard lock(mutex_);
        invalid_future_next_ = true;
    }

private:
    PieceId piece_of(RequestId id) const
    {
        const auto it = piece_by_id_.find(id);
        SW_CHECK(it != piece_by_id_.end());
        return it->second;
    }

    void auto_loop()
    {
        std::mt19937 rng(1234);
        std::unique_lock lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return stopping_ || !auto_queue_.empty(); });
            if (stopping_) break;
            const RequestId id = auto_queue_.front();
            auto_queue_.pop_front();
            const auto pause = std::chrono::microseconds(rng() % 300);
            lock.unlock();
            std::this_thread::sleep_for(pause);
            lock.lock();
            const auto it = promises_.find(id);
            if (it == promises_.end()) continue; // cancelado
            const PieceId p = piece_of(id);
            it->second.set_value(PieceResponse{p, bytes_of(p), SourceErrorCode::Success, ""});
            promises_.erase(it);
        }
        // Encerrando: nenhum pedido fica sem resposta.
        for (auto& [id, promise] : promises_) {
            promise.set_value(PieceResponse{piece_of(id), {}, SourceErrorCode::Canceled, "fim"});
        }
        promises_.clear();
    }

    const bool automatic_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Call> calls_;
    std::unordered_map<RequestId, PieceId> piece_by_id_;
    std::map<RequestId, std::promise<PieceResponse>> promises_;
    std::vector<RequestId> cancels_;
    std::deque<RequestId> auto_queue_;
    RequestId next_id_ = 1000;
    std::size_t entered_ = 0;
    bool gate_closed_ = false;
    bool throw_next_ = false;
    bool ignore_cancel_ = false;
    bool invalid_future_next_ = false;
    bool stopping_ = false;
    std::thread completer_;
};

// Guarda cada callback recebido.
class Recorder
{
public:
    struct Delivery
    {
        RequestId id;
        PieceId piece;
        SourceErrorCode error;
        std::vector<uint8_t> data;
        std::thread::id thread;
        std::string message;
    };

    swarmstream::SchedulerCallback callback()
    {
        return [this](RequestId id, const PieceResponse& r) {
            std::lock_guard lock(mutex_);
            deliveries_.push_back(Delivery{id, r.piece, r.error, r.data, std::this_thread::get_id(),
                                           r.error_message});
            cv_.notify_all();
        };
    }

    [[nodiscard]] bool wait_count(std::size_t n)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, kWait, [&] { return deliveries_.size() >= n; });
    }

    [[nodiscard]] std::size_t count() const
    {
        std::lock_guard lock(mutex_);
        return deliveries_.size();
    }

    [[nodiscard]] std::size_t count_for(RequestId id) const
    {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(
            std::count_if(deliveries_.begin(), deliveries_.end(),
                          [id](const Delivery& d) { return d.id == id; }));
    }

    [[nodiscard]] Delivery get(RequestId id) const
    {
        std::lock_guard lock(mutex_);
        for (const auto& d : deliveries_) {
            if (d.id == id) return d;
        }
        SW_CHECK(false);
        return {};
    }

    [[nodiscard]] std::vector<Delivery> all() const
    {
        std::lock_guard lock(mutex_);
        return deliveries_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Delivery> deliveries_;
};

std::unique_ptr<DeadlineScheduler> make(IContentSource& source, unsigned max_in_flight,
                                        unsigned reserved)
{
    auto created = DeadlineScheduler::create(source, SchedulerConfig{max_in_flight, reserved});
    SW_CHECK(created.has_value());
    return std::move(created.value());
}

// Ocupa a única vaga com uma demanda que só termina quando o teste manda (pedido número 0).
RequestId occupy(DeadlineScheduler& s, FakeSource& src, Recorder& rec)
{
    const RequestId id = s.submit(piece(900), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(id != kInvalidRequestId);
    SW_CHECK(src.wait_calls(1));
    SW_CHECK(src.call(0).piece == piece(900));
    return id;
}

void check_order(const FakeSource& src, std::size_t first, const std::vector<uint32_t>& expected)
{
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SW_CHECK(src.call(first + i).piece == piece(expected[i]));
    }
}

// Completa, um a um, os pedidos de `first` em diante, esperando cada próximo chegar.
void drain_in_order(FakeSource& src, std::size_t first, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        SW_CHECK(src.wait_calls(first + i + 1));
        src.complete_nth(first + i);
    }
}

// ------------------------------------------------------------------------------------- testes

void test_config_validation()
{
    FakeSource src;
    SW_CHECK(!DeadlineScheduler::create(src, SchedulerConfig{0, 0}).has_value());
    SW_CHECK(!DeadlineScheduler::create(src, SchedulerConfig{65, 0}).has_value());
    SW_CHECK(!DeadlineScheduler::create(src, SchedulerConfig{2, 2}).has_value());
    SW_CHECK(!DeadlineScheduler::create(src, SchedulerConfig{1, 1}).has_value());
    SW_CHECK(DeadlineScheduler::create(src, SchedulerConfig{1, 0}).has_value());
    SW_CHECK(DeadlineScheduler::create(src, SchedulerConfig{64, 63}).has_value());
    std::puts("configuracao invalida recusada: ok");
}

// Critério de aceite: com a vaga ocupada por um prefetch e outros cinco prefetch na fila, uma
// demanda que chega depois (e com prazo MAIS TARDE que todos eles) é a próxima a sair.
void test_demand_passes_prefetch()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    for (uint32_t i = 1; i <= 6; ++i) {
        SW_CHECK(s->submit(piece(i), in(std::chrono::milliseconds(10000 + i)),
                           RequestKind::Prefetch, rec.callback()) != kInvalidRequestId);
    }
    SW_CHECK(src.wait_calls(1));
    SW_CHECK(src.call(0).piece == piece(1));
    const RequestId demand = s->submit(piece(50), in(20000ms), RequestKind::Demand, rec.callback());
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 1); // a vaga continua do prefetch 1: nada sai antes de terminar
    src.complete_nth(0);
    SW_CHECK(src.wait_calls(2));
    SW_CHECK(src.call(1).piece == piece(50));
    src.complete_nth(1);
    drain_in_order(src, 2, 5);
    check_order(src, 2, {2, 3, 4, 5, 6});
    SW_CHECK(rec.wait_count(7));
    SW_CHECK(rec.get(demand).error == SourceErrorCode::Success);
    SW_CHECK(rec.get(demand).data == bytes_of(piece(50)));
    const auto st = s->stats();
    SW_CHECK(st.sent_demand == 1);
    SW_CHECK(st.sent_prefetch == 6);
    SW_CHECK(st.completed == 7);
    std::puts("criterio: demanda passa na frente de prefetch: ok");
}

// Com uma vaga reservada, prefetch nunca ocupa todas as vagas: a demanda sai na hora, com o
// prefetch ainda na fonte.
void test_reserved_slot()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 2, 1);
    for (uint32_t i = 1; i <= 3; ++i) {
        (void)s->submit(piece(i), in(10000ms), RequestKind::Prefetch, rec.callback());
    }
    SW_CHECK(src.wait_calls(1));
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 1); // a segunda vaga é da demanda
    SW_CHECK(s->stats().in_flight == 1);
    SW_CHECK(s->stats().queued_prefetch == 2);
    (void)s->submit(piece(50), in(5000ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(2));
    SW_CHECK(src.call(1).piece == piece(50)); // saiu sem o prefetch 1 terminar
    // O prefetch 1 termina: a vaga de prefetch volta e o 2 sai (a demanda segue na fonte).
    src.complete_nth(0);
    SW_CHECK(src.wait_calls(3));
    SW_CHECK(src.call(2).piece == piece(2));
    src.complete_nth(1);
    src.complete_nth(2);
    drain_in_order(src, 3, 1);
    SW_CHECK(rec.wait_count(4));
    std::puts("vaga reservada para demanda: ok");
}

// Controle do anterior: sem reserva, prefetch usa as duas vagas.
// Lacuna A15 da revisão: prefetch preso na fila pelo limite de prefetch vira demanda quando o
// jogo pede a mesma peça. A thread da vaga reservada está dormindo (não havia demanda); a
// promoção tem de acordá-la, sem esperar outro evento (o prefetch na fonte não termina).
void test_promotion_wakes_reserved_slot()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 2, 1);
    (void)s->submit(piece(1), in(10000ms), RequestKind::Prefetch, rec.callback());
    SW_CHECK(src.wait_calls(1));
    (void)s->submit(piece(2), in(10000ms), RequestKind::Prefetch, rec.callback());
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 1); // a peça 2 está presa pelo limite de prefetch
    const RequestId d = s->submit(piece(2), in(20000ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(2)); // saiu pela vaga reservada, com a peça 1 ainda na fonte
    SW_CHECK(src.call(1).piece == piece(2));
    SW_CHECK(s->stats().sent_demand == 1);
    src.complete_nth(1);
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(d).data == bytes_of(piece(2)));
    src.complete_nth(0);
    SW_CHECK(rec.wait_count(3));
    std::puts("promocao a demanda acorda a vaga reservada: ok");
}

void test_no_reservation_uses_all_slots()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 2, 0);
    for (uint32_t i = 1; i <= 3; ++i) {
        (void)s->submit(piece(i), in(10000ms), RequestKind::Prefetch, rec.callback());
    }
    SW_CHECK(src.wait_calls(2));
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 2);
    drain_in_order(src, 0, 3);
    SW_CHECK(rec.wait_count(3));
    std::puts("sem reserva, prefetch usa todas as vagas: ok");
}

// Dentro de cada tipo, o prazo mais cedo sai primeiro; empate, quem chegou antes.
void test_deadline_order_within_kind()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const auto base = Clock::now() + 10s;
    (void)s->submit(piece(1), base + 3s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(2), base + 1s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(3), base + 2s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(4), base + 3s, RequestKind::Demand, rec.callback());
    (void)s->submit(piece(5), base + 1s, RequestKind::Demand, rec.callback());
    (void)s->submit(piece(6), base + 1s, RequestKind::Demand, rec.callback()); // empate com o 5
    (void)s->submit(piece(7), base + 2s, RequestKind::Demand, rec.callback());
    src.complete_nth(0);
    drain_in_order(src, 1, 7);
    check_order(src, 1, {5, 6, 7, 4, 2, 3, 1});
    SW_CHECK(src.call(1).deadline == base + 1s); // o prazo é repassado à fonte
    SW_CHECK(rec.wait_count(8));
    std::puts("ordem por prazo dentro do tipo e empate por chegada: ok");
}

// Pedidos da mesma peça viram um pedido à fonte; os dois callbacks recebem os dados.
void test_coalescing()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const RequestId a = s->submit(piece(1), in(500ms), RequestKind::Demand, rec.callback());
    const RequestId b = s->submit(piece(1), in(900ms), RequestKind::Demand, rec.callback());
    SW_CHECK(a != b);
    SW_CHECK(s->stats().queued_demand == 1);
    src.complete_nth(0);
    drain_in_order(src, 1, 1);
    SW_CHECK(rec.wait_count(3));
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 2);
    SW_CHECK(rec.get(a).data == bytes_of(piece(1)));
    SW_CHECK(rec.get(b).data == bytes_of(piece(1)));
    SW_CHECK(rec.get(b).piece == piece(1));
    SW_CHECK(s->stats().coalesced == 1);
    std::puts("pedidos da mesma peca juntados: ok");
}

// Uma dica nova com prazo mais curto antecipa a peça; uma mais longa não atrasa.
void test_shorter_deadline_moves_piece_ahead()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const auto base = Clock::now() + 10s;
    (void)s->submit(piece(1), base + 5s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(2), base + 3s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(3), base + 4s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(1), base + 1s, RequestKind::Prefetch, rec.callback()); // antecipa
    (void)s->submit(piece(2), base + 9s, RequestKind::Prefetch, rec.callback()); // não atrasa
    src.complete_nth(0);
    drain_in_order(src, 1, 3);
    check_order(src, 1, {1, 2, 3});
    SW_CHECK(src.call(1).deadline == base + 1s);
    SW_CHECK(rec.wait_count(6));
    std::puts("prazo mais curto antecipa a peca: ok");
}

// Prefetch na fila vira demanda quando o jogo pede a mesma peça.
void test_prefetch_upgraded_by_demand()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const auto base = Clock::now() + 10s;
    (void)s->submit(piece(1), base + 1s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(2), base + 2s, RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(3), base + 3s, RequestKind::Demand, rec.callback());
    (void)s->submit(piece(2), base + 8s, RequestKind::Demand, rec.callback());
    SW_CHECK(s->stats().queued_demand == 2);
    SW_CHECK(s->stats().queued_prefetch == 1);
    src.complete_nth(0);
    drain_in_order(src, 1, 3);
    // 2 virou demanda e mantém o prazo menor (2 s), então sai antes do 3 (3 s).
    check_order(src, 1, {2, 3, 1});
    SW_CHECK(rec.wait_count(5));
    std::puts("prefetch promovido a demanda: ok");
}

// Demanda de uma peça que já está na fonte como prefetch só espera a resposta.
void test_demand_joins_in_flight()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 2, 0);
    const RequestId p = s->submit(piece(1), in(10000ms), RequestKind::Prefetch, rec.callback());
    SW_CHECK(src.wait_calls(1));
    const RequestId d = s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback());
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 1);
    SW_CHECK(s->pending(piece(1)));
    src.complete_nth(0);
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(p).error == SourceErrorCode::Success);
    SW_CHECK(rec.get(d).data == bytes_of(piece(1)));
    SW_CHECK(s->stats().coalesced == 1);
    std::puts("demanda se junta a peca ja na fonte: ok");
}

void test_cancel_queued()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const RequestId x = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(s->pending(piece(1)));
    SW_CHECK(s->cancel(x));
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(x).error == SourceErrorCode::Canceled);
    SW_CHECK(rec.get(x).piece == piece(1));
    SW_CHECK(!s->pending(piece(1)));
    SW_CHECK(!s->cancel(x)); // já terminou
    src.complete_nth(0);
    SW_CHECK(rec.wait_count(2));
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 1); // a peça 1 nunca foi à fonte
    SW_CHECK(src.cancels().empty());
    SW_CHECK(rec.count_for(x) == 1);
    SW_CHECK(s->stats().canceled == 1);
    SW_CHECK(s->stats().queued_demand == 0);
    std::puts("cancelar pedido na fila: ok");
}

// Cancelar um de dois pedidos da mesma peça: o outro continua e a peça perde a prioridade
// que só o cancelado dava.
void test_cancel_one_of_two()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const RequestId d = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    const RequestId p = s->submit(piece(1), in(5000ms), RequestKind::Prefetch, rec.callback());
    (void)s->submit(piece(2), in(3000ms), RequestKind::Prefetch, rec.callback());
    SW_CHECK(s->cancel(d));
    SW_CHECK(s->pending(piece(1)));
    SW_CHECK(s->stats().queued_demand == 0);
    SW_CHECK(s->stats().queued_prefetch == 2);
    src.complete_nth(0);
    drain_in_order(src, 1, 2);
    check_order(src, 1, {2, 1}); // a peça 1 voltou a ser prefetch de 5 s
    SW_CHECK(rec.wait_count(4));
    SW_CHECK(rec.get(d).error == SourceErrorCode::Canceled);
    SW_CHECK(rec.get(p).data == bytes_of(piece(1)));
    std::puts("cancelar um de dois pedidos da mesma peca: ok");
}

void test_cancel_in_flight()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    const RequestId x = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(1));
    SW_CHECK(s->cancel(x));
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(x).error == SourceErrorCode::Canceled);
    const auto cancels = src.wait_cancels(1);
    SW_CHECK(cancels.size() == 1);
    SW_CHECK(cancels[0] == src.call(0).id);
    // Um pedido novo da mesma peça vai à fonte de novo, sem herdar o cancelamento.
    const RequestId y = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(2));
    src.complete_nth(1);
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(y).data == bytes_of(piece(1)));
    SW_CHECK(rec.count_for(x) == 1);
    std::puts("cancelar pedido na fonte: ok");
}

// A fonte não atende o cancelamento na hora (a transferência continua): um pedido novo da mesma
// peça vai à fonte de novo e não recebe a resposta do pedido cancelado.
void test_cancel_in_flight_slow_source()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 2, 0);
    src.ignore_cancel();
    const RequestId x = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(1));
    SW_CHECK(s->cancel(x));
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(src.wait_cancels(1).size() == 1);
    SW_CHECK(!s->pending(piece(1))); // ninguém espera a peça
    const RequestId y = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(2));
    SW_CHECK(src.call(1).piece == piece(1));
    src.fail(src.call(0).id, SourceErrorCode::Canceled); // a resposta atrasada do cancelado
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(rec.count_for(y) == 0);
    src.complete_nth(1);
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(y).data == bytes_of(piece(1)));
    SW_CHECK(rec.count_for(x) == 1);
    SW_CHECK(s->stats().in_flight == 0);
    std::puts("cancelar na fonte que demora a atender: ok");
}

// Cancelamento que chega enquanto a thread ainda está dentro de IContentSource::request: o
// cancelamento é repassado à fonte assim que o id do pedido existir.
void test_cancel_while_requesting()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    src.close_gate();
    const RequestId x = s->submit(piece(1), in(100ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_entered(1));
    SW_CHECK(s->cancel(x));
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(x).error == SourceErrorCode::Canceled);
    src.open_gate();
    SW_CHECK(src.wait_calls(1));
    const auto cancels = src.wait_cancels(1);
    SW_CHECK(cancels.size() == 1);
    SW_CHECK(cancels[0] == src.call(0).id);
    SW_CHECK(rec.count_for(x) == 1);
    std::puts("cancelar durante o request da fonte: ok");
}

void test_cancel_unknown_or_done()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    SW_CHECK(!s->cancel(kInvalidRequestId));
    SW_CHECK(!s->cancel(123456));
    const RequestId x = s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(1));
    src.complete_nth(0);
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(!s->cancel(x));
    SW_CHECK(rec.count_for(x) == 1);
    SW_CHECK(rec.get(x).error == SourceErrorCode::Success);
    std::puts("cancelar id desconhecido ou concluido: ok");
}

// Prefetch vencido na fila é descartado sem ir à fonte; demanda atrasada vai.
void test_expired_prefetch_dropped()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const RequestId late_hint =
        s->submit(piece(1), in(20ms), RequestKind::Prefetch, rec.callback());
    const RequestId hint = s->submit(piece(2), in(10000ms), RequestKind::Prefetch, rec.callback());
    const RequestId late_demand =
        s->submit(piece(3), in(-1000ms), RequestKind::Demand, rec.callback());
    std::this_thread::sleep_for(kQuiet);
    src.complete_nth(0);
    drain_in_order(src, 1, 2);
    check_order(src, 1, {3, 2});
    SW_CHECK(rec.wait_count(4));
    std::this_thread::sleep_for(kQuiet);
    SW_CHECK(src.call_count() == 3);
    SW_CHECK(rec.get(late_hint).error == SourceErrorCode::Timeout);
    SW_CHECK(rec.get(late_hint).piece == piece(1));
    SW_CHECK(rec.get(hint).error == SourceErrorCode::Success);
    SW_CHECK(rec.get(late_demand).error == SourceErrorCode::Success);
    SW_CHECK(s->stats().expired == 1);
    std::puts("prefetch vencido descartado, demanda atrasada enviada: ok");
}

// O callback nunca roda dentro de submit/cancel e pode chamar o agendador de novo.
void test_callback_thread_and_reentrancy()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    const auto main_thread = std::this_thread::get_id();
    std::atomic<RequestId> inner{kInvalidRequestId};
    std::atomic<bool> inner_cancel{true};
    std::atomic<bool> outer_on_main{true};
    const RequestId outer =
        s->submit(piece(1), in(0ms), RequestKind::Demand, [&](RequestId, const PieceResponse&) {
            outer_on_main = std::this_thread::get_id() == main_thread;
            inner = s->submit(piece(2), in(0ms), RequestKind::Demand, rec.callback());
            inner_cancel = s->cancel(987654);
            (void)s->pending(piece(2));
        });
    SW_CHECK(outer != kInvalidRequestId);
    SW_CHECK(src.wait_calls(1));
    src.complete_nth(0);
    SW_CHECK(src.wait_calls(2)); // a peça 2, pedida de dentro do callback, está na fonte
    SW_CHECK(src.call(1).piece == piece(2));
    SW_CHECK(inner.load() != kInvalidRequestId);
    SW_CHECK(!inner_cancel.load());
    SW_CHECK(!outer_on_main.load());
    // Cancelado na fila pela thread principal: o callback vem em outra thread.
    const RequestId q = s->submit(piece(3), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(s->cancel(q));
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(q).error == SourceErrorCode::Canceled);
    SW_CHECK(rec.get(q).thread != main_thread);
    src.complete_nth(1);
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(inner.load()).thread != main_thread);
    SW_CHECK(rec.get(inner.load()).data == bytes_of(piece(2)));
    std::puts("callback fora de submit/cancel e reentrante: ok");
}

void test_source_error_propagates()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    occupy(*s, src, rec);
    const RequestId a = s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback());
    const RequestId b = s->submit(piece(1), in(0ms), RequestKind::Prefetch, rec.callback());
    src.complete_nth(0);
    SW_CHECK(src.wait_calls(2));
    src.fail(src.call(1).id, SourceErrorCode::NetworkError);
    SW_CHECK(rec.wait_count(3));
    SW_CHECK(rec.get(a).error == SourceErrorCode::NetworkError);
    SW_CHECK(rec.get(b).error == SourceErrorCode::NetworkError);
    SW_CHECK(s->stats().failed == 2);
    std::puts("erro da fonte chega a todos os pedidos da peca: ok");
}

// Falhas da própria fonte (exceção, ticket sem future) viram erro no callback, nunca silêncio.
void test_source_misbehaves()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    src.throw_next();
    const RequestId a = s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(a).error == SourceErrorCode::NetworkError);
    SW_CHECK(rec.get(a).message.find("falha de teste em request()") != std::string::npos);
    src.invalid_future_next();
    const RequestId b = s->submit(piece(2), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(rec.wait_count(2));
    SW_CHECK(rec.get(b).error == SourceErrorCode::NetworkError);
    SW_CHECK(rec.get(b).message.find("sem future") != std::string::npos);
    // Exceção que não deriva de std::exception, vinda do future da fonte.
    const RequestId e = s->submit(piece(4), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(2));
    src.fail_with_int(src.call(1).id);
    SW_CHECK(rec.wait_count(3));
    SW_CHECK(rec.get(e).error == SourceErrorCode::NetworkError);
    // O agendador continua funcionando.
    const RequestId c = s->submit(piece(3), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(src.wait_calls(3));
    src.complete(src.call(2).id);
    SW_CHECK(rec.wait_count(4));
    SW_CHECK(rec.get(c).error == SourceErrorCode::Success);
    SW_CHECK(s->stats().failed == 3);
    std::puts("excecao e ticket invalido da fonte viram erro: ok");
}

void test_callback_exception_counted()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    (void)s->submit(piece(1), in(0ms), RequestKind::Demand,
                    [](RequestId, const PieceResponse&) { throw std::runtime_error("callback"); });
    const RequestId b = s->submit(piece(2), in(0ms), RequestKind::Demand, rec.callback());
    drain_in_order(src, 0, 2);
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(rec.get(b).error == SourceErrorCode::Success);
    SW_CHECK(s->stats().callback_exceptions == 1);
    std::puts("excecao no callback contada, entrega continua: ok");
}

// Callback vazio é aceito (pedido sem aviso).
void test_empty_callback()
{
    FakeSource src;
    auto s = make(src, 1, 0);
    const RequestId a = s->submit(piece(1), in(10000ms), RequestKind::Prefetch, {});
    SW_CHECK(a != kInvalidRequestId);
    SW_CHECK(src.wait_calls(1));
    src.complete_nth(0);
    const auto deadline = Clock::now() + kWait;
    while (s->stats().completed == 0 && Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    SW_CHECK(s->stats().completed == 1);
    SW_CHECK(!s->pending(piece(1)));
    std::puts("pedido sem callback: ok");
}

// O destrutor entrega todos os callbacks antes de voltar: os da fila com Canceled, os da fonte
// com o que ela responder ao cancelamento.
void test_destructor_delivers_everything()
{
    FakeSource src;
    Recorder rec;
    std::vector<RequestId> ids;
    {
        auto s = make(src, 2, 0);
        ids.push_back(s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback()));
        ids.push_back(s->submit(piece(2), in(0ms), RequestKind::Demand, rec.callback()));
        SW_CHECK(src.wait_calls(2));
        for (uint32_t i = 3; i < 10; ++i) {
            ids.push_back(s->submit(piece(i), in(1000ms), RequestKind::Prefetch, rec.callback()));
        }
        ids.push_back(s->submit(piece(3), in(0ms), RequestKind::Demand, rec.callback()));
    }
    SW_CHECK(rec.count() == ids.size()); // sem esperar: o destrutor já entregou
    for (const RequestId id : ids) {
        SW_CHECK(rec.count_for(id) == 1);
        SW_CHECK(rec.get(id).error == SourceErrorCode::Canceled);
    }
    SW_CHECK(src.cancels().size() == 2);
    SW_CHECK(src.call_count() == 2);
    std::puts("destrutor cancela e entrega tudo: ok");
}

// O destrutor espera a entrega terminar, mesmo com um callback lento ocupando a thread de
// entrega enquanto ele cancela o resto.
void test_destructor_drains_behind_slow_callback()
{
    FakeSource src;
    Recorder rec;
    std::vector<RequestId> ids;
    std::atomic<bool> slow_started{false};
    {
        auto s = make(src, 2, 0);
        (void)s->submit(piece(1), in(0ms), RequestKind::Demand,
                        [&](RequestId, const PieceResponse&) {
                            slow_started = true;
                            std::this_thread::sleep_for(300ms);
                        });
        ids.push_back(s->submit(piece(2), in(0ms), RequestKind::Demand, rec.callback()));
        SW_CHECK(src.wait_calls(2));
        for (uint32_t i = 3; i < 8; ++i) {
            ids.push_back(s->submit(piece(i), in(10000ms), RequestKind::Prefetch, rec.callback()));
        }
        src.complete_nth(0);
        const auto deadline = Clock::now() + kWait;
        while (!slow_started.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        SW_CHECK(slow_started.load());
    } // destrói com a thread de entrega presa no callback lento
    SW_CHECK(rec.count() == ids.size());
    for (const RequestId id : ids)
        SW_CHECK(rec.count_for(id) == 1);
    std::puts("destrutor espera a entrega atras de callback lento: ok");
}

void test_pending()
{
    FakeSource src;
    Recorder rec;
    auto s = make(src, 1, 0);
    SW_CHECK(!s->pending(piece(1)));
    (void)s->submit(piece(1), in(0ms), RequestKind::Demand, rec.callback());
    SW_CHECK(s->pending(piece(1)));
    SW_CHECK(src.wait_calls(1));
    SW_CHECK(s->pending(piece(1)));
    SW_CHECK(s->stats().in_flight == 1);
    src.complete_nth(0);
    SW_CHECK(rec.wait_count(1));
    SW_CHECK(!s->pending(piece(1)));
    SW_CHECK(s->stats().in_flight == 0);
    std::puts("pending na fila, na fonte e depois: ok");
}

// Estresse: 16 threads pedindo e cancelando peças de um conjunto pequeno (muita junção e
// cancelamento na fila e na fonte). Cada pedido aceito tem exatamente um callback e, quando dá
// certo, os bytes são da peça pedida.
void test_stress()
{
    FakeSource src(true);
    std::mutex mutex;
    std::unordered_map<RequestId, PieceId> accepted;
    std::unordered_map<RequestId, int> callbacks;
    std::unordered_map<RequestId, bool> bad;
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> ok_count{0};
    std::atomic<uint64_t> canceled_ok{0};
    constexpr int kThreads = 16;
    constexpr int kPerThread = 400;
    {
        auto s = make(src, 4, 1);
        auto callback = [&](RequestId id, const PieceResponse& r) {
            std::lock_guard lock(mutex);
            ++callbacks[id];
            if (r.ok()) {
                ok_count.fetch_add(1);
                const auto it = accepted.find(id);
                if (it == accepted.end() || r.piece != it->second || r.data != bytes_of(r.piece)) {
                    bad[id] = true;
                }
            }
            done.fetch_add(1);
        };
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937 rng(static_cast<uint32_t>(t) * 7919u + 17u);
                std::vector<RequestId> mine;
                for (int i = 0; i < kPerThread; ++i) {
                    const PieceId p = piece(rng() % 24);
                    const auto kind =
                        (rng() % 3 == 0) ? RequestKind::Demand : RequestKind::Prefetch;
                    const auto deadline = in(std::chrono::milliseconds(rng() % 50));
                    RequestId id;
                    {
                        // Registra antes do callback poder chegar.
                        std::lock_guard lock(mutex);
                        id = s->submit(p, deadline, kind, callback);
                        SW_CHECK(id != kInvalidRequestId);
                        accepted.emplace(id, p);
                    }
                    mine.push_back(id);
                    if (rng() % 4 == 0) {
                        if (s->cancel(mine[rng() % mine.size()])) canceled_ok.fetch_add(1);
                    }
                    if (rng() % 16 == 0) std::this_thread::sleep_for(100us);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        const auto deadline = Clock::now() + 30s;
        while (done.load() < static_cast<uint64_t>(kThreads * kPerThread) &&
               Clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const auto st = s->stats();
        SW_CHECK(st.submitted == static_cast<uint64_t>(kThreads * kPerThread));
        SW_CHECK(st.completed + st.failed + st.canceled + st.expired == st.submitted);
        SW_CHECK(st.canceled >= canceled_ok.load());
        SW_CHECK(st.in_flight == 0);
        SW_CHECK(st.queued_demand == 0 && st.queued_prefetch == 0);
    }
    std::lock_guard lock(mutex);
    SW_CHECK(accepted.size() == static_cast<std::size_t>(kThreads * kPerThread));
    SW_CHECK(callbacks.size() == accepted.size());
    for (const auto& [id, n] : callbacks)
        SW_CHECK(n == 1);
    SW_CHECK(bad.empty());
    SW_CHECK(ok_count.load() > 0);
    SW_CHECK(canceled_ok.load() > 0);
    std::printf("estresse (%d threads, %d pedidos, %llu cancelados, %llu ok): ok\n", kThreads,
                kThreads * kPerThread, static_cast<unsigned long long>(canceled_ok.load()),
                static_cast<unsigned long long>(ok_count.load()));
}

} // namespace

int main()
{
    test_config_validation();
    test_demand_passes_prefetch();
    test_reserved_slot();
    test_promotion_wakes_reserved_slot();
    test_no_reservation_uses_all_slots();
    test_deadline_order_within_kind();
    test_coalescing();
    test_shorter_deadline_moves_piece_ahead();
    test_prefetch_upgraded_by_demand();
    test_demand_joins_in_flight();
    test_cancel_queued();
    test_cancel_one_of_two();
    test_cancel_in_flight();
    test_cancel_in_flight_slow_source();
    test_cancel_while_requesting();
    test_cancel_unknown_or_done();
    test_expired_prefetch_dropped();
    test_callback_thread_and_reentrancy();
    test_source_error_propagates();
    test_source_misbehaves();
    test_callback_exception_counted();
    test_empty_callback();
    test_destructor_delivers_everything();
    test_destructor_drains_behind_slow_callback();
    test_pending();
    test_stress();
    std::puts("agendador por prazo: todos os testes passaram");
    return 0;
}
