// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// CacheOwner: a thread de I/O dona do cache em disco e a fila de pedidos (F1-12, ADR-0005,
// docs/04 §Concorrência).
//
// - UMA thread (a "dona") é a única que toca no SlotAllocator, no StoreFiles e no índice.
//   As outras threads só montam um CacheRequest e o entregam com submit(); a resposta volta
//   no próprio pedido. Nada é compartilhado entre a dona e quem pede, a não ser o pedido, que
//   passa de mão em mão pela fila (MpscQueue) e pela trava do pedido.
// - O pedido é de quem pede (pilha, membro, pool), nunca da fila: enviar e receber não aloca.
//   O caminho de leitura (Get) não aloca em lugar nenhum, nem na dona (teste
//   test_owner_read_path_does_not_allocate).
// - submit() nunca espera pela dona: é um push wait-free e, só se a dona estiver dormindo,
//   uma trava curta para acordá-la. done() também não bloqueia (try_lock). wait() bloqueia,
//   e é para quem pode esperar (thread de stream, ferramentas, testes), nunca para o jogo.
// - Escrita (Put), nesta ordem (04 §3, F1-11): aloca o slot, calcula o XXH3-64 de cada bloco
//   sobre os bytes em memória, grava os dados, Barreira 1 (flush do store), registro
//   COMMIT_PIECE no journal.bin, Barreira 2 (flush do journal) e só então publica no índice.
//   Remove grava FREE_SLOT e faz a Barreira 2 ANTES de o slot poder ser reaproveitado.
// - Leitura (Get): copia a peça e confere o XXH3-64 de cada bloco antes de devolver. Um bloco
//   que não confere tira a peça do cache (FREE_SLOT) e devolve CorruptedChecksum: nada
//   estragado sai do cache (04 §5, passo 5).
// - start() recupera o estado (04 §5): index.bin do último checkpoint mais os registros do
//   journal; depois grava um checkpoint novo. O relatório fica em recovery().
// - Falha de sync no store ou no journal deixa o cache quebrado (broken()): tudo dá IoError
//   até stop() e um start() novo, que refaz a recuperação. Nunca se repete um sync que falhou.
// - Get COPIA a peça para o buffer de quem pede. Como a dona faz a leitura e a escrita, uma
//   leitura nunca vê meia escrita nem um slot reaproveitado por outra peça.
// - Leases (F1-13, ADR-0005): Acquire prende a peça num PieceLease. Enquanto houver um lease
//   da peça, ela NUNCA sai do cache nem tem o slot reaproveitado: Remove devolve SlotBusy sem
//   gravar nada; um bloco que não confere no Get marca a peça como estragada (Get e Acquire
//   passam a devolver CorruptedChecksum, nada estragado sai do cache), mas o slot só é liberado
//   no primeiro pedido que tocar a peça depois do último lease solto. A remoção CLOCK (F1-14)
//   tem de pular os slots presos pelo mesmo motivo.
//   * A contagem é um atômico por slot. Só a dona soma (no Acquire); qualquer thread subtrai
//     (PieceLease::reset), sem fila, sem trava e sem alocar. A dona só libera um slot dentro
//     de invalidate(), depois de ler a contagem com acquire e vê-la em zero. Como só ela soma,
//     nenhum lease novo aparece entre a leitura e a liberação; um lease solto no meio só deixa
//     a remoção recusada (conservador).
//   * O lease vale para a CacheOwner que o deu. Depois de stop() ninguém remove nada; um lease
//     que sobreviver à CacheOwner continua seguro de soltar (a tabela é compartilhada), mas não
//     prende nada numa CacheOwner nova (start() de novo começa sem leases).
// - Remoção CLOCK (F1-14, docs/04 §Política de remoção): ligada por CacheOwnerConfig::
//   evict_high_percent/evict_low_percent (0 = desligada, só o Remove explícito). Depois de um
//   Put que leve o uso (peças = slots ocupados) a >= a marca alta, a dona remove peças até o uso
//   chegar à marca baixa, na mesma thread, em lotes: um FREE_SLOT por peça e UM sync por lote;
//   os slots só são liberados depois do sync. O ponteiro do relógio dá segunda chance a quem foi
//   lido (Get/Acquire) ou gravado como quente desde a última passada. NUNCA saem: peça fixada
//   (Put com set_pinned(true) ou CacheOp::SetPinned: grupo `core`, UI e boot) e peça com lease.
//   Peça de um Put frio (set_cold, prefetch) só fica a salvo se for lida antes da passada.
//   O fixado vale só em memória (não vai ao journal): depois de start() o jogo marca de novo.
//   Se tudo o que resta está fixado ou preso, a remoção para e o Put devolve OutOfSlots: o uso
//   do teto nunca passa de 100%.
// - A peça é identificada pela raiz Merkle (`PieceId::hash`, ADR-0007); o índice da peça no
//   manifesto (`PieceId::index`) não entra no cache em disco.

