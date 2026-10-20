// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// SwarmLoader: ResourceFormatLoader registrado NA FRENTE (ADR-0016, decisão 1).
// Reivindica swarm://gate/** e materializa o recurso a partir de bytes do
// GateCore, sem FileAccess e sem delegar ao loader nativo do Godot.
//
// Regras do ADR-0016 que este código cumpre:
//  - o _load nunca espera a rede: lê com a semântica de timeout_ms = 0; se o
//    dado melhor não está local, entrega o nível do core (mip/LOD menor ou
//    áudio vazio) e pede o melhor com ensure_async;
//  - a troca acontece NO MESMO recurso que o ResourceLoader guardou, na thread
//    principal (call_deferred), quando o ensure termina.

#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

// Ponto de controle do portão para GDScript (métodos estáticos) e dono das
// trocas no lugar (as chamadas diferidas precisam de um objeto vivo).
class SwarmGate : public Object
{
    GDCLASS(SwarmGate, Object)

protected:
    static void _bind_methods();

public:
    static SwarmGate* singleton();
    static void create_singleton();
    static void destroy_singleton();

    // GDScript
    static int init(const String& pack_dir, int latency_ms);
    static void shutdown();
    static Array get_log();
    static void clear_log();
    static int pending_swaps();
    static Dictionary stats();
    static bool is_local(const String& name);

    // Uso interno
    static void log_event(const Dictionary& entry);
    static bool on_main_thread();
    static void pending_add(int delta);

    // Trocas no lugar: rodam na thread principal (chamadas por call_deferred).
    void apply_texture(const Ref<ImageTexture>& target, const Ref<Image>& image, const String& path,
                       int64_t ensure_id, int64_t t_request_us);
    void apply_mesh(const Ref<ArrayMesh>& target, const Array& arrays, const String& path,
                    int64_t ensure_id, int64_t t_request_us);
    void apply_audio(const Ref<AudioStreamOggVorbis>& target,
                     const Ref<AudioStreamOggVorbis>& loaded, const String& path, int64_t ensure_id,
                     int64_t t_request_us);
    void report_failure(const String& path, int code, int64_t ensure_id);
};

class SwarmLoader : public ResourceFormatLoader
{
    GDCLASS(SwarmLoader, ResourceFormatLoader)

protected:
    static void _bind_methods() {}

public:
    PackedStringArray _get_recognized_extensions() const override;
    bool _recognize_path(const String& path, const StringName& type) const override;
    bool _handles_type(const StringName& type) const override;
    String _get_resource_type(const String& path) const override;
    bool _exists(const String& path) const override;
    Variant _load(const String& path, const String& original_path, bool use_sub_threads,
                  int32_t cache_mode) const override;
};

} // namespace godot
