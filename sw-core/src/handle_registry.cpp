// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Registro global de handles, pin/FECHANDO e quarentena (F1-67 parte B; F1-60 §1.3-§1.4). Ver
// handle_registry.hpp para o protocolo e as regras de ordem.

#include "handle_registry.hpp"

#include "handle_registry_testing.hpp"

#include <new>
#include <type_traits>

namespace swarmstream::detail {

namespace {
constexpr std::uintptr_t kEmpty = 0;
constexpr std::uintptr_t kTomb = 1;
constexpr std::size_t kMinCap = 16;

// Gancho de teste: chamado pelo unpin lento logo depois do decremento (nulo = desligado; só custa
// uma leitura, e só com o handle FECHANDO).
std::atomic<void (*)() noexcept> g_after_slow_decrement{nullptr};

std::size_t next_pow2(std::size_t v) noexcept
{
    std::size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}
} // namespace

const char* to_string(HandleKind kind) noexcept
{
    switch (kind) {
    case HandleKind::None: return "None";
    case HandleKind::Context: return "Context";
    case HandleKind::File: return "File";
    case HandleKind::Dir: return "Dir";
    }
    return "HandleKind?";
}

const char* to_string(HandleStatus status) noexcept
{
    switch (status) {
    case HandleStatus::Ok: return "Ok";
    case HandleStatus::Unknown: return "Unknown";
    case HandleStatus::WrongKind: return "WrongKind";
    case HandleStatus::Closing: return "Closing";
    case HandleStatus::NotClosing: return "NotClosing";
    case HandleStatus::Duplicate: return "Duplicate";
    case HandleStatus::OutOfMemory: return "OutOfMemory";
    case HandleStatus::PinOverflow: return "PinOverflow";
    }
    return "HandleStatus?";
}

// ----------------------------------------------------------------------------------- PinCore

bool PinCore::pin_locked() noexcept
{
    // Sob o mutex da fatia, com a entrada VIVA: o bit FECHANDO está desligado e só decrementos
    // (unpin) correm em paralelo, então a conferência do limite antes do incremento é segura.
    const std::uint32_t v = word_.load(std::memory_order_acquire);
    if ((v & ~kClosing) >= kClosing - 1) return false;
    word_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void PinCore::set_closing_locked() noexcept { word_.fetch_or(kClosing, std::memory_order_acq_rel); }

void PinCore::clear_closing_locked() noexcept
{
    word_.fetch_and(~kClosing, std::memory_order_acq_rel);
}

void PinCore::unpin() noexcept
{
    std::uint32_t v = word_.load(std::memory_order_acquire);
    // Caminho rápido: sem FECHANDO, um CAS que decrementa; depois dele nada mais é tocado. Se o bit
    // aparecer no meio, o CAS falha e o caminho lento é tomado.
    while ((v & kClosing) == 0) {
        if (word_.compare_exchange_weak(v, v - 1, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return;
        }
    }
    // Caminho lento (FECHANDO): o decremento e o despertar acontecem sob o mutex do objeto, e o
    // fechador só confere a contagem sob esse mesmo mutex: não há despertar perdido, e o fechador
    // não libera o objeto antes de esta thread soltar o mutex.
    std::lock_guard<std::mutex> g(m_);
    const std::uint32_t now = drop_one_slow();
    if ((now & ~kClosing) == 0) cv_.notify_all();
}

std::uint32_t PinCore::drop_one_slow() noexcept
{
    const std::uint32_t now = word_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (auto* fn = g_after_slow_decrement.load(std::memory_order_acquire)) fn();
    return now;
}

void PinCore::wait_unpinned() noexcept
{
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return (word_.load(std::memory_order_acquire) & ~kClosing) == 0; });
}

// ---------------------------------------------------------------------------- HandleRegistry

HandleRegistry& HandleRegistry::instance() noexcept
{
    // Armazenamento estático, construído uma vez e NUNCA destruído (nada depende da ordem de
    // destruição no fim do processo; tabelas restantes ficam alcançáveis).
    alignas(HandleRegistry) static unsigned char storage[sizeof(HandleRegistry)];
    static HandleRegistry* const registry = ::new (static_cast<void*>(storage)) HandleRegistry();
    return *registry;
}

HandleRegistry::~HandleRegistry()
{
    for (Shard& s : shards_) {
        delete[] s.slots;
        s.slots = nullptr;
    }
}

std::uint64_t HandleRegistry::hash(std::uintptr_t a) noexcept
{
    // Mistura de Fibonacci sobre o endereço sem os bits baixos (cascas alinhadas). Só o VALOR do
    // ponteiro: nada é lido do objeto.
    std::uint64_t x = static_cast<std::uint64_t>(a) >> 4;
    x ^= x >> 31;
    x *= 0x9E37'79B9'7F4A'7C15ull;
    x ^= x >> 29;
    return x;
}

std::size_t HandleRegistry::shard_of(const void* addr) noexcept
{
    return static_cast<std::size_t>(hash(reinterpret_cast<std::uintptr_t>(addr)) >> 58); // 6 bits
}

std::size_t HandleRegistry::find_locked(const Shard& s, std::uintptr_t addr) noexcept
{
    if (s.cap == 0) return 0;
    const std::size_t mask = s.cap - 1;
    std::size_t i = static_cast<std::size_t>(hash(addr)) & mask;
    for (std::size_t probes = 0; probes < s.cap; ++probes) {
        const std::uintptr_t a = s.slots[i].addr;
        if (a == addr) return i;
        if (a == kEmpty) return s.cap;
        i = (i + 1) & mask;
    }
    return s.cap;
}

bool HandleRegistry::grow_locked(Shard& s) noexcept
{
    // Capacidade nova: o dobro das entradas vivas (+1), no mínimo 16; as lápides são descartadas.
    const std::size_t want = next_pow2((s.count + 1) * 2 < kMinCap ? kMinCap : (s.count + 1) * 2);
    // `Entry` é trivial no sentido que importa aqui: o construtor só zera (nenhum contêiner, nada
    // que aloque além do próprio vetor; Regra R).
    static_assert(std::is_nothrow_default_constructible_v<Entry>);
    Entry* slots = new (std::nothrow) Entry[want]();
    if (slots == nullptr) return false;
    const std::size_t mask = want - 1;
    for (std::size_t i = 0; i < s.cap; ++i) {
        const Entry& e = s.slots[i];
        if (e.addr == kEmpty || e.addr == kTomb) continue;
        std::size_t j = static_cast<std::size_t>(hash(e.addr)) & mask;
        while (slots[j].addr != kEmpty)
            j = (j + 1) & mask;
        slots[j] = e;
    }
    delete[] s.slots;
    s.slots = slots;
    s.cap = want;
    s.tombs = 0;
    return true;
}

HandleStatus HandleRegistry::insert(const void* addr, HandleKind kind, void* owner,
                                    PinCore* core) noexcept
{
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    if (a == kEmpty || a == kTomb || kind == HandleKind::None || core == nullptr) {
        return HandleStatus::Unknown;
    }
    Shard& s = shards_[shard_of(addr)];
    std::lock_guard<std::mutex> g(s.m);
    if (find_locked(s, a) != s.cap) return HandleStatus::Duplicate;
    // Carga máxima de 3/4 contando as lápides; senão cresce (ou só reorganiza) a tabela.
    if (s.cap == 0 || (s.count + s.tombs + 1) * 4 > s.cap * 3) {
        if (!grow_locked(s)) return HandleStatus::OutOfMemory;
    }
    const std::size_t mask = s.cap - 1;
    std::size_t i = static_cast<std::size_t>(hash(a)) & mask;
    while (s.slots[i].addr != kEmpty && s.slots[i].addr != kTomb)
        i = (i + 1) & mask;
    if (s.slots[i].addr == kTomb) --s.tombs;
    // O PinCore começa sem pins e VIVO (precondição: novo, ou de um fechamento que terminou).
    core->word_.store(0, std::memory_order_release);
    s.slots[i] = Entry{a, owner, core, kind, 0};
    ++s.count;
    return HandleStatus::Ok;
}

HandleLookup HandleRegistry::acquire(const void* addr, HandleKind kind) noexcept
{
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    HandleLookup out{};
    if (a == kEmpty || a == kTomb) return out;
    Shard& s = shards_[shard_of(addr)];
    std::lock_guard<std::mutex> g(s.m);
    const std::size_t i = find_locked(s, a);
    if (i == s.cap) return out; // Unknown
    const Entry& e = s.slots[i];
    if (e.kind != kind) {
        out.status = HandleStatus::WrongKind;
        return out;
    }
    if (e.closing != 0) {
        // Sem pin, owner e core ficam nulos: o fechador pode liberar o corpo a qualquer instante.
        out.status = HandleStatus::Closing;
        return out;
    }
    if (!e.core->pin_locked()) {
        out.status = HandleStatus::PinOverflow;
        return out;
    }
    out.owner = e.owner;
    out.core = e.core;
    out.status = HandleStatus::Ok;
    return out;
}

HandleLookup HandleRegistry::begin_close(const void* addr, HandleKind kind) noexcept
{
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    HandleLookup out{};
    if (a == kEmpty || a == kTomb) return out;
    Shard& s = shards_[shard_of(addr)];
    std::lock_guard<std::mutex> g(s.m);
    const std::size_t i = find_locked(s, a);
    if (i == s.cap) return out;
    Entry& e = s.slots[i];
    if (e.kind != kind) {
        out.status = HandleStatus::WrongKind;
        return out;
    }
    if (e.closing != 0) {
        // Outro fechador venceu: owner e core ficam nulos (o corpo é dele).
        out.status = HandleStatus::Closing;
        return out;
    }
    e.closing = 1;
    e.core->set_closing_locked(); // a partir daqui nenhum pin novo; a contagem só diminui
    out.owner = e.owner;
    out.core = e.core;
    out.status = HandleStatus::Ok;
    return out;
}

HandleStatus HandleRegistry::cancel_close(const void* addr, HandleKind kind) noexcept
{
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    if (a == kEmpty || a == kTomb) return HandleStatus::Unknown;
    Shard& s = shards_[shard_of(addr)];
    std::lock_guard<std::mutex> g(s.m);
    const std::size_t i = find_locked(s, a);
    if (i == s.cap) return HandleStatus::Unknown;
    Entry& e = s.slots[i];
    if (e.kind != kind) return HandleStatus::WrongKind;
    if (e.closing == 0) return HandleStatus::NotClosing;
    e.closing = 0;
    e.core->clear_closing_locked();
    return HandleStatus::Ok;
}

HandleStatus HandleRegistry::remove(const void* addr, HandleKind kind) noexcept
{
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    if (a == kEmpty || a == kTomb) return HandleStatus::Unknown;
    Shard& s = shards_[shard_of(addr)];
    Entry* to_free = nullptr;
    {
        std::lock_guard<std::mutex> g(s.m);
        const std::size_t i = find_locked(s, a);
        if (i == s.cap) return HandleStatus::Unknown;
        Entry& e = s.slots[i];
        if (e.kind != kind) return HandleStatus::WrongKind;
        if (e.closing == 0) return HandleStatus::NotClosing;
        e = Entry{};
        e.addr = kTomb;
        --s.count;
        ++s.tombs;
        if (s.count == 0) {
            // A última entrada saiu: a tabela da fatia volta ao sistema (fora do mutex).
            to_free = s.slots;
            s.slots = nullptr;
            s.cap = 0;
            s.tombs = 0;
        }
    }
    delete[] to_free;
    return HandleStatus::Ok;
}

std::size_t HandleRegistry::size() const noexcept
{
    std::size_t n = 0;
    for (const Shard& s : shards_) {
        std::lock_guard<std::mutex> g(s.m);
        n += s.count;
    }
    return n;
}

std::size_t HandleRegistry::table_bytes() const noexcept
{
    std::size_t n = 0;
    for (const Shard& s : shards_) {
        std::lock_guard<std::mutex> g(s.m);
        n += s.cap * sizeof(Entry);
    }
    return n;
}

// ---------------------------------------------------------------------- ProcessCtxQuarantine

ProcessCtxQuarantine& ProcessCtxQuarantine::instance() noexcept
{
    // Vetor estático de 64 posições, nunca destruído: as cascas retidas no fim do processo são
    // memória alcançável (não vazamento para o LSan).
    alignas(ProcessCtxQuarantine) static unsigned char storage[sizeof(ProcessCtxQuarantine)];
    static ProcessCtxQuarantine* const q =
        ::new (static_cast<void*>(storage)) ProcessCtxQuarantine();
    return *q;
}

QuarantinedShell ProcessCtxQuarantine::push(void* ptr, std::size_t size) noexcept
{
    std::lock_guard<std::mutex> g(m_);
    return q_.push(ptr, size);
}

bool ProcessCtxQuarantine::contains(const void* p) const noexcept
{
    std::lock_guard<std::mutex> g(m_);
    return q_.contains(p);
}

std::size_t ProcessCtxQuarantine::size() const noexcept
{
    std::lock_guard<std::mutex> g(m_);
    return q_.size();
}

// ------------------------------------------------------------------------- ganchos de teste

void testing::PinCoreTestHook::set_after_slow_decrement(void (*fn)() noexcept) noexcept
{
    g_after_slow_decrement.store(fn, std::memory_order_release);
}

} // namespace swarmstream::detail
