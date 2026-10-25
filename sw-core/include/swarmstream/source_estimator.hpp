// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Estimador por fonte: RTT e banda do enlace por médias móveis exponenciais (F1-29).
// Não aloca nem sincroniza; quem chama é responsável por serializar o acesso.
// `duration` de record_transfer vai do primeiro ao último byte do corpo (sem o RTT).
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace swarmstream {

class SourceEstimator
{
public:
    void record_rtt(std::chrono::microseconds rtt) noexcept
    {
        if (rtt.count() <= 0) return;
        const double sample = static_cast<double>(rtt.count());
        rtt_us_ = rtt_us_ == 0.0 ? sample : rtt_us_ + (sample - rtt_us_) / 8.0;
    }

    void record_transfer(std::uint64_t bytes, std::chrono::microseconds duration,
                         unsigned concurrent) noexcept
    {
        if (bytes < 65536 || duration.count() <= 0 || concurrent == 0) return;
        // Converte antes de multiplicar: bytes * concurrent pode ultrapassar uint64_t.
        const double sample = static_cast<double>(bytes) * static_cast<double>(concurrent) *
                              1000000.0 / static_cast<double>(duration.count());
        bandwidth_ = bandwidth_ == 0.0 ? sample : bandwidth_ + (sample - bandwidth_) / 4.0;
    }

    [[nodiscard]] std::optional<std::chrono::microseconds> rtt() const noexcept
    {
        if (rtt_us_ == 0.0) return std::nullopt;
        return rounded_duration(rtt_us_);
    }

    [[nodiscard]] std::optional<double> bandwidth_bytes_per_sec() const noexcept
    {
        if (bandwidth_ == 0.0) return std::nullopt;
        return bandwidth_;
    }

    [[nodiscard]] std::optional<std::chrono::microseconds>
    estimate_fetch_time(std::uint64_t bytes) const noexcept
    {
        if (rtt_us_ == 0.0 || bandwidth_ == 0.0) return std::nullopt;
        // Usa o RTT interno: arredonda uma única vez, depois de somar a transferência.
        return rounded_duration(rtt_us_ + static_cast<double>(bytes) * 1000000.0 / bandwidth_);
    }

private:
    static std::chrono::microseconds rounded_duration(double value) noexcept
    {
        using Duration = std::chrono::microseconds;
        using Rep = Duration::rep;
        const double rounded = std::ceil(value);
        // O máximo convertido para double pode arredondar para cima. Compara antes do cast.
        if (rounded >= static_cast<double>((std::numeric_limits<Rep>::max)())) {
            return Duration::max();
        }
        return Duration{static_cast<Rep>(rounded)};
    }

    double rtt_us_{0.0};
    double bandwidth_{0.0};
};

} // namespace swarmstream
