// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Fonte HTTP (CDN) implementando IContentSource, com conexões persistentes e Range (F1-27).
//
// - HTTP/1.1 puro sobre TCP (http://). https:// exige TLS, que o sw-core ainda não tem: a criação
//   recusa com erro explícito (decisão pendente com o dono do produto).
// - Um pequeno conjunto de conexões persistentes (keep-alive), uma por thread de trabalho. Os
//   pedidos pendentes saem em ordem de prazo (o mais cedo primeiro).
// - Peça = objeto "<base>/p/<hex(piece_hash)>" (docs/03 §7.2) por padrão, ou qualquer lugar que o
//   `locator` indicar, inclusive uma faixa (Range) de um arquivo de pacote (docs/03 §10).
// - Falha no meio do corpo: refaz o pedido com `Range: bytes=<recebido>-` e continua de onde parou.
// - Nunca devolve dado a mais do que `max_piece_bytes`; a verificação do hash da peça é do
//   Verifier (aqui só há um gancho opcional `verifier`).
#pragma once

#include "swarmstream/i_content_source.hpp"
#include "swarmstream/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace swarmstream {

// Onde uma peça está no servidor.
struct PieceLocation
{
    std::string path; // caminho absoluto no servidor, ex.: "/p/ab12..." (já com o prefixo da base)
    // Faixa de bytes [first, last] (inclusive) do objeto, quando a peça é uma janela de um arquivo
    // maior. Sem faixa, a peça é o objeto inteiro.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
};

using PieceLocator = std::function<PieceLocation(const PieceId&, std::string_view base_path)>;
using PieceVerifier = std::function<bool(const PieceId&, std::span<const std::uint8_t> data)>;

struct HttpSourceConfig
{
    std::string base_url;     // "http://host[:porta][/prefixo]"
    std::string name = "cdn"; // nome devolvido por IContentSource::name()
    unsigned connections = 4; // conexões persistentes e threads de trabalho (1..64)
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds io_timeout{10000};      // por send/recv
    std::chrono::milliseconds request_timeout{60000}; // por peça, somando as tentativas
    unsigned max_attempts = 3;                        // tentativas por peça (1..10)
    std::size_t max_piece_bytes = kDefaultPieceSize;  // maior corpo aceito
    // `locator` e `verifier` são chamados pelas threads de trabalho, várias ao mesmo tempo, e
    // devem ser thread-safe (sem estado mutável compartilhado sem trava).
    // `locator` e `verifier` são chamados pelas threads de trabalho, várias ao mesmo tempo: devem
    // ser thread-safe (nada de estado mutável compartilhado sem trava).
    PieceLocator locator;   // vazio = "/p/<hex>" sob o prefixo da base
    PieceVerifier verifier; // vazio = não verifica (devolve CorruptedData se retornar false)
};

// Contadores para os testes e para a telemetria; separados de SourceStats (contrato congelado).
struct HttpSourceDiagnostics
{
    std::uint64_t connections_opened = 0;
    std::uint64_t requests_on_reused_connection = 0;
    std::uint64_t retries = 0;          // tentativas além da primeira
    std::uint64_t resumed_requests = 0; // refeitos com Range a partir do que já tinha chegado
    std::uint64_t http_errors = 0;      // respostas 4xx/5xx
};

class HttpRangeSource final : public IContentSource
{
public:
    // Valida a configuração e sobe as threads. O erro é um texto para o log do chamador.
    [[nodiscard]] static Result<std::unique_ptr<HttpRangeSource>, std::string>
    create(HttpSourceConfig config);

    ~HttpRangeSource() override;
    HttpRangeSource(const HttpRangeSource&) = delete;
    HttpRangeSource& operator=(const HttpRangeSource&) = delete;

    [[nodiscard]] RequestTicket request(const PieceId& piece, SteadyTimePoint deadline) override;
    bool cancel(RequestId request_id) override;
    [[nodiscard]] SourceStats stats() const override;
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] HttpSourceDiagnostics diagnostics() const;

private:
    class Impl;
    HttpRangeSource(std::string name, std::unique_ptr<Impl> impl);

    std::string name_;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
