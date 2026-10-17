// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// Garante que o swarmstream.h compila limpo como C++20 (é o que o
// sw-godot inclui) e que os tipos têm o tamanho que os bindings esperam.
#include "swarmstream.h"

#include <type_traits>

static_assert(std::is_standard_layout_v<sw_config>);
static_assert(std::is_trivially_copyable_v<sw_stats>);
static_assert(sizeof(sw_read_options) == 16);
static_assert(sizeof(void*) != 8 || sizeof(sw_config) == 128, "layout de sw_config (64 bits) mudou");

int api_v0_cxx_compiles()
{
    sw_config cfg{};
    sw_config_init(&cfg);
    return static_cast<int>(cfg.struct_size);
}
