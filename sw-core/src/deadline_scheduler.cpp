// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Agendador por prazo v1 (F1-28). Regras no cabeçalho. Por dentro:
// - Uma trava (mutex_) protege tudo: filas, mapas, contadores e a fila de entrega. Nenhuma
//   chamada à fonte nem callback acontece com ela presa.
// - Uma "entrada" por peça pedida: os pedidos (subscribers) dela, o prazo e o tipo efetivos e o
//   estado (na fila ou na fonte). by_piece_ acha a entrada viva da peça; by_request_ acha a
//   entrada de cada pedido. Uma entrada na fonte sem ninguém esperando sai de by_piece_
//   ("esquecida"): um pedido novo da peça cria outra entrada e não herda o cancelamento.
// - Duas filas ordenadas por (prazo, ordem de chegada): demanda e prefetch. A thread de trabalho
//   livre tira a demanda mais cedo; se não houver, o prefetch mais cedo, desde que haja vaga de
//   prefetch (max_in_flight - reserved_for_demand).
// - Cancelamento na fonte: quem cancela lê o id da fonte sob a trava. Se a thread de trabalho
//   ainda está dentro de request() (id inválido), marca cancel_source, e a thread cancela assim
//   que guarda o id (na mesma seção crítica em que lê a marca). Exatamente um dos dois cancela.
// - Falta de memória: as funções *_locked que mexem em mais de uma estrutura são noexcept. Um
//   bad_alloc no meio delas termina o processo (std::terminate) em vez de deixar filas e mapas
//   desencontrados. As threads internas também terminam o processo se algo escapar delas.

#include "swarmstream/deadline_scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swarmstream {

namespace {

constexpr unsigned kMaxInFlight = 64;

std::shared_ptr<const PieceResponse> make_error(const PieceId& piece, SourceErrorCode code,
                                                std::string message)
{
    return std::make_shared<const PieceResponse>(
        PieceResponse{piece, {}, code, std::move(message)});
}

} // namespace

class DeadlineScheduler::Impl
{
public:
    Impl(IContentSource& source, SchedulerConfig config)
        : source_(source), max_in_flight_(config.max_in_flight),
          prefetch_limit_(config.max_in_flight - config.reserved_for_demand)
    {
    }

    ~Impl() { stop(); }

