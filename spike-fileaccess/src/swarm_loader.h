#pragma once
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

// Resultado de uma leitura "via sw-core" (mock): bytes + de onde vieram.
class SwarmBlob : public Resource {
    GDCLASS(SwarmBlob, Resource)
    PackedByteArray data;
    String source_path;
protected:
    static void _bind_methods();
public:
    void set_data(const PackedByteArray &p) { data = p; }
    PackedByteArray get_data() const { return data; }
    void set_source_path(const String &p) { source_path = p; }
    String get_source_path() const { return source_path; }
    String as_text() const { return data.get_string_from_utf8(); }
};

// Loader registrado NA FRENTE. Reivindica swarm:// e res://claimed/**.
class SwarmFormatLoader : public ResourceFormatLoader {
    GDCLASS(SwarmFormatLoader, ResourceFormatLoader)
    static int load_delay_ms;
    static int load_calls;
protected:
    static void _bind_methods();
public:
    PackedStringArray _get_recognized_extensions() const override;
    bool _recognize_path(const String &path, const StringName &type) const override;
    bool _handles_type(const StringName &type) const override;
    String _get_resource_type(const String &path) const override;
    bool _exists(const String &path) const override;
    Variant _load(const String &path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const override;

    static void set_load_delay_ms(int ms) { load_delay_ms = ms; }
    static int get_load_calls() { return load_calls; }
    static void reset_load_calls() { load_calls = 0; }
};

} // namespace godot
