// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarm_loader.h"

#include "gate_core.h"
#include "gate_formats.h"

#include <atomic>
#include <cstring>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/ogg_packet_sequence.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <string>
#include <vector>

namespace godot {

namespace {

// Nada de godot::String global: ele seria construído quando o DLL carrega, antes
// da interface da GDExtension existir (erro 1114 do Windows na carga do DLL).
constexpr const char* kPrefix = "swarm://gate/";
constexpr int64_t kPrefixLen = 13;

SwarmGate* g_gate = nullptr;
std::mutex g_log_mu;
Array* g_log = nullptr; // criado com o singleton (Array não pode ser estático global)
std::atomic<int> g_pending{0};

int64_t now_us() { return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec()); }

std::string to_std(const String& s)
{
    CharString utf8 = s.utf8();
    return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

String name_of(const String& path) { return path.substr(kPrefixLen); }

// "rock_4k.swtex" -> "rock_4k.low.swtex": a versão do grupo core.
String low_of(const String& name)
{
    const int64_t dot = name.rfind(".");
    return name.substr(0, dot) + ".low" + name.substr(dot);
}

// Lê o arquivo inteiro com a semântica de timeout_ms = 0 (nunca espera).
sw_result read_all(const String& name, std::vector<uint8_t>& out)
{
    swgate::GateCore& core = swgate::GateCore::instance();
    uint64_t size = 0;
    sw_result r = core.file_size(to_std(name), &size);
    if (r != SW_OK) return r;
    out.resize(static_cast<size_t>(size));
    uint64_t got = 0;
    r = core.read(to_std(name), 0, out.data(), size, &got);
    if (r == SW_OK && got != size) return SW_E_IO;
    return r;
}

Ref<Image> make_image(const swgate::TexBlob& t)
{
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(t.size));
    std::memcpy(bytes.ptrw(), t.data, static_cast<size_t>(t.size));
    return Image::create_from_data(static_cast<int32_t>(t.width), static_cast<int32_t>(t.height),
                                   t.mipmaps != 0, static_cast<Image::Format>(t.format), bytes);
}

Array make_mesh_arrays(const swgate::MeshBlob& m)
{
    PackedVector3Array pos, nrm;
    PackedVector2Array uv;
    PackedInt32Array idx;
    pos.resize(m.vertices);
    nrm.resize(m.vertices);
    uv.resize(m.vertices);
    idx.resize(m.indices);
    for (uint32_t i = 0; i < m.vertices; ++i) {
        float p[3], n[3], t[2];
        std::memcpy(p, m.pos + 3 * i, sizeof p);
        std::memcpy(n, m.nrm + 3 * i, sizeof n);
        std::memcpy(t, m.uv + 2 * i, sizeof t);
        pos.set(i, Vector3(p[0], p[1], p[2]));
        nrm.set(i, Vector3(n[0], n[1], n[2]));
        uv.set(i, Vector2(t[0], t[1]));
    }
    for (uint32_t i = 0; i < m.indices; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, m.idx + i, 4);
        idx.set(i, static_cast<int32_t>(v));
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = pos;
    arrays[Mesh::ARRAY_NORMAL] = nrm;
    arrays[Mesh::ARRAY_TEX_UV] = uv;
    arrays[Mesh::ARRAY_INDEX] = idx;
    return arrays;
}

Dictionary entry(const String& event, const String& path)
{
    Dictionary d;
    d["event"] = event;
    d["path"] = path;
    d["thread"] = SwarmGate::on_main_thread() ? String("MAIN") : String("WORKER");
    return d;
}

} // namespace

// ------------------------------------------------------------------ SwarmGate

void SwarmGate::_bind_methods()
{
    ClassDB::bind_static_method("SwarmGate", D_METHOD("init", "pack_dir", "latency_ms"),
                                &SwarmGate::init);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("shutdown"), &SwarmGate::shutdown);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("get_log"), &SwarmGate::get_log);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("clear_log"), &SwarmGate::clear_log);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("pending_swaps"), &SwarmGate::pending_swaps);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("stats"), &SwarmGate::stats);
    ClassDB::bind_static_method("SwarmGate", D_METHOD("is_local", "name"), &SwarmGate::is_local);
}