#pragma once

#include "swarmstream/mpsc_queue.hpp"
#include "swarmstream/result.hpp"
#include "swarmstream/store_files.hpp"
#include "swarmstream/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>

namespace swarmstream {

namespace detail {
enum class CrashPoint : int; // cache_journal.hpp (testes de queda)
using CrashHook = void (*)(CrashPoint point, void* context);
} // namespace detail

enum class CacheOp : uint8_t {
    Put,       // grava `input` como a peça; sucesso devolve o tamanho
    Get,       // copia a peça para `output` e confere os blocos; sucesso devolve o tamanho
    Remove,    // tira a peça do cache e libera o slot; sucesso devolve 0; SlotBusy com lease
    Acquire,   // prende a peça no PieceLease de quem pede (F1-13); sucesso devolve o tamanho;
               // pedido sem PieceLease (construtor genérico) dá InvalidSize
    SetPinned, // fixa (ou solta) a peça, segundo o pedido (F1-14); sucesso devolve 0
};

namespace detail {
struct LeaseTable;  // contagem de leases por slot (cache_owner.cpp)
struct LeaseAccess; // a dona preenche o PieceLease (cache_owner.cpp)
} // namespace detail

// Um lease de uma peça do cache em disco (F1-13, ADR-0005): enquanto ele existir, a peça não
// sai do cache nem tem o slot reaproveitado. Vem de um pedido Acquire. Só se move; soltar
// (reset() ou destrutor) vale em qualquer thread, nunca espera a dona e não aloca. Não é
// thread-safe: um mesmo PieceLease não pode ser usado por duas threads ao mesmo tempo.
class PieceLease
{
public:
    PieceLease() noexcept = default;
    ~PieceLease() { reset(); }
    PieceLease(const PieceLease&) = delete;
    PieceLease& operator=(const PieceLease&) = delete;
    PieceLease(PieceLease&& other) noexcept;
    PieceLease& operator=(PieceLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return table_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] const PieceHash& piece() const noexcept { return piece_; }
    [[nodiscard]] SlotId slot() const noexcept { return slot_; }   // kInvalidSlot se vazio
    [[nodiscard]] uint32_t size() const noexcept { return size_; } // bytes da peça

    // Solta o lease (se houver). Depois disso a peça pode ser removida.
    void reset() noexcept;

private:
    friend struct detail::LeaseAccess;
    std::shared_ptr<detail::LeaseTable> table_;
    PieceHash piece_{};
    SlotId slot_ = kInvalidSlot;
    uint32_t size_ = 0;
};

// Um pedido à thread dona. É de quem pede e não se move (o nó da fila mora dentro dele).
// Regras:
// - `input` (Put), `output` (Get), `lease` (Acquire) e `blocks` têm de continuar válidos até o
//   pedido terminar. Acquire com sucesso troca o lease que `lease` tinha pelo novo; com erro,
//   não mexe em `lease`.
// - `blocks`: o tamanho gravado de cada bloco da peça, na ordem (do manifesto, 03 §5). Vazio
//   = a peça é um bloco só. Put calcula um XXH3-64 por bloco; Get precisa da mesma lista para
//   conferir (InvalidOffset se ela não bate com a peça gravada).
// - Um pedido pendente não pode ser reenviado (submit devolve false). Um pedido concluído pode
//   ser reenviado (o resultado anterior é apagado).
// - O destrutor de um pedido ainda pendente espera a resposta (nunca deixa a dona escrever em
//   memória liberada).
class CacheRequest : private MpscNode
{
public:
    CacheRequest(CacheOp op, const PieceId& piece, std::span<const std::byte> input,
                 std::span<std::byte> output, std::span<const uint32_t> blocks = {},
                 bool pinned = false) noexcept;
    CacheRequest(const PieceId& piece, PieceLease& lease) noexcept; // Acquire
    ~CacheRequest();
    CacheRequest(const CacheRequest&) = delete;
    CacheRequest& operator=(const CacheRequest&) = delete;

