// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/cache_owner.hpp"

#include "cache_journal.hpp"
#include "cache_owner_testing.hpp"
#include "store_files_testing.hpp"
#include "swarmstream/slot_allocator.hpp"
#include "swarmstream/xxh3.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace swarmstream {

// ---------------------------------------------------------------- CacheRequest

CacheRequest::CacheRequest(CacheOp op, const PieceId& piece, std::span<const std::byte> input,
                           std::span<std::byte> output, std::span<const uint32_t> blocks,
                           bool pinned) noexcept
    : op_(op), piece_(piece), input_(input), output_(output), blocks_(blocks), pinned_(pinned)
{
}

CacheRequest::CacheRequest(const PieceId& piece, PieceLease& lease) noexcept
    : op_(CacheOp::Acquire), piece_(piece), lease_(&lease)
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

// ---------------------------------------------------------------- PieceLease

namespace detail {

// Contagem de leases por slot (F1-13). Compartilhada com os PieceLease: soltar um lease depois
// de a CacheOwner ser destruída ainda toca em memória válida. A contagem é de 32 bits: estourar
// exigiria 2^32 PieceLease vivos de um mesmo slot (centenas de GB só de objetos).
struct LeaseTable
{
    explicit LeaseTable(uint32_t slots)
        : counts(std::make_unique<std::atomic<uint32_t>[]>(slots)) // começa em zero
    {
    }
    std::unique_ptr<std::atomic<uint32_t>[]> counts; // um por slot; só a dona soma
    std::atomic<uint64_t> total{0};                  // leases vivos (telemetria e testes)
};

struct LeaseAccess
{
    // Só na thread dona (é a única que soma). Não aloca: copia o shared_ptr.
    static void grant(PieceLease& out, const std::shared_ptr<LeaseTable>& table,
                      const PieceHash& piece, SlotId slot, uint32_t size) noexcept
    {
        table->counts[slot].fetch_add(1, std::memory_order_relaxed);
        table->total.fetch_add(1, std::memory_order_relaxed);
        out.reset(); // depois de somar: trocar por um lease do mesmo slot nunca passa por zero
        out.table_ = table;
        out.piece_ = piece;
        out.slot_ = slot;
        out.size_ = size;
    }
};

} // namespace detail

PieceLease::PieceLease(PieceLease&& other) noexcept
    : table_(std::move(other.table_)), piece_(std::exchange(other.piece_, PieceHash{})),
      slot_(std::exchange(other.slot_, kInvalidSlot)), size_(std::exchange(other.size_, 0))
{
}

