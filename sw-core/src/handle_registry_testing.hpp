// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Gancho SÓ para os testes do registro de handles (F1-67 parte B). Fora da API instalada.
#pragma once

#include "handle_registry.hpp"

#include <cstdint>

namespace swarmstream::detail::testing {

class PinCoreTestHook
{
public:
    // Põe a contagem de pins (sem o bit FECHANDO) num valor, para testar o limite sem 2^31 pins.
    static void set_pins(PinCore& core, std::uint32_t pins) noexcept
    {
        const std::uint32_t closing = core.word_.load() & PinCore::kClosing;
        core.word_.store(closing | pins);
    }
    // `fn` é chamada pelo unpin LENTO logo depois do decremento da contagem (nullptr desliga).
    // Global do processo: um teste por vez, trocada sem unpin em curso.
    static void set_after_slow_decrement(void (*fn)() noexcept) noexcept;
    // O mutex do objeto está livre? (try_lock; chame de uma thread que NÃO o segura.)
    [[nodiscard]] static bool mutex_is_free(PinCore& core) noexcept
    {
        if (!core.m_.try_lock()) return false;
        core.m_.unlock();
        return true;
    }
    [[nodiscard]] static constexpr std::uint32_t max_pins() noexcept
    {
        return PinCore::kClosing - 1;
    }
};

} // namespace swarmstream::detail::testing
