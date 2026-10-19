// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarm_loader.h"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static Ref<SwarmLoader> g_loader;

static void initialize_swarmstream(ModuleInitializationLevel level)
{
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(SwarmGate);
    GDREGISTER_CLASS(SwarmLoader);
    SwarmGate::create_singleton();
    g_loader.instantiate();
    // Na frente: tem precedência sobre os loaders nativos (ADR-0016, decisão 1).
    ResourceLoader::get_singleton()->add_resource_format_loader(g_loader, true);
}

static void uninitialize_swarmstream(ModuleInitializationLevel level)
{
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    ResourceLoader::get_singleton()->remove_resource_format_loader(g_loader);
    g_loader.unref();
    SwarmGate::destroy_singleton();
}

extern "C" {
GDExtensionBool GDE_EXPORT swarmstream_godot_init(
    GDExtensionInterfaceGetProcAddress get_proc_address, const GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization)
{
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_swarmstream);
    init.register_terminator(uninitialize_swarmstream);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
