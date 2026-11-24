// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Registro de pedidos, ids do processo, threads de conclusão e leases por arquivo (F1-67 parte B;
// F1-60 §1.5-§1.7). Ver request_registry.hpp.

#include "request_registry.hpp"

#include "request_registry_testing.hpp"

#include <new>
#include <system_error>

namespace swarmstream::detail {

namespace {

// O ÚLTIMO id emitido no processo (0 = nenhum). O próximo é last + 1; UINT64_MAX nunca é emitido.
std::atomic<std::uint64_t> g_last_id{0};
std::atomic<bool> g_exhaustion_reported{false};
// Gancho de teste: a n-ésima thread do próximo create() falha (0 = desligado).
std::atomic<std::uint32_t> g_fail_thread_start{0};

thread_local bool t_in_callback = false;

constexpr std::uint64_t kLastValid = UINT64_MAX - 1;

} // namespace

sw_request_id issue_request_id(bool* first) noexcept
{
    std::uint64_t v = g_last_id.load(std::memory_order_relaxed);
    for (;;) {
        if (v >= kLastValid) {
            // Esgotou: o contador NÃO avança mais (nunca 0, nunca repete, nunca dá a volta).
            const bool was = g_exhaustion_reported.exchange(true, std::memory_order_relaxed);
            if (first != nullptr) *first = !was;
            return 0;
        }
        if (g_last_id.compare_exchange_weak(v, v + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
            if (first != nullptr) *first = false;
            return v + 1;
        }
    }
}

bool in_callback() noexcept { return t_in_callback; }

const char* to_string(RequestStatus status) noexcept
{
    switch (status) {
    case RequestStatus::Ok: return "Ok";
    case RequestStatus::QueueFull: return "QueueFull";
    case RequestStatus::Cancelled: return "Cancelled";
    case RequestStatus::IdsExhausted: return "IdsExhausted";
    case RequestStatus::ShuttingDown: return "ShuttingDown";
    case RequestStatus::NotFound: return "NotFound";
    case RequestStatus::Busy: return "Busy";
    case RequestStatus::InvalidArgument: return "InvalidArgument";
    case RequestStatus::OutOfMemory: return "OutOfMemory";
    case RequestStatus::Internal: return "Internal";
    }
    return "RequestStatus?";
}

// ----------------------------------------------------------------------- RequestRegistry

RequestRegistry::RequestRegistry(Key, const ContextAllocator& alloc, Request* pool, Request** table,
                                 std::uint32_t capacity, const void* owner) noexcept
    : alloc_(alloc), pool_(pool), table_(table), capacity_(capacity), table_size_(2 * capacity),
      owner_(owner)
{
    for (std::uint32_t i = 0; i + 1 < capacity; ++i)
        pool_[i].next = &pool_[i + 1];
    free_ = capacity > 0 ? &pool_[0] : nullptr;
}

RequestRegistryCreate RequestRegistry::create(const ContextAllocator& alloc,
                                              const RequestRegistryConfig& config) noexcept
{
    const std::uint32_t fail_nth = g_fail_thread_start.exchange(0);
    RequestRegistryCreate out{};
    if (config.capacity == 0 || config.capacity > UINT32_MAX / 2 ||
        config.completion_threads == 0 || config.completion_threads > kMaxCompletionThreads) {
        out.status = RequestStatus::InvalidArgument;
        return out;
    }
    const std::uint32_t n = config.capacity;
    AllocStatus st = AllocStatus::Ok;
    Request* pool = alloc.create_array<Request>(n, kRequestPoolAlign, &st);
    if (pool == nullptr) {
        out.status = RequestStatus::OutOfMemory;
        out.alloc = st;
        return out;
    }
    constexpr std::size_t kTableAlign = kObjectAlign<Request*>;
    Request** table = alloc.create_array<Request*>(std::size_t{2} * n, kTableAlign, &st);
    if (table == nullptr) {
        alloc.destroy_array(pool, n, kRequestPoolAlign);
        out.status = RequestStatus::OutOfMemory;
        out.alloc = st;
        return out;
    }
    RequestRegistry* self =
        alloc.create<RequestRegistry>(&st, Key{}, alloc, pool, table, n, config.owner);
    if (self == nullptr) {
        alloc.destroy_array(table, std::size_t{2} * n, kTableAlign);
        alloc.destroy_array(pool, n, kRequestPoolAlign);
        out.status = RequestStatus::OutOfMemory;
        out.alloc = st;
        return out;
    }

    // Threads de conclusão (estado e pilha do sistema, nível 2). `std::thread` construída por
    // padrão não aloca; a atribuição por movimento também não. bad_alloc => OutOfMemory; qualquer
    // outra (std::system_error: o sistema não criou a thread) => Internal.
    RequestStatus thread_status = RequestStatus::Ok;
    try {
        for (std::uint32_t i = 0; i < config.completion_threads; ++i) {
            if (fail_nth != 0 && i + 1 == fail_nth) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again),
                    "gancho de teste: thread de conclusao recusada");
            }
            self->threads_[i] = std::thread([self] { self->completion_loop(); });
            self->thread_count_ = i + 1;
        }
    } catch (const std::bad_alloc&) {
        thread_status = RequestStatus::OutOfMemory;
    } catch (...) {
        thread_status = RequestStatus::Internal;
    }
    if (thread_status != RequestStatus::Ok) {
        // Desfaz: as threads que subiram param e são juntadas; a memória volta ao alocador. Se um
        // join falhar (não deveria: nenhuma é a thread atual), o registro vaza em vez de terminar o
        // processo.
        if (destroy(self) != RequestStatus::Ok) {
            out.status = RequestStatus::Internal;
            return out;
        }
        out.status = thread_status;
        out.alloc = thread_status == RequestStatus::OutOfMemory ? AllocStatus::OutOfMemory
                                                                : AllocStatus::Ok;
        return out;
    }
    out.registry = self;
    return out;
}

