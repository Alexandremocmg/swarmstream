// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/crypto.hpp"

#include <atomic>
#include <sodium.h>

namespace swarmstream::pack {

namespace {

std::atomic<bool> g_forced_failure{false};

} // namespace

bool crypto_ready() noexcept
{
    // Inicialização thread-safe (C++11); sodium_init() devolve -1 se falhar, 1 se já iniciado.
    static const bool ready = sodium_init() >= 0;
    return ready && !g_forced_failure.load(std::memory_order_relaxed);
}

std::string_view crypto_version() noexcept { return sodium_version_string(); }

namespace testing {

void force_crypto_failure(bool enabled) noexcept
{
    g_forced_failure.store(enabled, std::memory_order_relaxed);
}

} // namespace testing

} // namespace swarmstream::pack
