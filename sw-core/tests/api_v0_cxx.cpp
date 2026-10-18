// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// Garante que o swarmstream.h compila limpo como C++20 (é o que o
// sw-godot inclui) e que os tipos têm o tamanho que os bindings esperam.
#include "swarmstream.h"

#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<sw_config>);
static_assert(std::is_trivially_copyable_v<sw_stats>);
static_assert(sizeof(sw_read_options) == 16);
static_assert(sizeof(void*) == 8, "o v0 só suporta plataformas de 64 bits");
static_assert(sizeof(sw_config) == 128, "layout de sw_config mudou");
static_assert(offsetof(sw_config, cache_dir) == 16);
static_assert(offsetof(sw_config, max_pending_requests) == 44);
static_assert(offsetof(sw_config, disk_cache_capacity_bytes) == 48);
static_assert(offsetof(sw_config, log_fn) == 80);
static_assert(offsetof(sw_config, allocator) == 104);

int api_v0_cxx_compiles()
{
    sw_config cfg{};
    sw_config_init(&cfg);
    return static_cast<int>(cfg.struct_size);
}