    void start()
    {
        delivery_ = std::thread([this] { delivery_loop(); });
        workers_.reserve(max_in_flight_);
        for (unsigned i = 0; i < max_in_flight_; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    RequestId submit(const PieceId& piece, SteadyTimePoint deadline, RequestKind kind,
                     SchedulerCallback callback)
    {
        RequestId id = kInvalidRequestId;
        bool wake = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return kInvalidRequestId;
            id = next_request_id_++;
            wake = add_locked(id, piece, deadline, kind, std::move(callback));
        }
        if (wake) work_cv_.notify_one();
        return id;
    }

    bool cancel(RequestId id)
    {
        RequestId source_id = kInvalidRequestId;
        {
            std::lock_guard lock(mutex_);
            if (!cancel_locked(id, source_id)) return false;
        }
        delivery_cv_.notify_one();
        if (source_id != kInvalidRequestId) (void)source_.cancel(source_id);
        return true;
    }

    bool pending(const PieceId& piece) const
    {
        std::lock_guard lock(mutex_);
        return by_piece_.find(piece) != by_piece_.end();
    }

    SchedulerStats stats() const
    {
        std::lock_guard lock(mutex_);
        SchedulerStats out = stats_;
        out.queued_demand = demand_.size();
        out.queued_prefetch = prefetch_.size();
        out.in_flight = in_flight_;
        return out;
    }

private:
    struct Subscriber
    {
        RequestId id;
        RequestKind kind;
        SteadyTimePoint deadline;
        SchedulerCallback callback;
    };

    enum class State : uint8_t { Queued, InFlight };

    struct Entry
    {
        PieceId piece{};
        std::vector<Subscriber> subscribers;
        SteadyTimePoint deadline{}; // menor prazo entre os pedidos vivos (enquanto na fila)
        RequestKind kind = RequestKind::Prefetch; // Demand se algum pedido vivo é demanda
        uint64_t seq = 0;                         // ordem de chegada (desempate)
        State state = State::Queued;
        RequestKind sent_as = RequestKind::Prefetch; // tipo quando saiu para a fonte
        bool forgotten = false;                      // fora de by_piece_
        bool cancel_source = false; // ninguém espera a resposta: cancelar na fonte
        RequestId source_id = kInvalidRequestId;
    };

    using EntryPtr = std::shared_ptr<Entry>;
    using QueueKey = std::pair<SteadyTimePoint, uint64_t>;
    using Queue = std::map<QueueKey, EntryPtr>;

    struct Delivery
    {
        RequestId id;
        SchedulerCallback callback;
        std::shared_ptr<const PieceResponse> response;
    };

    Queue& queue_for(RequestKind kind) { return kind == RequestKind::Demand ? demand_ : prefetch_; }

    void enqueue_locked(const EntryPtr& entry) noexcept
    {
        queue_for(entry->kind).emplace(QueueKey{entry->deadline, entry->seq}, entry);
    }

    void dequeue_locked(const Entry& entry) noexcept
    {
        const std::size_t erased = queue_for(entry.kind).erase(QueueKey{entry.deadline, entry.seq});
        if (erased != 1) std::terminate(); // entrada na fila tem de estar na fila do seu tipo
    }

    // Prazo e tipo efetivos a partir dos pedidos vivos (depois de um cancelamento).
    static void recompute(Entry& entry)
    {
        entry.kind = RequestKind::Prefetch;
        entry.deadline = SteadyTimePoint::max();
        for (const auto& sub : entry.subscribers) {
            if (sub.kind == RequestKind::Demand) entry.kind = RequestKind::Demand;
            entry.deadline = std::min(entry.deadline, sub.deadline);
        }
    }

    // Acrescenta o pedido `id`. Devolve true se uma thread de trabalho deve acordar.
    bool add_locked(RequestId id, const PieceId& piece, SteadyTimePoint deadline, RequestKind kind,
                    SchedulerCallback&& callback) noexcept
    {
        ++stats_.submitted;
        const auto found = by_piece_.find(piece);
        if (found != by_piece_.end()) {
            const EntryPtr entry = found->second;
            ++stats_.coalesced;
            entry->subscribers.push_back(Subscriber{id, kind, deadline, std::move(callback)});
            by_request_.emplace(id, entry);
            if (entry->state != State::Queued) return false; // já na fonte: só espera a resposta
            const bool upgrade =
                kind == RequestKind::Demand && entry->kind == RequestKind::Prefetch;
            if (!upgrade && deadline >= entry->deadline) return false;
            dequeue_locked(*entry);
            if (upgrade) entry->kind = RequestKind::Demand;
            entry->deadline = std::min(entry->deadline, deadline);
            enqueue_locked(entry);
            return upgrade; // a demanda pode usar a vaga reservada (teste da lacuna A15)
        }
        auto entry = std::make_shared<Entry>();
        entry->piece = piece;
        entry->deadline = deadline;
        entry->kind = kind;
        entry->seq = next_seq_++;
        entry->subscribers.push_back(Subscriber{id, kind, deadline, std::move(callback)});
        enqueue_locked(entry);
        by_piece_.emplace(piece, entry);
        by_request_.emplace(id, std::move(entry));
        return true;
    }

    // Tira o pedido `id` (se pendente) e agenda o callback Canceled. `source_id` recebe o id na
    // fonte a cancelar, se for o caso.
    bool cancel_locked(RequestId id, RequestId& source_id) noexcept
    {
        const auto found = by_request_.find(id);
        if (found == by_request_.end()) return false;
        const EntryPtr entry = found->second;
        by_request_.erase(found);
        auto& subs = entry->subscribers;
        const auto sub = std::find_if(subs.begin(), subs.end(),
                                      [id](const Subscriber& s) { return s.id == id; });
        // by_request_ e subscribers andam juntos: um pedido em um está no outro.
        if (sub == subs.end()) std::terminate();
        SchedulerCallback callback = std::move(sub->callback);
        subs.erase(sub);
        ++stats_.canceled;
        if (callback) {
            deliveries_.push_back(
                Delivery{id, std::move(callback),
                         make_error(entry->piece, SourceErrorCode::Canceled, "cancelado")});
        }
        if (entry->state == State::Queued) {
            dequeue_locked(*entry);
            if (subs.empty()) {
                forget_locked(*entry);
            } else {
                recompute(*entry);
                enqueue_locked(entry);
            }
        } else if (subs.empty()) {
            forget_locked(*entry);
            entry->cancel_source = true;
            source_id = entry->source_id; // inválido: a thread de trabalho cancela
        }
        return true;
    }

    // Tira a entrada de by_piece_: um pedido novo da peça cria outra entrada.
    void forget_locked(Entry& entry) noexcept
    {
        if (entry.forgotten) return;
        const auto found = by_piece_.find(entry.piece);
        if (found == by_piece_.end() || found->second.get() != &entry) std::terminate();
        by_piece_.erase(found);
        entry.forgotten = true;
    }

    // Entrega `response` a todos os pedidos da entrada e a esquece.
    void finish_locked(Entry& entry, const std::shared_ptr<const PieceResponse>& response,
                       bool expired) noexcept
    {
        for (auto& sub : entry.subscribers) {
            by_request_.erase(sub.id);
            if (expired) {
                ++stats_.expired;
            } else if (response->error == SourceErrorCode::Success) {
                ++stats_.completed;
            } else if (response->error == SourceErrorCode::Canceled) {
                ++stats_.canceled;
            } else {
                ++stats_.failed;
            }
            if (sub.callback)
                deliveries_.push_back(Delivery{sub.id, std::move(sub.callback), response});
        }
        entry.subscribers.clear();
        forget_locked(entry);
        delivery_cv_.notify_one();
    }

    [[nodiscard]] bool can_dispatch_locked() const
    {
        return !demand_.empty() || (!prefetch_.empty() && prefetch_in_flight_ < prefetch_limit_);
    }

    // A próxima entrada a sair, ou nada (só havia prefetch vencido, que é descartado aqui).
    EntryPtr pop_locked() noexcept
    {
        if (!demand_.empty()) {
            const auto first = demand_.begin();
            EntryPtr entry = std::move(first->second);
            demand_.erase(first);
            return entry;
        }
        const auto now = std::chrono::steady_clock::now();
        while (!prefetch_.empty() && prefetch_in_flight_ < prefetch_limit_) {
            const auto first = prefetch_.begin();
            EntryPtr entry = std::move(first->second);
            prefetch_.erase(first);
            if (entry->deadline >= now) return entry;
            finish_locked(*entry,
                          make_error(entry->piece, SourceErrorCode::Timeout,
                                     "prazo da dica passou antes de sair para a fonte"),
                          true);
        }
        return nullptr;
    }

    void worker_loop()
    {
        std::unique_lock lock(mutex_);
        for (;;) {
            work_cv_.wait(lock, [this] { return stopping_ || can_dispatch_locked(); });
            if (stopping_) return;
            const EntryPtr entry = pop_locked();
            if (!entry) continue;
            if (can_dispatch_locked()) work_cv_.notify_one(); // há mais trabalho para outra thread
            fetch(lock, *entry);
        }
    }

    // Leva a entrada à fonte e entrega a resposta. Entra e sai com a trava presa; solta durante
    // as chamadas à fonte.
    void fetch(std::unique_lock<std::mutex>& lock, Entry& entry)
    {
        entry.state = State::InFlight;
        entry.sent_as = entry.kind;
        ++in_flight_;
        if (entry.sent_as == RequestKind::Prefetch) {
            ++prefetch_in_flight_;
            ++stats_.sent_prefetch;
        } else {
            ++stats_.sent_demand;
        }
        const PieceId piece = entry.piece;
        const SteadyTimePoint deadline = entry.deadline;
        lock.unlock();

        std::shared_ptr<const PieceResponse> response;
        RequestTicket ticket;
        try {
            ticket = source_.request(piece, deadline);
        } catch (const std::exception& e) {
            response =
                make_error(piece, SourceErrorCode::NetworkError,
                           std::string("IContentSource::request lancou excecao: ") + e.what());
        } catch (...) {
            response = make_error(piece, SourceErrorCode::NetworkError,
                                  "IContentSource::request lancou excecao");
        }
        if (!response) {
            lock.lock();
            entry.source_id = ticket.request_id;
            const bool cancel_now = entry.cancel_source;
            lock.unlock();
            if (cancel_now && ticket.request_id != kInvalidRequestId) {
                (void)source_.cancel(ticket.request_id);
            }
            if (!ticket.future.valid()) {
                response = make_error(piece, SourceErrorCode::NetworkError,
                                      "IContentSource::request devolveu ticket sem future");
            } else {
                try {
                    response = std::make_shared<const PieceResponse>(ticket.future.get());
                } catch (const std::exception& e) {
                    response =
                        make_error(piece, SourceErrorCode::NetworkError,
                                   std::string("a fonte nao respondeu o pedido: ") + e.what());
                } catch (...) {
                    response = make_error(piece, SourceErrorCode::NetworkError,
                                          "a fonte respondeu com excecao desconhecida");
                }
            }
        }

        lock.lock();
        --in_flight_;
        if (entry.sent_as == RequestKind::Prefetch) --prefetch_in_flight_;
        finish_locked(entry, response, false);
    }

    void delivery_loop()
    {
        std::unique_lock lock(mutex_);
        for (;;) {
            delivery_cv_.wait(lock, [this] { return delivery_stop_ || !deliveries_.empty(); });
            if (deliveries_.empty()) return; // parando e nada mais a entregar
            std::deque<Delivery> batch;
            batch.swap(deliveries_);
            lock.unlock();
            uint64_t exceptions = 0;
            for (auto& d : batch) {
                try {
                    d.callback(d.id, *d.response);
                } catch (...) {
                    ++exceptions;
                }
            }
            batch.clear(); // destrói callbacks e respostas sem a trava
            lock.lock();
            stats_.callback_exceptions += exceptions;
        }
    }

    void stop()
    {
        std::vector<RequestId> source_cancels;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            for (Queue* queue : {&demand_, &prefetch_}) {
                for (auto& node : *queue) {
                    finish_locked(*node.second,
                                  make_error(node.second->piece, SourceErrorCode::Canceled,
                                             "agendador encerrado"),
                                  false);
                }
                queue->clear();
            }
            // O que sobrou em by_piece_ está na fonte: os pedidos recebem o que ela responder.
            for (auto& node : by_piece_) {
                Entry& entry = *node.second;
                entry.cancel_source = true;
                if (entry.source_id != kInvalidRequestId) source_cancels.push_back(entry.source_id);
            }
        }
        work_cv_.notify_all();
        for (const RequestId id : source_cancels)
            (void)source_.cancel(id);
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        {
            std::lock_guard lock(mutex_);
            delivery_stop_ = true;
        }
        delivery_cv_.notify_all();
        if (delivery_.joinable()) delivery_.join();
    }