RequestStatus RequestRegistry::destroy(RequestRegistry* registry) noexcept
{
    if (registry == nullptr) return RequestStatus::Ok;
    if (in_callback()) return RequestStatus::Busy;
    if (registry->stop() != RequestStatus::Ok) return RequestStatus::Internal;
    const ContextAllocator alloc = registry->alloc_;
    Request* pool = registry->pool_;
    Request** table = registry->table_;
    const std::uint32_t n = registry->capacity_;
    alloc.destroy(registry);
    alloc.destroy_array(table, std::size_t{2} * n, kObjectAlign<Request*>);
    alloc.destroy_array(pool, n, kRequestPoolAlign);
    return RequestStatus::Ok;
}

std::size_t RequestRegistry::slot_of(sw_request_id id) const noexcept
{
    std::uint64_t x = id * 0x9E37'79B9'7F4A'7C15ull;
    x ^= x >> 32;
    return static_cast<std::size_t>(x % table_size_);
}

Request* RequestRegistry::find_locked(sw_request_id id) const noexcept
{
    if (id == 0) return nullptr;
    std::size_t i = slot_of(id);
    // A carga é no máximo 1/2 (capacity pedidos em 2 x capacity posições): sempre há um vazio.
    for (std::uint32_t probes = 0; probes < table_size_; ++probes) {
        Request* r = table_[i];
        if (r == nullptr) return nullptr;
        if (r->id == id) return r; // compara o id INTEIRO
        i = (i + 1) % table_size_;
    }
    return nullptr;
}

void RequestRegistry::table_insert_locked(Request* r) noexcept
{
    std::size_t i = slot_of(r->id);
    while (table_[i] != nullptr)
        i = (i + 1) % table_size_;
    table_[i] = r;
}

void RequestRegistry::table_erase_locked(sw_request_id id) noexcept
{
    std::size_t i = slot_of(id);
    while (table_[i] != nullptr && table_[i]->id != id)
        i = (i + 1) % table_size_;
    if (table_[i] == nullptr) return;
    // Remoção por deslocamento para trás (sondagem linear sem lápides).
    table_[i] = nullptr;
    std::size_t j = i;
    for (;;) {
        j = (j + 1) % table_size_;
        Request* r = table_[j];
        if (r == nullptr) return;
        const std::size_t k = slot_of(r->id);
        // r fica onde está se a posição ideal k está ciclicamente em (i, j].
        const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (stays) continue;
        table_[i] = r;
        table_[j] = nullptr;
        i = j;
    }
}

void RequestRegistry::to_completing_locked(Request* r, sw_result result,
                                           std::uint64_t bytes) noexcept
{
    // Sai da tabela de ids: cancel/complete/mark_in_flight posteriores não o acham (NotFound), o
    // que descarta a conclusão tardia de uma fonte "presa" sem esperá-la.
    table_erase_locked(r->id);
    r->result = result;
    r->bytes = bytes;
    r->state.store(RequestState::Completing, std::memory_order_relaxed);
    r->next = nullptr;
    if (queue_tail_ != nullptr) {
        queue_tail_->next = r;
    } else {
        queue_head_ = r;
    }
    queue_tail_ = r;
}

