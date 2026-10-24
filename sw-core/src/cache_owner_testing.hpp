// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ganchos de teste da CacheOwner (F1-12, revisão da R2). Ficam em src/, fora da API instalada:
// só os testes do sw-core incluem este arquivo.
//
// Servem para abrir à força a janela da parada: um submit() que já leu `stopping == false`
// mas ainda não fez o push. É ali que a dona precisa esperar `inflight` zerar e depois drenar
// a fila; sem um dos dois, o pedido fica sem resposta. O TSan não vê isso (não é corrida de
// dados, é pedido perdido), por isso o teste é determinístico e não depende do tempo.
#pragma once

#include "swarmstream/cache_owner.hpp"

#include <chrono>
#include <cstdint>

namespace swarmstream {

struct CacheOwnerTestHook
{
    // A partir daqui, todo submit() para depois de ler `stopping` e antes do push, até
    // open_submit_window().
    static void close_submit_window(CacheOwner& owner) noexcept;
    static void open_submit_window(CacheOwner& owner) noexcept;
    // true quando exatamente `count` submit() estão parados na janela.
    static bool wait_submitters_in_window(CacheOwner& owner, uint32_t count,
                                          std::chrono::milliseconds timeout) noexcept;
    // true quando a dona viu `stopping` e saiu do laço principal (com a janela fechada).
    static bool wait_owner_left_loop(CacheOwner& owner, std::chrono::milliseconds timeout) noexcept;
};

} // namespace swarmstream
