#include "swarm_loader.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

static std::atomic<int> g_calls{0};
int SwarmFormatLoader::load_delay_ms = 0;
int SwarmFormatLoader::load_calls = 0;

void SwarmBlob::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_data"), &SwarmBlob::get_data);
    ClassDB::bind_method(D_METHOD("set_data", "data"), &SwarmBlob::set_data);
    ClassDB::bind_method(D_METHOD("get_source_path"), &SwarmBlob::get_source_path);
    ClassDB::bind_method(D_METHOD("set_source_path", "path"), &SwarmBlob::set_source_path);
    ClassDB::bind_method(D_METHOD("as_text"), &SwarmBlob::as_text);
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data"), "set_data", "get_data");
}

void SwarmFormatLoader::_bind_methods() {
    ClassDB::bind_static_method("SwarmFormatLoader", D_METHOD("set_load_delay_ms", "ms"), &SwarmFormatLoader::set_load_delay_ms);
    ClassDB::bind_static_method("SwarmFormatLoader", D_METHOD("get_load_calls"), &SwarmFormatLoader::get_load_calls);
    ClassDB::bind_static_method("SwarmFormatLoader", D_METHOD("reset_load_calls"), &SwarmFormatLoader::reset_load_calls);
}

static bool claims(const String &path) {
    return path.begins_with("swarm://") || path.begins_with("res://claimed/");
}

PackedStringArray SwarmFormatLoader::_get_recognized_extensions() const {
    PackedStringArray a;
    a.push_back("txt");
    a.push_back("bin");
    return a;
}

bool SwarmFormatLoader::_recognize_path(const String &path, const StringName &) const {
    return claims(path);
}

bool SwarmFormatLoader::_handles_type(const StringName &type) const {
    return type == StringName("SwarmBlob") || type == StringName("Resource");
}

String SwarmFormatLoader::_get_resource_type(const String &path) const {
    return claims(path) ? String("SwarmBlob") : String("");
}

bool SwarmFormatLoader::_exists(const String &path) const {
    return claims(path);
}

// Mock do sw-core: devolve bytes sintéticos derivados do caminho.
Variant SwarmFormatLoader::_load(const String &path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
    int n = ++g_calls;
    load_calls = n;
    OS *os = OS::get_singleton();
    bool main = os->get_thread_caller_id() == os->get_main_thread_id();
    UtilityFunctions::print("[SwarmFormatLoader] _load #", n, " path=", path, " sub_threads=", use_sub_threads,
                            " cache_mode=", cache_mode, " thread=", main ? "MAIN" : "WORKER");
    if (load_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(load_delay_ms));
    }
    Ref<SwarmBlob> blob;
    blob.instantiate();
    blob->set_source_path(path);
    blob->set_data(String("hello from swarm: " + path).to_utf8_buffer());
    return blob;
}

} // namespace godot
