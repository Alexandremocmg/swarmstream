// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// MpscQueue: fila intrusiva sem trava, muitas produtoras e UMA consumidora (F1-12, docs/04
// §Concorrência). É a fila de pedidos da thread dona do cache.
//
// - Intrusiva: o nó (MpscNode) mora dentro do pedido, que é de quem pede. A fila nunca aloca,
//   e por isso não tem capacidade máxima.
// - push() é wait-free (uma troca atômica e uma escrita) e pode ser chamado de qualquer thread.
//   pop() e empty() são SÓ da consumidora.
// - FIFO por produtora. Entre produtoras, vale a ordem das trocas atômicas no `head_`.
// - pop() pode devolver nullptr por um instante com um push em andamento (a produtora já
//   trocou o `head_` mas ainda não ligou o `next`). Nesse instante empty() pode devolver
//   false (o nó atrás do qual a produtora vai ligar ainda não saiu; a consumidora tenta de
//   novo) ou TRUE (a fila parecia vazia, a produtora vai ligar o nó no sentinela). O true é
//   seguro para a CacheOwner: a produtora ainda vai fazer a escrita seq_cst do `next` e, depois
//   dela, ler a marca "dormindo" e acordar a dona (protocolo de Dekker em cache_owner.cpp).
// - Um nó devolvido por pop() não é mais referenciado pela fila: quem pediu pode destruí-lo
//   depois que a consumidora terminar de usá-lo.
//
// Algoritmo: fila intrusiva MPSC de Dmitry Vyukov, com nó sentinela.
//
// As escritas e leituras do `next` que decidem se a fila está vazia são seq_cst, de propósito:
// a CacheOwner usa empty() e uma marca "dormindo" num protocolo de Dekker para não perder um
// despertar (ver cache_owner.cpp). Com seq_cst, nenhuma cerca solta é necessária, e o TSan
// entende todas as sincronizações.

#pragma once

#include <atomic>

namespace swarmstream {

struct MpscNode
{
    std::atomic<MpscNode*> mpsc_next{nullptr};
};

class MpscQueue
{
public:
    MpscQueue() noexcept : head_(&stub_), tail_(&stub_) {}
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Qualquer thread. O nó não pode estar em outra fila nem nesta.
    void push(MpscNode* node) noexcept
    {
        node->mpsc_next.store(nullptr, std::memory_order_relaxed);
        MpscNode* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->mpsc_next.store(node, std::memory_order_seq_cst); // publica o nó
    }

    // Só a consumidora. nullptr se vazia ou com um push no meio.
    [[nodiscard]] MpscNode* pop() noexcept
    {
        MpscNode* tail = tail_;
        MpscNode* next = tail->mpsc_next.load(std::memory_order_acquire);
        if (tail == &stub_) {
            if (next == nullptr) return nullptr;
            tail_ = next;
            tail = next;
            next = next->mpsc_next.load(std::memory_order_acquire);
        }
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        if (tail != head_.load(std::memory_order_acquire)) {
            return nullptr; // um push está ligando o próximo nó; tente de novo
        }
        push(&stub_); // `tail` é o último: o sentinela entra atrás dele para soltá-lo
        next = tail->mpsc_next.load(std::memory_order_acquire);
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

    // Só a consumidora. true quando não há nó para entregar; pode ser true com um push que
    // ainda não ligou o `next` (ver o topo do arquivo).
    [[nodiscard]] bool empty() const noexcept
    {
        // Um `tail_` que não é o sentinela é um nó ainda não entregue.
        return tail_ == &stub_ && stub_.mpsc_next.load(std::memory_order_seq_cst) == nullptr;
    }

private:
    std::atomic<MpscNode*> head_; // última produtora a entrar
    MpscNode* tail_;              // próximo a sair; só a consumidora mexe
    MpscNode stub_;
};

} // namespace swarmstream