SwarmGate* SwarmGate::singleton() { return g_gate; }

void SwarmGate::create_singleton()
{
    g_gate = memnew(SwarmGate);
    g_log = memnew(Array);
}

void SwarmGate::destroy_singleton()
{
    swgate::GateCore::instance().shutdown();
    if (g_log) {
        memdelete(g_log);
        g_log = nullptr;
    }
    if (g_gate) {
        memdelete(g_gate);
        g_gate = nullptr;
    }
}

int SwarmGate::init(const String& pack_dir, int latency_ms)
{
    g_pending.store(0);
    clear_log();
    return swgate::GateCore::instance().init(to_std(pack_dir), static_cast<uint32_t>(latency_ms));
}

void SwarmGate::shutdown() { swgate::GateCore::instance().shutdown(); }

Array SwarmGate::get_log()
{
    std::lock_guard<std::mutex> lock(g_log_mu);
    return g_log ? g_log->duplicate(true) : Array();
}

void SwarmGate::clear_log()
{
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (g_log) g_log->clear();
}

void SwarmGate::log_event(const Dictionary& e)
{
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (g_log) g_log->push_back(e);
}

int SwarmGate::pending_swaps() { return g_pending.load(); }

void SwarmGate::pending_add(int delta) { g_pending.fetch_add(delta); }

Dictionary SwarmGate::stats()
{
    swgate::GateStats s = swgate::GateCore::instance().stats();
    Dictionary d;
    d["reads"] = static_cast<int64_t>(s.reads);
    d["reads_not_local"] = static_cast<int64_t>(s.reads_not_local);
    d["ensures"] = static_cast<int64_t>(s.ensures);
    d["pieces_fetched"] = static_cast<int64_t>(s.pieces_fetched);
    return d;
}

bool SwarmGate::is_local(const String& name)
{
    return swgate::GateCore::instance().is_local(to_std(name));
}

bool SwarmGate::on_main_thread()
{
    OS* os = OS::get_singleton();
    return os->get_thread_caller_id() == os->get_main_thread_id();
}

void SwarmGate::apply_texture(const Ref<ImageTexture>& target, const Ref<Image>& image,
                              const String& path, int64_t ensure_id, int64_t t_request_us)
{
    Dictionary e = entry("swap", path);
    target->set_image(image); // mesmo objeto que o ResourceLoader guardou
    e["wait_us"] = now_us() - t_request_us;
    e["width"] = target->get_width();
    e["height"] = target->get_height();
    e["mipmaps"] = image->has_mipmaps();
    e["mip_count"] = image->get_mipmap_count();
    e["format"] = static_cast<int64_t>(image->get_format());
    e["instance_id"] = static_cast<int64_t>(target->get_instance_id());
    log_event(e);
    swgate::GateCore::instance().release(static_cast<uint64_t>(ensure_id));
    pending_add(-1);
}

void SwarmGate::apply_mesh(const Ref<ArrayMesh>& target, const Array& arrays, const String& path,
                           int64_t ensure_id, int64_t t_request_us)
{
    Dictionary e = entry("swap", path);
    target->clear_surfaces();
    target->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    e["wait_us"] = now_us() - t_request_us;
    e["vertices"] = PackedVector3Array(arrays[Mesh::ARRAY_VERTEX]).size();
    e["instance_id"] = static_cast<int64_t>(target->get_instance_id());
    log_event(e);
    swgate::GateCore::instance().release(static_cast<uint64_t>(ensure_id));
    pending_add(-1);
}

