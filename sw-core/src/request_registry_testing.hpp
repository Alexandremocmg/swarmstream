// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ganchos SÓ para os testes do registro de pedidos (F1-67 parte B). Fora da API instalada; nada é
// exportado.
#pragma once

#include "request_registry.hpp"

#include <cstdint>

namespace swarmstream::detail::testing {

class RequestRegistryTestHook
{
public:
    // Blocos do registro (para conferir alinhamento e tamanho pedidos ao alocador).
    [[nodiscard]] static const void* pool(const RequestRegistry& r) noexcept { return r.pool_; }
    [[nodiscard]] static const void* table(const RequestRegistry& r) noexcept { return r.table_; }
    // A `nth`-ésima thread de conclusão do PRÓXIMO create() (1 = a primeira) falha como se o
    // sistema não a criasse (std::system_error). 0 desliga. Estado global: um teste por vez.
    static void fail_thread_start(std::uint32_t nth) noexcept;
    // Faz o papel de uma fonte que pega QUALQUER pedido enfileirado assim que ele aparece: conclui
    // o primeiro ENFILEIRADO do pool com `result` e devolve o id dele (0 se não havia). Serve para
    // provar que o *out_id é escrito antes de o pedido poder concluir.
    static sw_request_id complete_first_queued(RequestRegistry& r, sw_result result) noexcept;
};

} // namespace swarmstream::detail::testing
