// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-29: amostras sintéticas, sem rede nem relógio real. SW_CHECK funciona com NDEBUG.
#include "check.hpp"
#include "swarmstream/source_estimator.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>

namespace {

using swarmstream::SourceEstimator;
using Us = std::chrono::microseconds;

void check_empty(const SourceEstimator& estimator)
{
    SW_CHECK(!estimator.rtt());
    SW_CHECK(!estimator.bandwidth_bytes_per_sec());
    SW_CHECK(!estimator.estimate_fetch_time(1048576));
}

void test_empty()
{
    SourceEstimator estimator;
    check_empty(estimator);
    std::puts("sem amostra: ok");
}

void test_ignored()
{
    SourceEstimator estimator;
    const auto invalid = [&estimator]() {
        estimator.record_rtt(Us{0});
        estimator.record_rtt(Us{-1});
        estimator.record_transfer(1, Us{1}, 1);
        estimator.record_transfer(65535, Us{1}, 1);
        estimator.record_transfer(65536, Us{0}, 1);
        estimator.record_transfer(65536, Us{-1}, 1);
        estimator.record_transfer(65536, Us{1}, 0);
    };
    invalid();
    check_empty(estimator);
    estimator.record_rtt(Us{10000});
    estimator.record_transfer(65536, Us{65536}, 1);
    invalid();
    SW_CHECK(estimator.rtt()->count() == 10000);
    SW_CHECK(*estimator.bandwidth_bytes_per_sec() == 1000000.0);
    std::puts("amostras inválidas e limite de 64 KiB: ok");
}

void test_ewma()
{
    SourceEstimator estimator;
    estimator.record_rtt(Us{8000});
    estimator.record_transfer(1000000, Us{1000000}, 1);
    SW_CHECK(estimator.rtt()->count() == 8000);
    SW_CHECK(*estimator.bandwidth_bytes_per_sec() == 1000000.0);
    estimator.record_rtt(Us{16000});
    estimator.record_transfer(2000000, Us{1000000}, 1);
    SW_CHECK(estimator.rtt()->count() == 9000); // Trocar /8 por /4 produz 10000.
    SW_CHECK(*estimator.bandwidth_bytes_per_sec() == 1250000.0);
    estimator.record_rtt(Us{1000});
    estimator.record_transfer(250000, Us{1000000}, 1);
    SW_CHECK(estimator.rtt()->count() == 8000);
    SW_CHECK(*estimator.bandwidth_bytes_per_sec() == 1000000.0);
    std::puts("primeira amostra e EWMA crescente/decrescente: ok");
}

void test_concurrent()
{
    SourceEstimator one;
    SourceEstimator four;
    one.record_transfer(1048576, Us{1000000}, 1);
    four.record_transfer(1048576, Us{1000000}, 4);
    SW_CHECK(*one.bandwidth_bytes_per_sec() == 1048576.0);
    SW_CHECK(*four.bandwidth_bytes_per_sec() == 4194304.0);
    SW_CHECK(*four.bandwidth_bytes_per_sec() == 4.0 * *one.bandwidth_bytes_per_sec());
    std::puts("concorrência multiplica a banda: ok");
}

void test_estimate()
{
    SourceEstimator estimator;
    estimator.record_rtt(Us{10000});
    SW_CHECK(!estimator.estimate_fetch_time(500000));
    estimator.record_transfer(1000000, Us{1000000}, 1);
    SW_CHECK(estimator.estimate_fetch_time(500000)->count() == 510000);
    SW_CHECK(estimator.estimate_fetch_time(0)->count() == 10000);
    SourceEstimator only_bandwidth;
    only_bandwidth.record_transfer(1000000, Us{1000000}, 1);
    SW_CHECK(!only_bandwidth.estimate_fetch_time(500000));
    // 3 B/s e RTT de 1 us: 1 byte leva 333333,333... us, mais o RTT.
    SourceEstimator fractional;
    fractional.record_rtt(Us{1});
    fractional.record_transfer(300000, Us{100000000000LL}, 1);
    SW_CHECK(*fractional.bandwidth_bytes_per_sec() == 3.0);
    SW_CHECK(fractional.estimate_fetch_time(1)->count() == 333335);
    // A fração da EWMA é preservada até a conversão final.
    SourceEstimator fractional_rtt;
    fractional_rtt.record_rtt(Us{1});
    fractional_rtt.record_rtt(Us{2});
    fractional_rtt.record_transfer(2000000, Us{1000000}, 1);
    SW_CHECK(fractional_rtt.rtt()->count() == 2);
    SW_CHECK(fractional_rtt.estimate_fetch_time(1)->count() == 2);
    std::puts("estimativa, amostras parciais e arredondamento: ok");
}

void test_extremes()
{
    const auto max_bytes = (std::numeric_limits<std::uint64_t>::max)();
    SourceEstimator fast;
    fast.record_rtt(Us{1});
    fast.record_transfer(max_bytes, Us{1}, (std::numeric_limits<unsigned>::max)());
    SW_CHECK(std::isfinite(*fast.bandwidth_bytes_per_sec()));
    SW_CHECK(*fast.bandwidth_bytes_per_sec() > static_cast<double>(max_bytes));
    SW_CHECK(fast.estimate_fetch_time(max_bytes)->count() >= 1);
    SourceEstimator slow;
    slow.record_rtt(Us{1});
    slow.record_transfer(65536, Us::max(), 1);
    SW_CHECK(*slow.bandwidth_bytes_per_sec() > 0.0);
    SW_CHECK(slow.estimate_fetch_time(max_bytes) == Us::max());
    SourceEstimator large_rtt;
    large_rtt.record_rtt(Us::max());
    large_rtt.record_transfer(1000000, Us{1000000}, 1);
    SW_CHECK(large_rtt.rtt() == Us::max());
    SW_CHECK(large_rtt.estimate_fetch_time(max_bytes) == Us::max());
    large_rtt.record_rtt(Us{1});
    SW_CHECK(large_rtt.rtt()->count() > 0);
    SW_CHECK(*large_rtt.rtt() < Us::max());
    std::puts("extremos numéricos e saturação: ok");
}

void test_profiles()
{
    struct Profile
    {
        const char* name;
        double bandwidth;
        double rtt_us;
    };
    constexpr Profile profiles[] = {
        {"fibra_300", 37500000.0, 10000.0},
        {"cabo_50", 6250000.0, 25000.0},
        {"adsl_10", 1250000.0, 60000.0},
    };
    std::mt19937 random{20261025u};
    // Escala a saída do gerador diretamente: sequência igual entre bibliotecas padrão.
    const auto jitter = [&random]() {
        return 0.9 + 0.2 * static_cast<double>(random()) /
                         static_cast<double>((std::mt19937::max)());
    };
    for (const auto& profile : profiles) {
        SourceEstimator estimator;
        for (unsigned i = 0; i < 50; ++i) {
            const unsigned concurrent = 1 + i % 4;
            estimator.record_rtt(Us{static_cast<Us::rep>(std::ceil(profile.rtt_us * jitter()))});
            const double duration = 1048576.0 * static_cast<double>(concurrent) * 1000000.0 /
                                    (profile.bandwidth * jitter());
            estimator.record_transfer(1048576, Us{static_cast<Us::rep>(std::ceil(duration))},
                                      concurrent);
        }
        SW_CHECK(estimator.rtt());
        SW_CHECK(estimator.bandwidth_bytes_per_sec());
        const double rtt_error =
            std::abs(static_cast<double>(estimator.rtt()->count()) / profile.rtt_us - 1.0);
        const double bandwidth_error =
            std::abs(*estimator.bandwidth_bytes_per_sec() / profile.bandwidth - 1.0);
        SW_CHECK(rtt_error < 0.20);
        SW_CHECK(bandwidth_error < 0.20);
        std::printf("%s: erro RTT %.2f%%, banda %.2f%%: ok\n", profile.name,
                    rtt_error * 100.0, bandwidth_error * 100.0);
    }
}

} // namespace

int main()
{
    std::puts("--- Estimador de fonte (F1-29) ---");
    test_empty();
    test_ignored();
    test_ewma();
    test_concurrent();
    test_estimate();
    test_extremes();
    test_profiles();
    return 0;
}
