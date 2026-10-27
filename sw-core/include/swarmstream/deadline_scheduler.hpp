// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Agendador por prazo v1 (F1-28, ADR-0003, docs/05): uma fonte, fila por prazo, cancelamento.
//
// - Dois tipos de pedido. DEMANDA vem de leitura e de sw_ensure_async: o jogo precisa da peça.
//   PREFETCH vem das dicas (sw_hint_*): é palpite, nunca garante nada. Quem chama classifica:
//   instalação, patch e modo completo em background são PREFETCH, mesmo vindo de
//   sw_ensure_async. Como demanda, passariam na frente de todo prefetch, até do urgente.
// - Ordem de saída para a fonte: toda demanda antes de todo prefetch; dentro de cada tipo, o
//   prazo mais cedo primeiro (empate: quem chegou antes). É o critério de aceite: "pedido urgente
//   passa na frente de prefetch". Prefetch é palpite e não disputa vaga com o jogo, nem quando o
//   prazo dele é mais cedo.
// - No máximo `max_in_flight` pedidos na fonte ao mesmo tempo; `reserved_for_demand` dessas vagas
//   só a demanda usa. Assim uma falta nunca espera um prefetch já em andamento terminar (com a
//   fila da fonte vazia, ela sai na hora). Use `max_in_flight` <= conexões da fonte: senão a fila
//   interna da fonte volta a pôr prefetch na frente de demanda que chegou depois.
// - Pedidos da mesma peça viram UM pedido à fonte. O prazo é o menor entre os pedidos ainda vivos;
//   se um deles é demanda, a peça é demanda (uma dica nova com prazo mais curto antecipa a peça,
//   como diz o swarmstream.h). Um pedido que chega com a peça já na fonte só espera a resposta.
// - Prefetch cujo prazo passa antes de sair para a fonte é descartado (Timeout): nunca vira
//   pedido urgente (swarmstream.h, dicas). Demanda atrasada continua: o prazo é prioridade, não
//   limite (o limite de espera é o timeout de quem lê). O prazo é repassado à fonte, que o usa
//   como prioridade; a fonte não pode tratá-lo como limite (a HttpRangeSource não trata; a
//   MemoryContentSource devolve Timeout depois do prazo, então use prazos folgados com ela).
// - O prefetch vencido só vira Timeout quando chega a vez dele, isto é, quando há vaga de
//   prefetch e ele está na frente da fila. Até lá, pending() da peça continua true e o callback
//   atrasa (nunca se perde). Não há varredura por relógio.
// - cancel() tira só aquele pedido. Se era o último da peça: ainda na fila, a peça sai da fila e
//   a fonte nem fica sabendo; já na fonte, chama IContentSource::cancel e a peça é esquecida (um
//   pedido novo da mesma peça vai à fonte de novo, sem herdar o cancelamento).
// - Callback: exatamente uma vez por pedido aceito (submit devolveu id válido), com os dados, um
//   erro da fonte, Canceled ou Timeout (prefetch vencido). Sempre na thread de entrega do
//   agendador, uma por vez, nunca dentro de submit()/cancel(). Não pode bloquear; pode chamar
//   submit(), cancel() e pending(). Exceção que sair do callback é contada em
//   callback_exceptions, não derruba o agendador. NÃO destrua o agendador dentro de um callback:
//   o destrutor espera a thread de entrega, que não pode esperar por si mesma
//   (std::thread::join dá resource_deadlock_would_occur e o processo termina).
// - Threads: `max_in_flight` threads que pedem à fonte e esperam a resposta, mais uma de entrega.
//   Nenhuma é criada por pedido.
// - Destrutor: pedidos na fila recebem Canceled; os que estão na fonte recebem cancel() e o
//   callback vem com o que a fonte responder. Espera todas as threads (a fonte tem de responder
//   ao cancelamento) e entrega todos os callbacks antes de voltar. A fonte tem de viver mais que o
//   agendador.
// - Exceções da fonte: qualquer exceção de request() ou do future, de qualquer tipo, vira
//   NetworkError no callback. Falta de memória dentro do agendador (bad_alloc em submit, cancel
//   ou nas threads internas) termina o processo (std::terminate). Seguir em frente deixaria
//   filas e mapas desencontrados.
// - submit() aloca (o pedido e o callback). Não é o caminho quente de leitura (acerto de cache),
//   que não passa por aqui.
// - v1 não usa o SourceEstimator (F1-29): com uma fonte só não há fonte a escolher nem pedido
//   duplicado (hedging); a ordem pedida pelo critério é por prazo. O estimador entra quando houver
//   peers e CDN (docs/05).
#pragma once

