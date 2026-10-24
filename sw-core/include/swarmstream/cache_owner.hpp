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
// - Escrita (Put), nesta ordem: aloca o slot, grava os dados, flush (Barreira 1 do 04 §3) e
//   publica no índice. O journal e o índice em disco entram na F1-11, entre o flush e a
//   publicação. Até lá, o índice só existe em RAM e some ao fechar.
// - Ainda não há leases (F1-13): Get COPIA a peça para o buffer de quem pede, e Remove libera
//   o slot na hora. Como a dona faz as duas coisas, uma leitura nunca vê meia escrita nem um
//   slot reaproveitado por outra peça.
//
// O índice desta tarefa é provisório (um unordered_map reservado para o teto, só na dona). A
// F1-11 o substitui pelo índice com journal, mantendo a dona e a fila.

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

enum class CacheOp : uint8_t {
    Put,    // grava `input` como a peça; sucesso devolve o tamanho
    Get,    // copia a peça para `output`; sucesso devolve o tamanho
    Remove, // tira a peça do cache e libera o slot; sucesso devolve 0
};

// Um pedido à thread dona. É de quem pede e não se move (o nó da fila mora dentro dele).
// Regras:
// - `input` (Put) e `output` (Get) têm de continuar válidos até o pedido terminar.
// - Um pedido pendente não pode ser reenviado (submit devolve false). Um pedido concluído pode
//   ser reenviado (o resultado anterior é apagado).
// - O destrutor de um pedido ainda pendente espera a resposta (nunca deixa a dona escrever em
//   memória liberada).
class CacheRequest : private MpscNode
{
public:
    CacheRequest(CacheOp op, const PieceId& piece, std::span<const std::byte> input,
                 std::span<std::byte> output) noexcept;
    ~CacheRequest();
    CacheRequest(const CacheRequest&) = delete;
    CacheRequest& operator=(const CacheRequest&) = delete;

    [[nodiscard]] static CacheRequest put(const PieceId& piece,
                                          std::span<const std::byte> data) noexcept
    {
        return CacheRequest(CacheOp::Put, piece, data, {});
    }
    [[nodiscard]] static CacheRequest get(const PieceId& piece, std::span<std::byte> out) noexcept
    {
        return CacheRequest(CacheOp::Get, piece, {}, out);
    }
    [[nodiscard]] static CacheRequest remove(const PieceId& piece) noexcept
    {
        return CacheRequest(CacheOp::Remove, piece, {}, {});
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

    std::atomic<bool> pending_{false}; // entre arm() e complete()
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool done_ = false;                             // guardado por mutex_
    CacheErrorCode code_ = CacheErrorCode::IoError; // guardado por mutex_
    size_t bytes_ = 0;                              // guardado por mutex_
};

struct CacheOwnerConfig
{
    std::filesystem::path dir; // pasta dos store-XXX.bin; precisa existir
    uint32_t total_slots = 0;  // teto do cache, em slots
    StoreLayout layout = {};
};

class CacheOwner
{
public:
    // Abre o store e sobe a thread dona. Erros do StoreFiles::open (IoError, InvalidSize);
    // IoError se a thread não pôde ser criada.
    [[nodiscard]] static Result<std::unique_ptr<CacheOwner>, CacheErrorCode>
    start(CacheOwnerConfig config);

    // Para a thread (como stop()) e fecha o store sem relatar erro.
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

private:
    struct Impl;
    explicit CacheOwner(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
