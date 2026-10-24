// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/cache_owner.hpp"

#include "cache_owner_testing.hpp"
#include "swarmstream/slot_allocator.hpp"

#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace swarmstream {

// ---------------------------------------------------------------- CacheRequest

CacheRequest::CacheRequest(CacheOp op, const PieceId& piece, std::span<const std::byte> input,
                           std::span<std::byte> output) noexcept
    : op_(op), piece_(piece), input_(input), output_(output)
{
}

CacheRequest::~CacheRequest()
{
    // Pendente: espera a dona responder, para ela nunca escrever num pedido destruído.
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !pending_; });
}

bool CacheRequest::arm() noexcept
{
    std::lock_guard lock(mutex_);
    if (pending_) return false;
    pending_ = true;
    done_ = false;
    code_ = CacheErrorCode::IoError;
    bytes_ = 0;
    return true;
}

void CacheRequest::complete(CacheErrorCode code, size_t bytes) noexcept
{
    // Tudo dentro da trava: quem pede só vê done_/pending_ com a trava, então só pode destruir
    // o pedido depois que esta função soltou a trava, e ela não toca em mais nada depois.
    std::lock_guard lock(mutex_);
    code_ = code;
    bytes_ = bytes;
    done_ = true;
    pending_ = false;
    cv_.notify_all();
}

bool CacheRequest::done() const noexcept
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    return lock.owns_lock() && done_;
}

Result<size_t, CacheErrorCode> CacheRequest::wait() const
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !pending_; });
    if (!done_) return Unexpected(CacheErrorCode::IoError); // nunca foi enviado
    if (code_ != CacheErrorCode::Success) return Unexpected(code_);
    return bytes_;
}

Result<size_t, CacheErrorCode> CacheRequest::result() const
{
    std::lock_guard lock(mutex_);
    if (!done_) return Unexpected(CacheErrorCode::IoError);
    if (code_ != CacheErrorCode::Success) return Unexpected(code_);
    return bytes_;
}

// ---------------------------------------------------------------- CacheOwner

struct CacheOwner::Impl
{
    struct Entry
    {
        SlotId slot;
        uint32_t size;
    };

    Impl(StoreFiles s, uint32_t total_slots) : store(std::move(s)), slots(total_slots) {}

    // Só a thread dona toca nestes três (depois de start() e antes do join em stop()).
    StoreFiles store;
    SlotAllocator slots;
    std::unordered_map<PieceId, Entry> index; // provisório até a F1-11

    MpscQueue queue;

    // Despertar da dona. O par `sleeping`/fila segue o protocolo de Dekker: quem pede faz
    // push (escrita seq_cst do `next`) e depois lê `sleeping` (seq_cst); a dona grava
    // `sleeping` (seq_cst) e depois lê a fila (empty(), leitura seq_cst do `next`). Pelo menos
    // um dos dois vê a escrita do outro: ou a dona vê o pedido e não dorme, ou quem pede vê a
    // dona dormindo e a acorda pegando `wake_mutex` (que a dona segura entre conferir a fila e
    // dormir, então o aviso não se perde).
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    std::atomic<bool> sleeping{false};

    // Parada: quem pede soma `inflight` antes de olhar `stopping` e subtrai depois do push.
    // A dona, depois de ver `stopping`, espera `inflight` zerar e esvazia a fila: nenhum pedido
    // fica sem resposta, nem os enviados durante a parada.
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> inflight{0};

    std::atomic<std::thread::id> owner_id{};
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> sleeps{0};
    std::atomic<uint32_t> pieces{0};

    std::thread thread;
    std::mutex stop_mutex;
    bool stopped = false; // guardado por stop_mutex

    // CacheOwnerTestHook: segura os submit() na janela entre ler `stopping` e o push, e marca
    // quando a dona saiu do laço principal. Fora dos testes, custa uma leitura relaxada.
    std::atomic<bool> hook_gate_enabled{false};
    std::mutex hook_mutex;
    std::condition_variable hook_cv;
    bool hook_gate_open = true;        // guardado por hook_mutex
    uint32_t hook_in_window = 0;       // guardado por hook_mutex
    bool hook_owner_left_loop = false; // guardado por hook_mutex

    void hold_in_window() noexcept;

    void run() noexcept;
    void process(CacheRequest& r) noexcept;
    void put(CacheRequest& r) noexcept;
    void get(CacheRequest& r) noexcept;
    void remove(CacheRequest& r) noexcept;
};

