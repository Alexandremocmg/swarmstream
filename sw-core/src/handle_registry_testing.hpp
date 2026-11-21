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
    [[nodiscard]] static constexpr std::uint32_t max_pins() noexcept
    {
        return PinCore::kClosing - 1;
    }
};

} // namespace swarmstream::detail::testing