void SwarmGate::apply_audio(const Ref<AudioStreamOggVorbis>& target,
                            const Ref<AudioStreamOggVorbis>& loaded, const String& path,
                            int64_t ensure_id, int64_t t_request_us)
{
    Dictionary e = entry("swap", path);
    target->set_packet_sequence(loaded->get_packet_sequence());
    e["wait_us"] = now_us() - t_request_us;
    e["length_s"] = target->get_length();
    e["instance_id"] = static_cast<int64_t>(target->get_instance_id());
    log_event(e);
    swgate::GateCore::instance().release(static_cast<uint64_t>(ensure_id));
    pending_add(-1);
}

void SwarmGate::report_failure(const String& path, int code, int64_t ensure_id)
{
    Dictionary e = entry("swap_failed", path);
    e["error"] = String(sw_result_name(code));
    log_event(e);
    swgate::GateCore::instance().release(static_cast<uint64_t>(ensure_id));
    pending_add(-1);
}

// ------------------------------------------------------------------ SwarmLoader

PackedStringArray SwarmLoader::_get_recognized_extensions() const
{
    PackedStringArray a;
    a.push_back("swtex");
    a.push_back("swmesh");
    a.push_back("ogg");
    return a;
}

bool SwarmLoader::_recognize_path(const String& path, const StringName&) const
{
    return path.begins_with(kPrefix);
}

bool SwarmLoader::_handles_type(const StringName& type) const
{
    return type == StringName("ImageTexture") || type == StringName("Texture2D") ||
           type == StringName("ArrayMesh") || type == StringName("Mesh") ||
           type == StringName("AudioStreamOggVorbis") || type == StringName("AudioStream") ||
           type == StringName("Resource");
}

String SwarmLoader::_get_resource_type(const String& path) const
{
    if (!path.begins_with(kPrefix)) return "";
    if (path.ends_with(".swtex")) return "ImageTexture";
    if (path.ends_with(".swmesh")) return "ArrayMesh";
    if (path.ends_with(".ogg")) return "AudioStreamOggVorbis";
    return "";
}

bool SwarmLoader::_exists(const String& path) const
{
    uint64_t size = 0;
    return path.begins_with(kPrefix) &&
           swgate::GateCore::instance().file_size(to_std(name_of(path)), &size) == SW_OK;
}

