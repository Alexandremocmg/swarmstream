// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Gancho SÓ para os testes da F1-30: simula falha de escrita (disco cheio) no arquivo.
#pragma once

#include "swarmstream/telemetry.hpp"

namespace swarmstream {

class TelemetryLogTestHook
{
public:
    // O próximo fflush com eventos no buffer falha, como com disco cheio.
    static void fail_next_flush(TelemetryLog& log) noexcept;
};

} // namespace swarmstream