#include "swarmstream/i_content_source.hpp"
#include "swarmstream/result.hpp"
#include "swarmstream/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace swarmstream {

enum class RequestKind : uint8_t {
    Demand,   // leitura ou sw_ensure_async: o jogo precisa da peça
    Prefetch, // dica: palpite, descartado se o prazo passar antes de sair para a fonte
};

struct SchedulerConfig
{
    unsigned max_in_flight = 4;       // pedidos na fonte ao mesmo tempo (1..64)
    unsigned reserved_for_demand = 1; // vagas só de demanda (0..max_in_flight-1)
};

struct SchedulerStats
{
    uint64_t submitted = 0;           // pedidos aceitos
    uint64_t coalesced = 0;           // pedidos que se juntaram a uma peça já pedida
    uint64_t sent_demand = 0;         // peças enviadas à fonte como demanda
    uint64_t sent_prefetch = 0;       // peças enviadas à fonte como prefetch
    uint64_t completed = 0;           // callbacks com sucesso
    uint64_t failed = 0;              // callbacks com erro da fonte
    uint64_t canceled = 0;            // callbacks com Canceled
    uint64_t expired = 0;             // callbacks de prefetch vencido antes de sair (Timeout)
    uint64_t callback_exceptions = 0; // callbacks que lançaram exceção
    uint64_t queued_demand = 0;       // agora: peças de demanda na fila
    uint64_t queued_prefetch = 0;     // agora: peças de prefetch na fila
    uint64_t in_flight = 0;           // agora: peças na fonte
};

// `id` é o devolvido por submit(). A resposta é compartilhada pelos pedidos da mesma peça.
using SchedulerCallback = std::function<void(RequestId id, const PieceResponse& response)>;

class DeadlineScheduler
{
public:
    // Valida a configuração e sobe as threads. O erro é um texto para o log de quem chama.
    [[nodiscard]] static Result<std::unique_ptr<DeadlineScheduler>, std::string>
    create(IContentSource& source, SchedulerConfig config = {});

    ~DeadlineScheduler();
    DeadlineScheduler(const DeadlineScheduler&) = delete;
    DeadlineScheduler& operator=(const DeadlineScheduler&) = delete;

    // Põe o pedido na fila. Devolve o id (nunca kInvalidRequestId) e o callback vem depois, ou
    // kInvalidRequestId se o agendador está parando, e aí o callback nunca vem. Callback vazio
    // é aceito (pedido sem aviso, como uma dica).
    [[nodiscard]] RequestId submit(const PieceId& piece, SteadyTimePoint deadline, RequestKind kind,
                                   SchedulerCallback callback);

    // true: o pedido estava pendente e o callback vem com Canceled. false: id desconhecido ou o
    // pedido já terminou (o callback já foi ou está indo com o resultado).
    bool cancel(RequestId id);

    // A peça tem pedido na fila ou na fonte (para sw_query: SW_RESIDENCY_PENDING).
    [[nodiscard]] bool pending(const PieceId& piece) const;

    [[nodiscard]] SchedulerStats stats() const;

private:
    class Impl;
    explicit DeadlineScheduler(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