    IContentSource& source_;
    const unsigned max_in_flight_;
    const unsigned prefetch_limit_;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable delivery_cv_;
    Queue demand_;
    Queue prefetch_;
    std::unordered_map<PieceId, EntryPtr> by_piece_;
    std::unordered_map<RequestId, EntryPtr> by_request_;
    std::deque<Delivery> deliveries_;
    RequestId next_request_id_ = 1;
    uint64_t next_seq_ = 0;
    unsigned in_flight_ = 0;
    unsigned prefetch_in_flight_ = 0;
    SchedulerStats stats_{};
    bool stopping_ = false;
    bool delivery_stop_ = false;

    std::vector<std::thread> workers_;
    std::thread delivery_;
};

Result<std::unique_ptr<DeadlineScheduler>, std::string>
DeadlineScheduler::create(IContentSource& source, SchedulerConfig config)
{
    if (config.max_in_flight == 0 || config.max_in_flight > kMaxInFlight) {
        return Unexpected(std::string("max_in_flight deve estar entre 1 e 64"));
    }
    if (config.reserved_for_demand >= config.max_in_flight) {
        return Unexpected(std::string(
            "reserved_for_demand deve ser menor que max_in_flight (prefetch precisa de uma vaga)"));
    }
    auto impl = std::make_unique<Impl>(source, config);
    try {
        impl->start();
    } catch (const std::exception& e) {
        return Unexpected(std::string("nao consegui criar as threads do agendador: ") + e.what());
    }
    return std::unique_ptr<DeadlineScheduler>(new DeadlineScheduler(std::move(impl)));
}

DeadlineScheduler::DeadlineScheduler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DeadlineScheduler::~DeadlineScheduler() = default;

RequestId DeadlineScheduler::submit(const PieceId& piece, SteadyTimePoint deadline,
                                    RequestKind kind, SchedulerCallback callback)
{
    return impl_->submit(piece, deadline, kind, std::move(callback));
}

bool DeadlineScheduler::cancel(RequestId id) { return impl_->cancel(id); }

bool DeadlineScheduler::pending(const PieceId& piece) const { return impl_->pending(piece); }

SchedulerStats DeadlineScheduler::stats() const { return impl_->stats(); }

} // namespace swarmstream
