// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// TelemetryLog (F1-30): eventos de telemetria gravados em arquivo (docs/08 §Eventos de
// telemetria e §Como medir). Protocolo aberto: JSON Lines (um objeto por linha), campos em
// inglês e snake_case.
//
// - emit_*() pode ser chamado de qualquer thread, NÃO aloca e NÃO bloqueia: copia o evento
//   para um anel pré-alocado (fila limitada, muitas produtoras, uma consumidora). Anel cheio:
//   o evento é descartado e contado em dropped(); nunca bloqueia o jogo.
// - Uma thread gravadora própria esvazia o anel e escreve no arquivo.
// - Nenhuma falha passa em silêncio: descarte e erro de escrita são contados (dropped(),
//   write_errors()) e, ao fechar, o arquivo termina com uma linha "telemetry_summary".
// - Eventos deste módulo: read_miss_start, read_miss, eviction, frame_stutter.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace swarmstream {

enum class MissOutcome : uint8_t { Completed, Failed, Cancelled, PendingAtEnd };
enum class MissSource : uint8_t { None, Cdn, Peer };

struct ReadMissStart
{
    uint64_t request_id = 0;
    std::string_view path; // truncado em TelemetryLog::kMaxPath bytes
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t deadline_ms = 0;
    uint32_t timeout_ms = 0;
    uint64_t t0_us = 0;
};

struct ReadMissEnd
{
    uint64_t request_id = 0;
    uint64_t t0_us = 0; // para calcular miss_wait_us = t1_us - t0_us
    uint64_t t1_us = 0;
    std::string_view call_result; // "SW_OK", "SW_E_NOT_LOCAL" ou "SW_E_TIMEOUT"
    MissOutcome outcome = MissOutcome::Completed;
    MissSource source = MissSource::None;
    std::string_view error; // código SW_E_*, só com outcome = Failed
};

class TelemetryLog
{
public:
    static constexpr size_t kMaxPath = 160;
    static constexpr size_t kMaxText = 32; // call_result, error, cause

    // Abre (trunca) o arquivo e inicia a thread gravadora. capacity = nº de eventos do anel,
    // arredondado para cima a potência de 2 (mínimo 2). nullptr se o arquivo não abre ou
    // não há memória.
    [[nodiscard]] static std::unique_ptr<TelemetryLog> open(const std::filesystem::path& file,
                                                            size_t capacity = 4096) noexcept;

    // Instante monotônico em microssegundos (o relógio de t0_us/t1_us).
    [[nodiscard]] static uint64_t now_us() noexcept;

    // Drena o anel, escreve o resumo e fecha o arquivo.
    ~TelemetryLog();
    TelemetryLog(const TelemetryLog&) = delete;
    TelemetryLog& operator=(const TelemetryLog&) = delete;

    // Cada uma devolve false se o evento foi descartado (anel cheio).
    bool emit_read_miss_start(const ReadMissStart& e) noexcept;
    bool emit_read_miss(const ReadMissEnd& e) noexcept;
    bool emit_eviction(uint64_t pieces, uint64_t bytes) noexcept;
    bool emit_frame_stutter(uint64_t duration_us, std::string_view cause) noexcept;

    [[nodiscard]] uint64_t dropped() const noexcept;      // eventos perdidos por anel cheio
    [[nodiscard]] uint64_t written() const noexcept;      // eventos já gravados
    [[nodiscard]] uint64_t write_errors() const noexcept; // eventos que o arquivo recusou

    // Espera o anel esvaziar e descarrega o arquivo (para os testes e antes de ler o arquivo).
    void flush() noexcept;

private:
    struct Impl;
    explicit TelemetryLog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
