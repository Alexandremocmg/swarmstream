// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ganchos de teste do StoreFiles (F1-10). Fica em src/, fora da API instalada: só os testes
// do sw-core incluem este arquivo.
#pragma once

#include "swarmstream/store_files.hpp"

#include <cstdint>

namespace swarmstream {

struct StoreFilesTestHook
{
    // O próximo sync (flush ou criação de arquivo) falha como se o disco tivesse devolvido EIO.
    static void fail_next_sync(StoreFiles& store) noexcept;
    // Quantas vezes a pasta foi sincronizada depois de criar ou completar um arquivo.
    static uint32_t directory_syncs(const StoreFiles& store) noexcept;
};

} // namespace swarmstream