SubmitResult RequestRegistry::submit(const SubmitArgs& args, sw_request_id* out_id) noexcept
{
    if (out_id != nullptr) *out_id = 0;
    SubmitResult out{};
    if (args.file == nullptr || args.cb == nullptr) {
        out.status = RequestStatus::InvalidArgument;
        return out;
    }
    std::lock_guard<std::mutex> g(m_);
    if (stopping_) {
        out.status = RequestStatus::ShuttingDown;
        return out;
    }
    if (args.file->closing) {
        out.status = RequestStatus::Cancelled; // na hora, sem callback
        return out;
    }
    if (free_ == nullptr) {
        out.status = RequestStatus::QueueFull; // sem alocar
        return out;
    }
    bool first = false;
    const sw_request_id id = issue_request_id(&first);
    if (id == 0) {
        out.status = RequestStatus::IdsExhausted;
        out.first_exhaustion = first;
        return out;
    }
    Request* r = free_;
    free_ = r->next;
    r->next = nullptr;
    r->kind = args.kind;
    r->id = id;
    r->owner = owner_;
    r->file = args.file;
    r->dst = args.dst;
    r->offset = args.offset;
    r->size = args.size;
    r->deadline_ms = args.deadline_ms;
    r->cb = args.cb;
    r->user = args.user;
    r->result = SW_OK;
    r->bytes = 0;
    r->state.store(RequestState::Queued, std::memory_order_relaxed);
    FileRequests& f = *args.file;
    r->file_prev = nullptr;
    r->file_next = f.head;
    if (f.head != nullptr) f.head->file_prev = r;
    f.head = r;
    ++f.pending;
    ++pending_;
    table_insert_locked(r);
    // O id é escrito ANTES de o pedido poder concluir (a conclusão precisa deste mutex).
    if (out_id != nullptr) *out_id = id;
    return out;
}

RequestStatus RequestRegistry::mark_in_flight(sw_request_id id) noexcept
{
    std::lock_guard<std::mutex> g(m_);
    Request* r = find_locked(id);
    if (r == nullptr || r->state.load(std::memory_order_relaxed) != RequestState::Queued) {
        return RequestStatus::NotFound;
    }
    r->state.store(RequestState::InFlight, std::memory_order_relaxed);
    return RequestStatus::Ok;
}

RequestStatus RequestRegistry::complete(sw_request_id id, sw_result result,
                                        std::uint64_t bytes) noexcept
{
    {
        std::lock_guard<std::mutex> g(m_);
        Request* r = find_locked(id);
        if (r == nullptr) return RequestStatus::NotFound;
        to_completing_locked(r, result, bytes);
    }
    work_cv_.notify_one();
    return RequestStatus::Ok;
}

RequestStatus RequestRegistry::cancel(const void* owner, sw_request_id id) noexcept
{
    if (id == 0) return RequestStatus::NotFound;
    {
        std::lock_guard<std::mutex> g(m_);
        Request* r = find_locked(id);
        // Duas barreiras: o id inteiro (acima) e o dono.
        if (r == nullptr || r->owner != owner) return RequestStatus::NotFound;
        to_completing_locked(r, SW_E_CANCELLED, 0);
    }
    work_cv_.notify_one();
    return RequestStatus::Ok;
}

RequestStatus RequestRegistry::close_file(FileRequests& file) noexcept
{
    if (in_callback()) return RequestStatus::Busy; // nada muda
    std::unique_lock<std::mutex> lk(m_);
    file.closing = true; // a partir daqui: pedido novo => Cancelled, sem callback
    bool queued = false;
    for (Request* r = file.head; r != nullptr; r = r->file_next) {
        const RequestState s = r->state.load(std::memory_order_relaxed);
        if (s == RequestState::Queued || s == RequestState::InFlight) {
            to_completing_locked(r, SW_E_CANCELLED, 0); // sem esperar a fonte
            queued = true;
        }
    }
    if (queued) work_cv_.notify_all();
    // Espera os callbacks (cancelados e os que já concluíam). O mutex é solto durante a espera.
    files_cv_.wait(lk, [&file] { return file.pending == 0; });
    return RequestStatus::Ok;
}

std::uint32_t RequestRegistry::pending() const noexcept
{
    std::lock_guard<std::mutex> g(m_);
    return pending_;
}