    [[nodiscard]] static CacheRequest put(const PieceId& piece, std::span<const std::byte> data,
                                          std::span<const uint32_t> blocks = {}) noexcept
    {
        return CacheRequest(CacheOp::Put, piece, data, {}, blocks);
    }
    [[nodiscard]] static CacheRequest get(const PieceId& piece, std::span<std::byte> out,
                                          std::span<const uint32_t> blocks = {}) noexcept
    {
        return CacheRequest(CacheOp::Get, piece, {}, out, blocks);
    }
    [[nodiscard]] static CacheRequest remove(const PieceId& piece) noexcept
    {
        return CacheRequest(CacheOp::Remove, piece, {}, {});
    }
    // Fixa (true) ou solta (false) a peça já gravada: a remoção CLOCK nunca a escolhe (F1-14).
    [[nodiscard]] static CacheRequest pin(const PieceId& piece, bool pinned = true) noexcept
    {
        return CacheRequest(CacheOp::SetPinned, piece, {}, {}, {}, pinned);
    }
    // Só para Put (antes de submit): grava a peça já fixada / como fria (prefetch, F1-14).
    void set_pinned(bool pinned) noexcept { pinned_ = pinned; }
    void set_cold(bool cold) noexcept { cold_ = cold; }
    [[nodiscard]] static CacheRequest acquire(const PieceId& piece, PieceLease& lease) noexcept
    {
        return CacheRequest(piece, lease);
    }

    // true quando a resposta chegou. Nunca bloqueia: se a dona estiver gravando a resposta
    // naquele instante, devolve false e a próxima chamada vê o resultado.
    [[nodiscard]] bool done() const noexcept;

    // Espera a resposta e a devolve. Bloqueia: não use na thread do jogo.
    [[nodiscard]] Result<size_t, CacheErrorCode> wait() const;

    // A resposta, depois de done() ou wait(). Antes disso devolve IoError.
    [[nodiscard]] Result<size_t, CacheErrorCode> result() const;

    [[nodiscard]] CacheOp op() const noexcept { return op_; }
    [[nodiscard]] const PieceId& piece() const noexcept { return piece_; }

private:
    friend class CacheOwner;

    // Quem envia: true se o pedido estava livre (nunca enviado ou já concluído).
    [[nodiscard]] bool arm() noexcept;
    // A dona (ou submit, ao recusar): grava a resposta e acorda quem espera. Depois de sair da
    // trava, nunca mais toca no pedido, que pode ser destruído por quem pediu.
    void complete(CacheErrorCode code, size_t bytes) noexcept;

    static CacheRequest* from_node(MpscNode* node) noexcept
    {
        return static_cast<CacheRequest*>(node);
    }

    CacheOp op_;
    PieceId piece_;
    std::span<const std::byte> input_;
    std::span<std::byte> output_;
    std::span<const uint32_t> blocks_;
    PieceLease* lease_ = nullptr; // Acquire
    bool pinned_ = false;         // Put (fixar ao gravar) e SetPinned (o valor novo)
    bool cold_ = false;           // Put: entra sem a marca de "lida" (prefetch)