Variant SwarmLoader::_load(const String& path, const String&, bool, int32_t) const
{
    const int64_t t0 = now_us();
    const String name = name_of(path);
    SwarmGate* gate = SwarmGate::singleton();
    if (!gate || !swgate::GateCore::instance().initialized()) return ERR_UNCONFIGURED;
    Dictionary e = entry("load", path);

    std::vector<uint8_t> best;
    const sw_result r_best = read_all(name, best);
    e["read_best"] = String(sw_result_name(r_best));
    if (r_best != SW_OK && r_best != SW_E_NOT_LOCAL) return ERR_FILE_CANT_READ;
    const bool local = r_best == SW_OK;

    Variant result;
    if (path.ends_with(".swtex")) {
        std::vector<uint8_t> bytes;
        if (!local && read_all(low_of(name), bytes) != SW_OK) return ERR_FILE_CANT_READ;
        swgate::TexBlob t;
        if (!swgate::parse_tex(local ? best : bytes, t)) return ERR_FILE_CORRUPT;
        Ref<Image> img = make_image(t);
        if (img.is_null() || img->is_empty()) return ERR_FILE_CORRUPT;
        Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
        if (tex.is_null()) return ERR_CANT_CREATE;
        e["level"] = local ? "best" : "core";
        e["width"] = tex->get_width();
        if (!local) {
            SwarmGate::pending_add(1);
            const int64_t t_req = now_us();
            const uint64_t id = swgate::GateCore::instance().ensure_async(
                to_std(name),
                [gate, tex, path, name, t_req](sw_result r, uint64_t, uint64_t ensure_id) {
                    const int64_t eid = static_cast<int64_t>(ensure_id);
                    std::vector<uint8_t> full;
                    swgate::TexBlob ft;
                    if (r != SW_OK || read_all(name, full) != SW_OK ||
                        !swgate::parse_tex(full, ft)) {
                        callable_mp(gate, &SwarmGate::report_failure)
                            .call_deferred(path, int(r), eid);
                        return;
                    }
                    Ref<Image> full_img = make_image(ft); // decodificação fora da thread principal
                    callable_mp(gate, &SwarmGate::apply_texture)
                        .call_deferred(tex, full_img, path, eid, t_req);
                });
            if (id == 0) SwarmGate::pending_add(-1);
        }
        result = tex;
    } else if (path.ends_with(".swmesh")) {
        std::vector<uint8_t> bytes;
        if (!local && read_all(low_of(name), bytes) != SW_OK) return ERR_FILE_CANT_READ;
        swgate::MeshBlob m;
        if (!swgate::parse_mesh(local ? best : bytes, m)) return ERR_FILE_CORRUPT;
        Ref<ArrayMesh> mesh;
        mesh.instantiate();
        mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, make_mesh_arrays(m));
        e["level"] = local ? "best" : "core";
        e["vertices"] = static_cast<int64_t>(m.vertices);
        if (!local) {
            SwarmGate::pending_add(1);
            const int64_t t_req = now_us();
            const uint64_t id = swgate::GateCore::instance().ensure_async(
                to_std(name),
                [gate, mesh, path, name, t_req](sw_result r, uint64_t, uint64_t ensure_id) {
                    const int64_t eid = static_cast<int64_t>(ensure_id);
                    std::vector<uint8_t> full;
                    swgate::MeshBlob fm;
                    if (r != SW_OK || read_all(name, full) != SW_OK ||
                        !swgate::parse_mesh(full, fm)) {
                        callable_mp(gate, &SwarmGate::report_failure)
                            .call_deferred(path, int(r), eid);
                        return;
                    }
                    Array arrays = make_mesh_arrays(fm);
                    callable_mp(gate, &SwarmGate::apply_mesh)
                        .call_deferred(mesh, arrays, path, eid, t_req);
                });
            if (id == 0) SwarmGate::pending_add(-1);
        }
        result = mesh;
    } else if (path.ends_with(".ogg")) {
        Ref<AudioStreamOggVorbis> stream;
        if (local) {
            PackedByteArray pba;
            pba.resize(static_cast<int64_t>(best.size()));
            std::memcpy(pba.ptrw(), best.data(), best.size());
            stream = AudioStreamOggVorbis::load_from_buffer(pba);
            if (stream.is_null()) return ERR_FILE_CORRUPT;
            e["level"] = "best";
        } else {
            // Política de falta do áudio de ambiente (06): começa atrasado.
            stream.instantiate();
            e["level"] = "empty";
            SwarmGate::pending_add(1);
            const int64_t t_req = now_us();
            const uint64_t id = swgate::GateCore::instance().ensure_async(
                to_std(name),
                [gate, stream, path, name, t_req](sw_result r, uint64_t, uint64_t ensure_id) {
                    const int64_t eid = static_cast<int64_t>(ensure_id);
                    std::vector<uint8_t> full;
                    if (r != SW_OK || read_all(name, full) != SW_OK) {
                        callable_mp(gate, &SwarmGate::report_failure)
                            .call_deferred(path, int(r), eid);
                        return;
                    }
                    PackedByteArray pba;
                    pba.resize(static_cast<int64_t>(full.size()));
                    std::memcpy(pba.ptrw(), full.data(), full.size());
                    Ref<AudioStreamOggVorbis> loaded = AudioStreamOggVorbis::load_from_buffer(pba);
                    if (loaded.is_null()) {
                        callable_mp(gate, &SwarmGate::report_failure)
                            .call_deferred(path, int(SW_E_CORRUPT), eid);
                        return;
                    }
                    callable_mp(gate, &SwarmGate::apply_audio)
                        .call_deferred(stream, loaded, path, eid, t_req);
                });
            if (id == 0) SwarmGate::pending_add(-1);
        }
        result = stream;
    } else {
        return ERR_FILE_UNRECOGNIZED;
    }
    e["dur_us"] = now_us() - t0;
    SwarmGate::log_event(e);
    return result;
}

} // namespace godot
