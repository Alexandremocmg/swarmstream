#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include "swarm_loader.h"

using namespace godot;

static Ref<SwarmFormatLoader> g_loader;

static void initialize_swarm(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(SwarmBlob);
    GDREGISTER_CLASS(SwarmFormatLoader);
    g_loader.instantiate();
    ResourceLoader::get_singleton()->add_resource_format_loader(g_loader, true);
}

static void uninitialize_swarm(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    ResourceLoader::get_singleton()->remove_resource_format_loader(g_loader);
    g_loader.unref();
}

extern "C" {
GDExtensionBool GDE_EXPORT swarm_spike_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
    GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_swarm);
    init_obj.register_terminator(uninitialize_swarm);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