    std::atomic<bool> pending_{false}; // entre arm() e complete()
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool done_ = false;                             // guardado por mutex_
    CacheErrorCode code_ = CacheErrorCode::IoError; // guardado por mutex_
    size_t bytes_ = 0;                              // guardado por mutex_
};

struct CacheOwnerConfig
{
    std::filesystem::path dir; // pasta dos store-XXX.bin, do index.bin e do journal.bin
    uint32_t total_slots = 0;  // teto do cache, em slots (> 0; pode diminuir entre aberturas)
    StoreLayout layout = {};
    // Checkpoint (04 §4) quando o journal passa deste tamanho; também há um ao abrir e ao parar.
    uint64_t checkpoint_journal_bytes = uint64_t{64} << 20;
    // Remoção CLOCK (F1-14), em % do teto (total_slots). 0/0 = desligada. Ligada, exige
    // 0 < baixa < alta <= 100; senão start() devolve InvalidSize.
    uint32_t evict_high_percent = 0;
    uint32_t evict_low_percent = 0;
};

// Marcas do docs/04: a remoção começa em 95% do teto e para em 85%.
inline constexpr uint32_t kDefaultEvictHighPercent = 95;
inline constexpr uint32_t kDefaultEvictLowPercent = 85;

// O que start() encontrou no disco (04 §5). Para os testes e a telemetria.
struct CacheRecovery
{
    bool index_loaded = false;            // index.bin válido usado
    bool index_invalid = false;           // index.bin existia, mas foi descartado
    uint64_t index_lsn = 0;               // LSN do checkpoint do índice usado
    bool journal_created = false;         // journal.bin não existia
    bool journal_header_ok = true;        // false: cabeçalho inválido, registros descartados
    uint64_t journal_records_applied = 0; // registros depois do checkpoint, aplicados
    uint64_t journal_truncated_bytes = 0; // cauda inválida descartada
    uint32_t dropped_ceiling = 0;         // peças além do teto (o teto diminuiu)
    uint32_t dropped_missing_file = 0;    // peças num store-XXX.bin que não existe
    uint32_t dropped_conflict = 0;        // slot reaproveitado ou registro de outro layout
    uint32_t pieces = 0;                  // peças no índice depois da recuperação
};

class CacheOwner
{
public:
    // Abre o store, recupera o índice e o journal (04 §5), grava um checkpoint e sobe a thread
    // dona. Erros do StoreFiles::open (IoError, InvalidSize), da recuperação (IoError,
    // DiskFull) ou IoError se a thread não pôde ser criada.
    [[nodiscard]] static Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
    start(CacheOwnerConfig config);

    // Para a thread (como stop()) e fecha o store sem relatar erro. Chamar submit() de outra
    // thread durante ou depois da destruição é uso incorreto: a parada só protege os submit()
    // que começaram antes de a dona sair (o objeto deixa de existir depois). Quem pode enviar
    // pedidos tem de parar de enviar antes de destruir a CacheOwner; stop() continua seguro.
    ~CacheOwner();
    CacheOwner(const CacheOwner&) = delete;
    CacheOwner& operator=(const CacheOwner&) = delete;

    // Entrega o pedido à dona. Qualquer thread; nunca espera pela dona. false se o pedido ainda
    // está pendente (nada é feito). Depois de stop(), o pedido termina na hora com Stopped.
    [[nodiscard]] bool submit(CacheRequest& request) noexcept;

    // submit + wait. Bloqueia. Um pedido ainda pendente devolve SlotBusy sem ser reenviado.
    [[nodiscard]] Result<size_t, CacheErrorCode> call(CacheRequest& request);

    // Para a dona e fecha o store, devolvendo o erro do fechamento. Os pedidos que estavam na
    // fila são atendidos ou terminam com Stopped; todos recebem resposta, inclusive os que
    // outras threads enviarem durante a parada. Chamar de novo devolve sucesso.
    [[nodiscard]] Result<void, CacheErrorCode> stop() noexcept;

    // true na thread dona (para asserts de quem estende a dona: F1-11, F1-13).
    [[nodiscard]] bool is_owner_thread() const noexcept;

    // Contadores para os testes e a telemetria (leitura relaxada, valores aproximados).
    [[nodiscard]] uint64_t requests_processed() const noexcept;
    [[nodiscard]] uint64_t sleeps() const noexcept; // vezes que a dona dormiu sem pedidos
    [[nodiscard]] uint32_t pieces() const noexcept; // peças publicadas no índice
    // Leases dados por esta CacheOwner e ainda não soltos (F1-13).
    [[nodiscard]] uint64_t leases() const noexcept;
    // Remoção CLOCK (F1-14): peças removidas e bytes liberados desde start().
    [[nodiscard]] uint64_t evicted_pieces() const noexcept;
    [[nodiscard]] uint64_t evicted_bytes() const noexcept;
    // true depois de uma falha de sync: só stop() e um start() novo resolvem.
    [[nodiscard]] bool broken() const noexcept;
    // O que start() encontrou. Escrito antes de a dona existir; só leitura depois.
    [[nodiscard]] const CacheRecovery& recovery() const noexcept;

private:
    friend struct CacheOwnerTestHook; // src/cache_owner_testing.hpp, só para os testes
    [[nodiscard]] static Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
    start_impl(CacheOwnerConfig config, detail::CrashHook hook, void* context);
    struct Impl;
    explicit CacheOwner(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