void CacheOwner::Impl::run() noexcept
{
    owner_id.store(std::this_thread::get_id());
    for (;;) {
        while (MpscNode* node = queue.pop()) {
            process(*CacheRequest::from_node(node));
            processed.fetch_add(1, std::memory_order_relaxed);
        }
        if (stopping.load(std::memory_order_seq_cst)) break;
        if (!queue.empty()) { // um push está no meio: o nó aparece em instantes
            std::this_thread::yield();
            continue;
        }
        std::unique_lock lock(wake_mutex);
        sleeping.store(true, std::memory_order_seq_cst);
        wake_cv.wait(lock,
                     [this] { return !queue.empty() || stopping.load(std::memory_order_seq_cst); });
        sleeping.store(false, std::memory_order_relaxed);
        sleeps.fetch_add(1, std::memory_order_relaxed);
    }
    if (hook_gate_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(hook_mutex);
        hook_owner_left_loop = true;
        hook_cv.notify_all();
    }
    // Parando: espera quem está no meio de um submit e recusa o que sobrou na fila.
    while (inflight.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    while (MpscNode* node = queue.pop()) {
        CacheRequest::from_node(node)->complete(CacheErrorCode::Stopped, 0);
    }
}

void CacheOwner::Impl::process(CacheRequest& r) noexcept
{
    switch (r.op_) {
    case CacheOp::Put: put(r); return;
    case CacheOp::Get: get(r); return;
    case CacheOp::Remove: remove(r); return;
    }
    r.complete(CacheErrorCode::IoError, 0); // op inválida (memória corrompida?)
}

void CacheOwner::Impl::put(CacheRequest& r) noexcept
{
    const size_t size = r.input_.size();
    if (size == 0 || size > store.layout().slot_size) {
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    if (const auto it = index.find(r.piece_); it != index.end()) {
        r.complete(CacheErrorCode::Success, it->second.size); // já está: não regrava
        return;
    }
    const auto slot = slots.allocate();
    if (!slot) {
        r.complete(slot.error(), 0);
        return;
    }
    // 04 §3: dados, depois a Barreira 1. O journal (F1-11) entra aqui, antes de publicar.
    CacheErrorCode code = CacheErrorCode::Success;
    if (auto w = store.write(*slot, 0, r.input_); !w) {
        code = w.error();
    } else if (auto f = store.flush(); !f) {
        code = f.error();
    } else {
        try {
            index.emplace(r.piece_, Entry{*slot, static_cast<uint32_t>(size)});
        } catch (...) { // sem memória para o nó do índice
            code = CacheErrorCode::IoError;
        }
    }
    if (code != CacheErrorCode::Success) {
        (void)slots.release(*slot); // o slot acabou de ser alocado: release não falha
        r.complete(code, 0);
        return;
    }
    pieces.fetch_add(1, std::memory_order_relaxed);
    r.complete(CacheErrorCode::Success, size);
}

void CacheOwner::Impl::get(CacheRequest& r) noexcept
{
    const auto it = index.find(r.piece_);
    if (it == index.end()) {
        r.complete(CacheErrorCode::PieceNotFound, 0);
        return;
    }
    const Entry e = it->second;
    if (r.output_.size() < e.size) {
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    if (auto rd = store.read(e.slot, 0, r.output_.first(e.size)); !rd) {
        r.complete(rd.error(), 0);
        return;
    }
    r.complete(CacheErrorCode::Success, e.size);
}

void CacheOwner::Impl::remove(CacheRequest& r) noexcept
{
    const auto it = index.find(r.piece_);
    if (it == index.end()) {
        r.complete(CacheErrorCode::PieceNotFound, 0);
        return;
    }
    // Sem leases ainda (F1-13): nenhuma leitura segura o slot fora da dona.
    if (auto rel = slots.release(it->second.slot); !rel) {
        r.complete(CacheErrorCode::IoError, 0); // índice e alocador discordam: não deveria ocorrer
        return;
    }
    index.erase(it);
    pieces.fetch_sub(1, std::memory_order_relaxed);
    r.complete(CacheErrorCode::Success, 0);
}

Result<std::unique_ptr<CacheOwner>, CacheErrorCode> CacheOwner::start(CacheOwnerConfig config)
{
    auto store = StoreFiles::open(config.dir, config.total_slots, config.layout);
    if (!store) return Unexpected(store.error());
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(std::move(*store), config.total_slots);
        impl->index.reserve(config.total_slots); // o índice não reorganiza no meio do jogo
    } catch (const std::bad_alloc&) {
        return Unexpected(CacheErrorCode::IoError);
    }
    try {
        Impl* p = impl.get();
        impl->thread = std::thread([p] { p->run(); });
    } catch (const std::system_error&) {
        return Unexpected(CacheErrorCode::IoError);
    }
    return std::unique_ptr<CacheOwner>(new CacheOwner(std::move(impl)));
}

CacheOwner::CacheOwner(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

CacheOwner::~CacheOwner() { (void)stop(); }

bool CacheOwner::submit(CacheRequest& request) noexcept
{
    if (!request.arm()) return false;
    Impl& s = *impl_;
    s.inflight.fetch_add(1, std::memory_order_seq_cst);
    if (s.stopping.load(std::memory_order_seq_cst)) {
        s.inflight.fetch_sub(1, std::memory_order_seq_cst);
        request.complete(CacheErrorCode::Stopped, 0);
        return true;
    }
    if (s.hook_gate_enabled.load(std::memory_order_relaxed)) s.hold_in_window();
    s.queue.push(&request); // depois daqui o pedido é da dona: não tocar mais nele
    if (s.sleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard lock(s.wake_mutex);
        s.wake_cv.notify_one();
    }
    s.inflight.fetch_sub(1, std::memory_order_seq_cst);
    return true;
}

Result<size_t, CacheErrorCode> CacheOwner::call(CacheRequest& request)
{
    if (!submit(request)) return Unexpected(CacheErrorCode::SlotBusy);
    return request.wait();
}

Result<void, CacheErrorCode> CacheOwner::stop() noexcept
{
    Impl& s = *impl_;
    std::lock_guard stop_lock(s.stop_mutex);
    if (s.stopped) return {};
    s.stopped = true;
    s.stopping.store(true, std::memory_order_seq_cst);
    {
        std::lock_guard lock(s.wake_mutex); // a dona está dormindo ou ainda vai conferir `stopping`
    }
    s.wake_cv.notify_one();
    if (s.thread.joinable()) {
        try {
            s.thread.join();
        } catch (const std::system_error&) {
            return Unexpected(CacheErrorCode::IoError); // chamado da própria dona
        }
    }
    // Depois do join, o store é desta thread.
    return s.store.close();
}

bool CacheOwner::is_owner_thread() const noexcept
{
    return impl_->owner_id.load() == std::this_thread::get_id();
}

uint64_t CacheOwner::requests_processed() const noexcept
{
    return impl_->processed.load(std::memory_order_relaxed);
}

uint64_t CacheOwner::sleeps() const noexcept
{
    return impl_->sleeps.load(std::memory_order_relaxed);
}

uint32_t CacheOwner::pieces() const noexcept
{
    return impl_->pieces.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------- ganchos de teste

void CacheOwner::Impl::hold_in_window() noexcept
{
    std::unique_lock lock(hook_mutex);
    ++hook_in_window;
    hook_cv.notify_all();
    hook_cv.wait(lock, [this] { return hook_gate_open; });
    --hook_in_window;
}

void CacheOwnerTestHook::close_submit_window(CacheOwner& owner) noexcept
{
    CacheOwner::Impl& s = *owner.impl_;
    {
        std::lock_guard lock(s.hook_mutex);
        s.hook_gate_open = false;
    }
    s.hook_gate_enabled.store(true);
}

void CacheOwnerTestHook::open_submit_window(CacheOwner& owner) noexcept
{
    CacheOwner::Impl& s = *owner.impl_;
    std::lock_guard lock(s.hook_mutex);
    s.hook_gate_open = true;
    s.hook_cv.notify_all();
}

bool CacheOwnerTestHook::wait_submitters_in_window(CacheOwner& owner, uint32_t count,
                                                   std::chrono::milliseconds timeout) noexcept
{
    CacheOwner::Impl& s = *owner.impl_;
    std::unique_lock lock(s.hook_mutex);
    return s.hook_cv.wait_for(lock, timeout, [&] { return s.hook_in_window == count; });
}

bool CacheOwnerTestHook::wait_owner_left_loop(CacheOwner& owner,
                                              std::chrono::milliseconds timeout) noexcept
{
    CacheOwner::Impl& s = *owner.impl_;
    std::unique_lock lock(s.hook_mutex);
    return s.hook_cv.wait_for(lock, timeout, [&] { return s.hook_owner_left_loop; });
}

} // namespace swarmstream