PieceLease& PieceLease::operator=(PieceLease&& other) noexcept
{
    if (this != &other) {
        reset();
        table_ = std::move(other.table_);
        piece_ = std::exchange(other.piece_, PieceHash{});
        slot_ = std::exchange(other.slot_, kInvalidSlot);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void PieceLease::reset() noexcept
{
    if (!table_) return;
    // release: tudo o que quem tinha o lease fez com a peça acontece antes de a dona ver a
    // contagem em zero (ela lê com acquire) e reaproveitar o slot.
    table_->counts[slot_].fetch_sub(1, std::memory_order_release);
    table_->total.fetch_sub(1, std::memory_order_relaxed);
    table_.reset();
    piece_ = {};
    slot_ = kInvalidSlot;
    size_ = 0;
}

// ---------------------------------------------------------------- CacheOwner

struct CacheOwner::Impl
{
    struct Entry
    {
        SlotId slot = kInvalidSlot;
        uint32_t size = 0;
        std::vector<uint64_t> blocks; // XXH3-64 por bloco; reservado na escrita, nunca na leitura
        // F1-13: um bloco não conferiu com a peça presa por lease. Nunca é servida; o slot sai
        // no primeiro pedido que tocar a peça sem lease. Só vale em memória: o checkpoint grava
        // a entrada como as outras e, depois de reabrir, o XXH3 do bloco a pega de novo.
        bool corrupt = false;
    };

    Impl(StoreFiles s, CacheOwnerConfig c)
        : store(std::move(s)), slots(c.total_slots), config(std::move(c)),
          leases(std::make_shared<detail::LeaseTable>(config.total_slots))
    {
    }

    // Só a thread dona toca nestes (depois de start() e antes do join em stop()). start() os
    // prepara antes de a thread existir.
    StoreFiles store;
    SlotAllocator slots;
    CacheOwnerConfig config;
    // F1-13: contagem de leases por slot. A dona soma (Acquire) e lê (invalidate); os
    // PieceLease subtraem de qualquer thread. Criada antes de a thread existir.
    std::shared_ptr<detail::LeaseTable> leases;
    std::unordered_map<PieceHash, Entry> index; // chave: a raiz Merkle (ADR-0007)

    // F1-14: o relógio. Um item por slot, reservado em start(); só a dona toca.
    struct ClockSlot
    {
        PieceHash piece{};
        bool used = false;
        bool ref = false; // lida ou gravada como quente desde a última passada do ponteiro
        bool pinned = false;
    };
    struct Victim
    {
        PieceHash piece;
        SlotId slot;
    };
    static constexpr size_t kEvictBatch = 64; // FREE_SLOT por sync
    std::vector<ClockSlot> clock;
    std::vector<Victim> victims; // reservado: remover não aloca
    uint32_t hand = 0;
    uint32_t high_slots = 0; // 0 = remoção desligada
    uint32_t low_slots = 0;
    std::atomic<bool> evict_throw_next{false}; // CacheOwnerTestHook (F1-14)
    std::atomic<bool> evict_fail_sync_next{false};
    std::atomic<uint64_t> evicted_pieces{0};
    std::atomic<uint64_t> evicted_bytes{0};
    detail::Journal journal;
    uint64_t next_lsn = 1;
    bool broken = false; // um sync falhou: tudo dá IoError até fechar e reabrir
    CacheRecovery recovery;

    // CacheOwnerTestHook (testes de queda): chamado em pontos da escrita e do checkpoint.
    detail::CrashHook crash_hook = nullptr;
    void* crash_context = nullptr;
    std::atomic<bool> fail_next_store_sync{false};
    std::atomic<bool> fail_next_journal_sync{false};
    std::atomic<bool> skip_stop_checkpoint{false}; // simula queda: o journal fica com os registros
    std::atomic<bool> corrupt_next_read{false};    // F1-13: o próximo Get vê um byte trocado

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
    std::atomic<bool> broken_flag{false}; // cópia de `broken` para leitura fora da dona

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

    [[nodiscard]] std::filesystem::path index_path() const { return config.dir / "index.bin"; }
    [[nodiscard]] std::filesystem::path journal_path() const { return config.dir / "journal.bin"; }

    void crash(detail::CrashPoint p) const noexcept
    {
        if (crash_hook) crash_hook(p, crash_context);
    }
    void mark_broken() noexcept
    {
        broken = true;
        broken_flag.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] CacheErrorCode recover();
    [[nodiscard]] CacheErrorCode checkpoint();
    [[nodiscard]] CacheErrorCode append_record(detail::JournalRecord& rec);
    [[nodiscard]] CacheErrorCode append_and_sync(detail::JournalRecord& rec);
    // Remove peças pelo relógio até sobrarem no máximo `target`. Success também quando não há
    // mais o que remover (tudo fixado ou preso): quem precisa do slot confere depois.
    [[nodiscard]] CacheErrorCode evict_to(uint32_t target);
    void maybe_evict() noexcept;
    void touch(SlotId slot) noexcept { clock[slot].ref = true; }
    [[nodiscard]] CacheErrorCode invalidate(PieceHash piece);
    [[nodiscard]] CacheErrorCode drop_corrupt(PieceHash piece);
    // Leitura com acquire: o uso da peça por quem soltou o lease (release) acontece antes de o
    // slot ser reaproveitado.
    [[nodiscard]] bool leased(SlotId slot) const noexcept
    {
        return leases->counts[slot].load(std::memory_order_acquire) != 0;
    }

    void run() noexcept;
    void process(CacheRequest& r) noexcept;
    // Podem lançar bad_alloc (índice, buffer do journal); process() trata.
    void put(CacheRequest& r);
    void get(CacheRequest& r);
    void remove(CacheRequest& r);
    void acquire(CacheRequest& r) noexcept;
    void set_pinned(CacheRequest& r) noexcept;
};

// 04 §5. Índice do último checkpoint + registros do journal com LSN maior; depois descarta o
// que não pode ser servido (fora do teto, arquivo de store ausente) e grava um checkpoint
// novo, para o journal recomeçar limpo.
CacheErrorCode CacheOwner::Impl::recover()
{
    const StoreLayout& layout = config.layout;
    detail::LoadedIndex loaded;
    const detail::IndexLoad load = detail::read_index_file(index_path(), layout, loaded);
    recovery.index_loaded = load == detail::IndexLoad::Ok;
    recovery.index_invalid = load == detail::IndexLoad::Invalid;
    recovery.index_lsn = recovery.index_loaded ? loaded.checkpoint_lsn : 0;

    std::unordered_map<SlotId, PieceHash> by_slot;
    auto erase_piece = [&](const PieceHash& piece) {
        if (auto it = index.find(piece); it != index.end()) {
            by_slot.erase(it->second.slot);
            index.erase(it);
        }
    };
    auto put_entry = [&](const PieceHash& piece, Entry e) {
        if (auto s = by_slot.find(e.slot); s != by_slot.end() && s->second != piece) {
            // O slot foi reaproveitado: o dado antigo foi sobrescrito, vale o registro novo.
            erase_piece(s->second);
            ++recovery.dropped_conflict;
        }
        erase_piece(piece); // a mesma peça noutro slot: vale a mais nova
        by_slot[e.slot] = piece;
        index[piece] = std::move(e);
    };
    if (recovery.index_loaded) {
        for (detail::IndexEntry& e : loaded.entries) {
            put_entry(e.piece, Entry{e.slot, e.size, std::move(e.blocks)});
        }
    }

    auto apply = [&](const detail::JournalRecord& r) {
        const uint64_t slot = uint64_t{r.store_file_id} * layout.slots_per_file + r.slot_in_file;
        if (r.slot_in_file >= layout.slots_per_file || slot >= kInvalidSlot) {
            ++recovery.dropped_conflict; // registro de outro layout: não aponta para slot válido
            return;
        }
        if (r.type == detail::JournalType::CommitPiece) {
            put_entry(r.piece, Entry{static_cast<SlotId>(slot), r.piece_size, r.blocks});
        } else if (r.type == detail::JournalType::FreeSlot) {
            if (auto s = by_slot.find(static_cast<SlotId>(slot));
                s != by_slot.end() && s->second == r.piece) {
                erase_piece(r.piece);
            }
        }
    };
    detail::JournalReplay replay;
    auto opened = detail::Journal::open(journal_path(), recovery.index_lsn, apply, replay);
    if (!opened) return opened.error();
    journal = std::move(*opened);
    recovery.journal_created = replay.created;
    recovery.journal_header_ok = replay.header_ok;
    recovery.journal_records_applied = replay.applied;
    recovery.journal_truncated_bytes = replay.truncated_bytes;
    next_lsn = std::max(replay.last_lsn, recovery.index_lsn) + 1;

    // O que não pode ser servido sai do índice: fora do teto (o teto diminuiu), maior que o
    // slot, ou num store-XXX.bin que não existe (no Windows não há fsync da pasta: um arquivo
    // criado logo antes de uma queda pode sumir). Arquivo presente mas zerado ou estragado é
    // pego pelo checksum por bloco na leitura.
    for (auto it = index.begin(); it != index.end();) {
        const Entry& e = it->second;
        bool drop = false;
        if (e.slot >= config.total_slots) {
            ++recovery.dropped_ceiling;
            drop = true;
        } else if (e.size > layout.slot_size) {
            ++recovery.dropped_conflict;
            drop = true;
        } else {
            std::error_code ec;
            const auto file = StoreFiles::file_path(config.dir, e.slot / layout.slots_per_file);
            if (!std::filesystem::exists(file, ec) || ec) {
                ++recovery.dropped_missing_file;
                drop = true;
            }
        }
        it = drop ? index.erase(it) : std::next(it);
    }

    // Teto menor: apaga os arquivos além do teto e encurta o último (F1-10, item 6).
    if (auto s = store.shrink_to_ceiling(); !s) return s.error();
    for (const auto& [piece, e] : index) {
        if (!slots.mark_used(e.slot)) return CacheErrorCode::IoError; // não deveria ocorrer
        clock[e.slot] = ClockSlot{piece, true, false, false}; // sem marca: ninguém leu ainda
    }
    pieces.store(static_cast<uint32_t>(index.size()), std::memory_order_relaxed);
    recovery.pieces = static_cast<uint32_t>(index.size());
    return checkpoint();
}

// 04 §4: index.bin novo por rename atômico, depois um journal novo (cabeçalho e CHECKPOINT)
// também por rename. Uma queda entre os dois renames deixa o índice novo com o journal antigo,
// cujos registros têm LSN menor ou igual ao do índice e são pulados na recuperação.
CacheErrorCode CacheOwner::Impl::checkpoint()
{
    const uint64_t lsn = next_lsn - 1;
    std::vector<detail::IndexEntry> entries;
    entries.reserve(index.size());
    for (const auto& [piece, e] : index) {
        detail::IndexEntry out;
        out.piece = piece;
        out.slot = e.slot;
        out.size = e.size;
        out.blocks = e.blocks;
        entries.push_back(std::move(out));
    }
    std::vector<const detail::IndexEntry*> ptrs;
    ptrs.reserve(entries.size());
    for (const auto& e : entries)
        ptrs.push_back(&e);
    std::filesystem::path tmp = index_path();
    tmp += ".tmp";
    if (auto rc = detail::write_index_file(index_path(), tmp, lsn, config.layout, ptrs, crash_hook,
                                           crash_context);
        rc != CacheErrorCode::Success) {
        return rc;
    }
    if (auto rc = journal.close(); rc != CacheErrorCode::Success) return rc;
    if (auto rc = detail::Journal::create_fresh(journal_path(), lsn, crash_hook, crash_context);
        rc != CacheErrorCode::Success) {
        return rc;
    }
    detail::JournalReplay replay;
    auto opened =
        detail::Journal::open(journal_path(), lsn, [](const detail::JournalRecord&) {}, replay);
    if (!opened) return opened.error();
    journal = std::move(*opened);
    next_lsn = std::max(next_lsn, replay.last_lsn + 1);
    return CacheErrorCode::Success;
}

CacheErrorCode CacheOwner::Impl::append_record(detail::JournalRecord& rec)
{
    if (fail_next_journal_sync.exchange(false)) journal.fail_next_sync();
    rec.lsn = next_lsn++;
    rec.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return journal.append(rec, crash_hook, crash_context);
}

CacheErrorCode CacheOwner::Impl::append_and_sync(detail::JournalRecord& rec)
{
    if (auto rc = append_record(rec); rc != CacheErrorCode::Success) return rc;
    return journal.sync(); // Barreira 2: ponto de commit
}

// F1-14. Ponteiro do relógio sobre os slots: pula livre, fixado e preso por lease; peça com a
// marca "lida" perde a marca e ganha outra volta; a primeira sem marca é a vítima. Duas voltas
// bastam para achar tudo o que é removível (a primeira limpa as marcas). Cada lote grava um
// FREE_SLOT por vítima e faz UM sync (Barreira 2); os slots só são liberados depois dele.
CacheErrorCode CacheOwner::Impl::evict_to(uint32_t target)
{
    const uint32_t total = config.total_slots;
    while (index.size() > target) {
        victims.clear();
        const size_t want = std::min<size_t>(index.size() - target, kEvictBatch);
        for (uint64_t budget = uint64_t{2} * total + 2; victims.size() < want && budget > 0;
             --budget) {
            const SlotId s = hand;
            hand = hand + 1 == total ? 0 : hand + 1;
            ClockSlot& c = clock[s];
            if (!c.used || c.pinned || leased(s)) continue;
            if (c.ref) {
                c.ref = false;
                continue;
            }
            victims.push_back({c.piece, s});
            c.used = false; // não é escolhida duas vezes no lote
        }
        if (victims.empty()) return CacheErrorCode::Success; // nada removível
        for (const Victim& v : victims) {
            detail::JournalRecord rec;
            rec.type = detail::JournalType::FreeSlot;
            rec.piece = v.piece;
            rec.store_file_id = v.slot / config.layout.slots_per_file;
            rec.slot_in_file = v.slot % config.layout.slots_per_file;
            if (append_record(rec) != CacheErrorCode::Success) {
                mark_broken();
                return CacheErrorCode::IoError;
            }
        }
        if (evict_fail_sync_next.exchange(false)) journal.fail_next_sync();
        if (journal.sync() != CacheErrorCode::Success) { // Barreira 2, antes de liberar os slots
            mark_broken();
            return CacheErrorCode::IoError;
        }
        for (const Victim& v : victims) {
            const auto it = index.find(v.piece);
            if (it == index.end() || it->second.slot != v.slot || !slots.release(v.slot)) {
                mark_broken(); // índice e alocador discordam: não deveria ocorrer
                return CacheErrorCode::IoError;
            }
            evicted_bytes.fetch_add(it->second.size, std::memory_order_relaxed);
            index.erase(it);
        }
        evicted_pieces.fetch_add(victims.size(), std::memory_order_relaxed);
        pieces.fetch_sub(static_cast<uint32_t>(victims.size()), std::memory_order_relaxed);
    }
    return CacheErrorCode::Success;
}

// Marca alta atingida: desce até a marca baixa (docs/04: começa em 95%, para em 85%).
void CacheOwner::Impl::maybe_evict() noexcept
{
    // Roda DEPOIS de o Put responder: quem pediu pode já ter destruído o pedido. Nada escapa
    // daqui, porque o catch de process() completaria esse pedido de novo (uso depois da
    // liberação). Falha de alocação (índice, buffer do journal, checkpoint) quebra o cache.
    try {
        if (high_slots == 0 || index.size() < high_slots) return;
        if (evict_throw_next.exchange(false)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            throw std::bad_alloc();
        }
        if (evict_to(low_slots) == CacheErrorCode::Success && !broken &&
            journal.size_bytes() > config.checkpoint_journal_bytes) {
            if (checkpoint() != CacheErrorCode::Success) mark_broken();
        }
    } catch (...) {
        mark_broken();
    }
}

// Tira a peça do cache: FREE_SLOT no journal (sincronizado) ANTES de o slot poder ser
// reaproveitado; senão uma queda deixaria o journal apontando para dado de outra peça.
// É o ÚNICO lugar que libera o slot de uma peça (F1-13): com lease, devolve SlotBusy sem gravar
// nada. Como só a dona soma leases, nenhum aparece entre a leitura da contagem e a liberação.
CacheErrorCode CacheOwner::Impl::invalidate(PieceHash piece)
{
    const auto it = index.find(piece);
    if (it == index.end()) return CacheErrorCode::PieceNotFound;
    if (leased(it->second.slot)) return CacheErrorCode::SlotBusy;
    detail::JournalRecord rec;
    rec.type = detail::JournalType::FreeSlot;
    rec.piece = piece;
    rec.store_file_id = it->second.slot / config.layout.slots_per_file;
    rec.slot_in_file = it->second.slot % config.layout.slots_per_file;
    if (auto rc = append_and_sync(rec); rc != CacheErrorCode::Success) {
        mark_broken();
        return CacheErrorCode::IoError;
    }
    if (auto rel = slots.release(it->second.slot); !rel) {
        mark_broken(); // índice e alocador discordam: não deveria ocorrer
        return CacheErrorCode::IoError;
    }
    clock[it->second.slot].used = false;
    index.erase(it);
    pieces.fetch_sub(1, std::memory_order_relaxed);
    return CacheErrorCode::Success;
}

// Peça estragada (Entry::corrupt, ou um bloco que acabou de não conferir): nunca é servida.
// Tenta tirá-la agora; com lease, fica marcada até o último sair. Devolve o código para quem
// pediu: CorruptedChecksum, ou o erro do FREE_SLOT (IoError).
CacheErrorCode CacheOwner::Impl::drop_corrupt(PieceHash piece)
{
    if (const auto it = index.find(piece); it != index.end()) it->second.corrupt = true;
    const CacheErrorCode rc = invalidate(piece);
    return rc == CacheErrorCode::Success || rc == CacheErrorCode::SlotBusy
               ? CacheErrorCode::CorruptedChecksum
               : rc;
}

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
    if (broken) { // fechar e reabrir refaz a recuperação (04 §5)
        r.complete(CacheErrorCode::IoError, 0);
        return;
    }
    try {
        switch (r.op_) {
        case CacheOp::Put: put(r); return;
        case CacheOp::Get: get(r); return;
        case CacheOp::Remove: remove(r); return;
        case CacheOp::Acquire: acquire(r); return;
        case CacheOp::SetPinned: set_pinned(r); return;
        }
    } catch (...) { // sem memória para o índice ou o buffer do journal
        mark_broken();
        r.complete(CacheErrorCode::IoError, 0);
        return;
    }
    r.complete(CacheErrorCode::IoError, 0); // op inválida (memória corrompida?)
}

void CacheOwner::Impl::put(CacheRequest& r)
{
    const size_t size = r.input_.size();
    if (size == 0 || size > store.layout().slot_size || size > detail::kMaxPieceSize) {
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    // Limites dos blocos (do manifesto): sem lista, a peça é um bloco só.
    uint64_t sum = 0;
    for (uint32_t b : r.blocks_) {
        if (b == 0) {
            r.complete(CacheErrorCode::InvalidSize, 0);
            return;
        }
        sum += b;
    }
    if (!r.blocks_.empty() && sum != size) {
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    if (const auto it = index.find(r.piece_.hash); it != index.end()) {
        if (!it->second.corrupt) {
            ClockSlot& c = clock[it->second.slot];
            if (r.pinned_) c.pinned = true;
            if (!r.cold_) c.ref = true;
            r.complete(CacheErrorCode::Success, it->second.size); // já está: não regrava
            return;
        }
        // Estragada: sai antes de gravar de novo; com lease (SlotBusy), o slot dela não muda.
        if (const CacheErrorCode rc = invalidate(r.piece_.hash); rc != CacheErrorCode::Success) {
            r.complete(rc, 0);
            return;
        }
    }
    auto slot = slots.allocate();
    if (!slot && slot.error() == CacheErrorCode::OutOfSlots && high_slots != 0) {
        // Cheio mesmo com a remoção ligada (a anterior achou tudo fixado ou preso): tenta de
        // novo, porque leases podem ter sido soltos desde então.
        if (const CacheErrorCode rc = evict_to(low_slots); rc != CacheErrorCode::Success) {
            r.complete(rc, 0);
            return;
        }
        slot = slots.allocate();
    }
    if (!slot) {
        r.complete(slot.error(), 0);
        return;
    }
    // Checksums sobre os bytes em memória, antes de gravar (04 §2): nunca relendo o disco.
    Entry entry{*slot, static_cast<uint32_t>(size), {}};
    if (r.blocks_.empty()) {
        entry.blocks.push_back(xxh3_64(r.input_));
    } else {
        entry.blocks.reserve(r.blocks_.size());
        size_t off = 0;
        for (uint32_t b : r.blocks_) {
            entry.blocks.push_back(xxh3_64(r.input_.subspan(off, b)));
            off += b;
        }
    }
    // 04 §3: dados, Barreira 1, registro no journal, Barreira 2, publicação.
    auto fail = [&](CacheErrorCode code) {
        mark_broken(); // o estado no disco é desconhecido: só a recuperação decide
        r.complete(code == CacheErrorCode::DiskFull ? code : CacheErrorCode::IoError, 0);
    };
    if (auto w = store.write(*slot, 0, r.input_); !w) {
        (void)slots.release(*slot);
        if (w.error() == CacheErrorCode::DiskFull) { // reserva do arquivo: nada foi gravado
            r.complete(CacheErrorCode::DiskFull, 0);
            return;
        }
        fail(w.error());
        return;
    }
    crash(detail::CrashPoint::AfterDataWrite);
    if (fail_next_store_sync.exchange(false)) StoreFilesTestHook::fail_next_sync(store);
    if (auto f = store.flush(); !f) {
        fail(f.error());
        return;
    }
    crash(detail::CrashPoint::AfterDataFlush);
    detail::JournalRecord rec;
    rec.type = detail::JournalType::CommitPiece;
    rec.piece = r.piece_.hash;
    rec.store_file_id = *slot / config.layout.slots_per_file;
    rec.slot_in_file = *slot % config.layout.slots_per_file;
    rec.piece_size = static_cast<uint32_t>(size);
    rec.blocks = entry.blocks;
    if (auto rc = append_and_sync(rec); rc != CacheErrorCode::Success) {
        fail(rc);
        return;
    }
    crash(detail::CrashPoint::AfterJournalSync);
    index.emplace(r.piece_.hash, std::move(entry));
    clock[*slot] = ClockSlot{r.piece_.hash, true, !r.cold_, r.pinned_};
    pieces.fetch_add(1, std::memory_order_relaxed);
    if (journal.size_bytes() > config.checkpoint_journal_bytes) {
        if (checkpoint() != CacheErrorCode::Success) mark_broken(); // a peça já é durável
    }
    r.complete(CacheErrorCode::Success, size);
    maybe_evict(); // depois da resposta: quem pediu não espera pela remoção
}

void CacheOwner::Impl::get(CacheRequest& r)
{
    const auto it = index.find(r.piece_.hash);
    if (it == index.end()) {
        r.complete(CacheErrorCode::PieceNotFound, 0);
        return;
    }
    const Entry& e = it->second;
    if (e.corrupt) {
        r.complete(drop_corrupt(r.piece_.hash), 0);
        return;
    }
    if (r.output_.size() < e.size) {
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    // Para conferir bloco a bloco é preciso saber onde cada um começa (do manifesto). Sem a
    // lista, só uma peça de um bloco pode ser conferida.
    if (r.blocks_.empty() ? e.blocks.size() != 1 : r.blocks_.size() != e.blocks.size()) {
        r.complete(CacheErrorCode::InvalidOffset, 0);
        return;
    }
    const std::span<std::byte> out = r.output_.first(e.size);
    if (auto rd = store.read(e.slot, 0, out); !rd) {
        r.complete(rd.error(), 0);
        return;
    }
    // CacheOwnerTestHook: fora dos testes, custa uma leitura relaxada.
    if (corrupt_next_read.load(std::memory_order_relaxed) && corrupt_next_read.exchange(false)) {
        out[0] ^= std::byte{0xFF};
    }
    // 04 §5, passo 5: nada sai do cache sem o XXH3 do próprio bloco conferir. Um bloco ruim
    // invalida a peça inteira, que é pedida de novo.
    bool ok = true;
    if (r.blocks_.empty()) {
        ok = xxh3_64(out) == e.blocks[0];
    } else {
        size_t off = 0;
        for (size_t i = 0; i < r.blocks_.size() && ok; ++i) {
            const uint32_t b = r.blocks_[i];
            ok = b > 0 && off + b <= out.size() && xxh3_64(out.subspan(off, b)) == e.blocks[i];
            off += b;
        }
        ok = ok && off == out.size();
    }
    if (!ok) {
        r.complete(drop_corrupt(r.piece_.hash), 0); // com lease, fica marcada e presa
        return;
    }
    touch(e.slot);
    r.complete(CacheErrorCode::Success, e.size);
}

void CacheOwner::Impl::acquire(CacheRequest& r) noexcept
{
    if (r.lease_ == nullptr) { // CacheRequest(CacheOp::Acquire, ...) sem destino
        r.complete(CacheErrorCode::InvalidSize, 0);
        return;
    }
    const auto it = index.find(r.piece_.hash);
    if (it == index.end()) {
        r.complete(CacheErrorCode::PieceNotFound, 0);
        return;
    }
    if (it->second.corrupt) {
        r.complete(drop_corrupt(r.piece_.hash), 0);
        return;
    }
    detail::LeaseAccess::grant(*r.lease_, leases, r.piece_.hash, it->second.slot, it->second.size);
    touch(it->second.slot);
    r.complete(CacheErrorCode::Success, it->second.size);
}

void CacheOwner::Impl::set_pinned(CacheRequest& r) noexcept
{
    const auto it = index.find(r.piece_.hash);
    if (it == index.end()) {
        r.complete(CacheErrorCode::PieceNotFound, 0);
        return;
    }
    clock[it->second.slot].pinned = r.pinned_;
    r.complete(CacheErrorCode::Success, 0);
}

void CacheOwner::Impl::remove(CacheRequest& r)
{
    const CacheErrorCode rc = invalidate(r.piece_.hash);
    if (rc == CacheErrorCode::Success && journal.size_bytes() > config.checkpoint_journal_bytes) {
        if (checkpoint() != CacheErrorCode::Success) mark_broken();
    }
    r.complete(rc, 0);
}

Result<std::unique_ptr<CacheOwner>, CacheErrorCode> CacheOwner::start(CacheOwnerConfig config)
{
    return start_impl(std::move(config), nullptr, nullptr);
}

Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
CacheOwner::start_impl(CacheOwnerConfig config, detail::CrashHook hook, void* context)
{
    // Teto 0 é configuração errada (um CacheOwnerConfig{} só com a pasta): reduzir o teto a 0
    // apagaria todos os store-NNN.bin na recuperação.
    if (config.layout.slots_per_file == 0 || config.total_slots == 0) {
        return Unexpected(CacheErrorCode::InvalidSize);
    }
    const bool evict_on = config.evict_high_percent != 0 || config.evict_low_percent != 0;
    if (evict_on &&
        !(config.evict_low_percent > 0 && config.evict_low_percent < config.evict_high_percent &&
          config.evict_high_percent <= 100)) {
        return Unexpected(CacheErrorCode::InvalidSize);
    }
    auto store = StoreFiles::open(config.dir, config.total_slots, config.layout);
    if (!store) return Unexpected(store.error());
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(std::move(*store), std::move(config));
        impl->index.reserve(impl->config.total_slots); // o índice não reorganiza no meio do jogo
        impl->clock.resize(impl->config.total_slots);
        impl->victims.reserve(Impl::kEvictBatch);
        if (evict_on) {
            const uint64_t total = impl->config.total_slots;
            // Pelo menos 1 slot na marca alta e a baixa estritamente abaixo dela.
            impl->high_slots = static_cast<uint32_t>(
                std::max<uint64_t>(1, total * impl->config.evict_high_percent / 100));
            impl->low_slots = static_cast<uint32_t>(std::min<uint64_t>(
                total * impl->config.evict_low_percent / 100, impl->high_slots - 1));
        }
        impl->crash_hook = hook;
        impl->crash_context = context;
        if (auto rc = impl->recover(); rc != CacheErrorCode::Success) return Unexpected(rc);
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
    // Depois do join, o store, o índice e o journal são desta thread. Checkpoint para a próxima
    // abertura não precisar reler o journal; com o cache quebrado, só fecha (a recuperação
    // decide na próxima abertura).
    CacheErrorCode first = CacheErrorCode::Success;
    if (!s.broken && !s.skip_stop_checkpoint.load()) first = s.checkpoint();
    if (const auto rc = s.journal.close(); first == CacheErrorCode::Success) first = rc;
    if (auto rc = s.store.close(); !rc && first == CacheErrorCode::Success) first = rc.error();
    if (first != CacheErrorCode::Success) return Unexpected(first);
    return {};
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

uint64_t CacheOwner::leases() const noexcept
{
    return impl_->leases->total.load(std::memory_order_relaxed);
}

uint64_t CacheOwner::evicted_pieces() const noexcept
{
    return impl_->evicted_pieces.load(std::memory_order_relaxed);
}

uint64_t CacheOwner::evicted_bytes() const noexcept
{
    return impl_->evicted_bytes.load(std::memory_order_relaxed);
}

bool CacheOwner::broken() const noexcept
{
    return impl_->broken_flag.load(std::memory_order_relaxed);
}

const CacheRecovery& CacheOwner::recovery() const noexcept { return impl_->recovery; }

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

Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
CacheOwnerTestHook::start_with_crash_hook(CacheOwnerConfig config, detail::CrashHook hook,
                                          void* context)
{
    return CacheOwner::start_impl(std::move(config), hook, context);
}

void CacheOwnerTestHook::fail_next_store_sync(CacheOwner& owner) noexcept
{
    owner.impl_->fail_next_store_sync.store(true);
}

void CacheOwnerTestHook::fail_next_journal_sync(CacheOwner& owner) noexcept
{
    owner.impl_->fail_next_journal_sync.store(true);
}

uint64_t CacheOwnerTestHook::store_syncs(const CacheOwner& owner) noexcept
{
    return StoreFilesTestHook::file_syncs(owner.impl_->store);
}

uint64_t CacheOwnerTestHook::journal_syncs(const CacheOwner& owner) noexcept
{
    return owner.impl_->journal.syncs();
}

void CacheOwnerTestHook::skip_checkpoint_on_stop(CacheOwner& owner) noexcept
{
    owner.impl_->skip_stop_checkpoint.store(true);
}

void CacheOwnerTestHook::corrupt_next_read(CacheOwner& owner) noexcept
{
    owner.impl_->corrupt_next_read.store(true);
}

void CacheOwnerTestHook::throw_in_next_eviction(CacheOwner& owner) noexcept
{
    owner.impl_->evict_throw_next.store(true);
}

void CacheOwnerTestHook::fail_next_eviction_sync(CacheOwner& owner) noexcept
{
    owner.impl_->evict_fail_sync_next.store(true);
}

bool CacheOwnerTestHook::wait_owner_left_loop(CacheOwner& owner,
                                              std::chrono::milliseconds timeout) noexcept
{
    CacheOwner::Impl& s = *owner.impl_;
    std::unique_lock lock(s.hook_mutex);
    return s.hook_cv.wait_for(lock, timeout, [&] { return s.hook_owner_left_loop; });
}

} // namespace swarmstream