RequestStatus RequestRegistry::stop() noexcept
{
    if (in_callback()) return RequestStatus::Busy;
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!stopped_) {
            stopping_ = true; // pedidos novos => ShuttingDown
            // Pendentes que ninguém cancelou (só no desfazer ou em uso incorreto): cada pedido
            // aceito tem exatamente um callback, então eles recebem SW_E_CANCELLED.
            bool queued = false;
            for (std::uint32_t i = 0; i < capacity_; ++i) {
                Request* r = &pool_[i];
                const RequestState s = r->state.load(std::memory_order_relaxed);
                if (s == RequestState::Queued || s == RequestState::InFlight) {
                    to_completing_locked(r, SW_E_CANCELLED, 0);
                    queued = true;
                }
            }
            if (queued) work_cv_.notify_all();
            files_cv_.wait(lk, [this] { return pending_ == 0; });
            stopped_ = true;
        }
    }
    work_cv_.notify_all();
    // Junta as threads (fora do mutex). Uma que não junta fica contada: destroy recusa liberar.
    RequestStatus st = RequestStatus::Ok;
    std::uint32_t remaining = 0;
    for (std::uint32_t i = 0; i < thread_count_; ++i) {
        if (!threads_[i].joinable()) continue;
        try {
            threads_[i].join();
        } catch (...) {
            st = RequestStatus::Internal;
            ++remaining;
        }
    }
    if (remaining == 0) thread_count_ = 0;
    return st;
}

void RequestRegistry::completion_loop() noexcept
{
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        work_cv_.wait(lk, [this] { return queue_head_ != nullptr || stopped_; });
        if (queue_head_ == nullptr) return; // parado e sem trabalho
        Request* r = queue_head_;
        queue_head_ = r->next;
        if (queue_head_ == nullptr) queue_tail_ = nullptr;
        r->next = nullptr;
        const sw_read_cb cb = r->cb;
        void* const user = r->user;
        const sw_request_id id = r->id;
        const sw_result result = r->result;
        const std::uint64_t bytes = r->bytes;
        lk.unlock();
        t_in_callback = true;
        cb(user, id, result, bytes); // exatamente uma vez por pedido aceito
        t_in_callback = false;
        lk.lock();
        // Libera: sai da lista do arquivo e volta ao pool.
        FileRequests& f = *r->file;
        if (r->file_prev != nullptr) {
            r->file_prev->file_next = r->file_next;
        } else {
            f.head = r->file_next;
        }
        if (r->file_next != nullptr) r->file_next->file_prev = r->file_prev;
        --f.pending;
        --pending_;
        const bool wake = (f.closing && f.pending == 0) || pending_ == 0;
        r->state.store(RequestState::Free, std::memory_order_relaxed);
        r->kind = RequestKind::Synthetic;
        r->id = 0;
        r->owner = nullptr;
        r->file = nullptr;
        r->dst = nullptr;
        r->offset = 0;
        r->size = 0;
        r->deadline_ms = 0;
        r->cb = nullptr;
        r->user = nullptr;
        r->result = SW_OK;
        r->bytes = 0;
        r->file_prev = nullptr;
        r->file_next = nullptr;
        r->next = free_;
        free_ = r;
        if (wake) files_cv_.notify_all();
    }
}

// ----------------------------------------------------------------------------- FileLeases

AllocStatus FileLeases::add(const ContextAllocator& alloc, Lease&& lease) noexcept
{
    AllocStatus st = AllocStatus::Ok;
    // create aloca ANTES de construir: se falhar, o lease não foi movido e fica com o chamador.
    Node* n = alloc.create<Node>(&st, std::move(lease));
    if (n == nullptr) return st;
    n->next = head_;
    head_ = n;
    ++count_;
    return AllocStatus::Ok;
}

void FileLeases::release_all(const ContextAllocator& alloc) noexcept
{
    Node* n = head_;
    head_ = nullptr;
    count_ = 0;
    while (n != nullptr) {
        Node* next = n->next;
        alloc.destroy(n); // o destrutor do Lease solta a peça
        n = next;
    }
}

namespace testing {

void set_last_issued_request_id(std::uint64_t last) noexcept
{
    g_last_id.store(last);
    g_exhaustion_reported.store(false);
}

void RequestRegistryTestHook::fail_thread_start(std::uint32_t nth) noexcept
{
    g_fail_thread_start.store(nth);
}

sw_request_id RequestRegistryTestHook::complete_first_queued(RequestRegistry& r,
                                                             sw_result result) noexcept
{
    sw_request_id id = 0;
    {
        std::lock_guard<std::mutex> g(r.m_);
        for (std::uint32_t i = 0; i < r.capacity_; ++i) {
            Request* q = &r.pool_[i];
            if (q->state.load(std::memory_order_relaxed) == RequestState::Queued) {
                id = q->id;
                r.to_completing_locked(q, result, 0);
                break;
            }
        }
    }
    if (id != 0) r.work_cv_.notify_one();
    return id;
}

} // namespace testing

} // namespace swarmstream::detail
