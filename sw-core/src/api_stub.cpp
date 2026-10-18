// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// Implementação provisória do swarmstream.h v0 (F0-06): toda função que
// depende de estado devolve SW_E_NOT_IMPLEMENTED e zera as saídas, como o
// contrato exige em erro. Será substituída módulo a módulo na Fase 1.

#ifndef SW_BUILDING_LIBRARY
#define SW_BUILDING_LIBRARY
#endif
#include "swarmstream.h"

namespace {

template <typename T>
void clear_out(T** out) noexcept
{
    if (out != nullptr) *out = nullptr;
}

void clear_out(uint64_t* out) noexcept
{
    if (out != nullptr) *out = 0;
}

// Zera uma struct de saída respeitando o struct_size do chamador.
template <typename T>
void clear_sized(T* out) noexcept
{
    if (out == nullptr || out->struct_size < sizeof(uint32_t)) return;
    const uint32_t caller_size = out->struct_size;
    const size_t n = caller_size < sizeof(T) ? caller_size : sizeof(T);
    auto* bytes = reinterpret_cast<unsigned char*>(out);
    memset(bytes + sizeof(uint32_t), 0, n - sizeof(uint32_t));
}

}  // namespace

uint32_t SW_CALL sw_api_version(void) { return SW_API_VERSION; }

const char* SW_CALL sw_result_name(sw_result result)
{
    switch (result) {
    case SW_OK:                  return "SW_OK";
    case SW_DONE:                return "SW_DONE";
    case SW_PARTIAL:             return "SW_PARTIAL";
    case SW_E_NOT_IMPLEMENTED:   return "SW_E_NOT_IMPLEMENTED";
    case SW_E_INVALID_ARGUMENT:  return "SW_E_INVALID_ARGUMENT";
    case SW_E_VERSION_MISMATCH:  return "SW_E_VERSION_MISMATCH";
    case SW_E_OUT_OF_MEMORY:     return "SW_E_OUT_OF_MEMORY";
    case SW_E_NOT_FOUND:         return "SW_E_NOT_FOUND";
    case SW_E_OUT_OF_RANGE:      return "SW_E_OUT_OF_RANGE";
    case SW_E_NOT_LOCAL:         return "SW_E_NOT_LOCAL";
    case SW_E_TIMEOUT:           return "SW_E_TIMEOUT";
    case SW_E_CANCELLED:         return "SW_E_CANCELLED";
    case SW_E_BUSY:              return "SW_E_BUSY";
    case SW_E_SHUTTING_DOWN:     return "SW_E_SHUTTING_DOWN";
    case SW_E_IO:                return "SW_E_IO";
    case SW_E_DISK_FULL:         return "SW_E_DISK_FULL";
    case SW_E_CORRUPT:           return "SW_E_CORRUPT";
    case SW_E_UNAVAILABLE:       return "SW_E_UNAVAILABLE";
    case SW_E_MANIFEST_INVALID:  return "SW_E_MANIFEST_INVALID";
    case SW_E_SIGNATURE_INVALID: return "SW_E_SIGNATURE_INVALID";
    case SW_E_CACHE_LOCKED:      return "SW_E_CACHE_LOCKED";
    case SW_E_INTERNAL:          return "SW_E_INTERNAL";
    case SW_E_MANIFEST_UNSUPPORTED: return "SW_E_MANIFEST_UNSUPPORTED";
    case SW_E_MANIFEST_ROLLBACK: return "SW_E_MANIFEST_ROLLBACK";
    case SW_E_QUEUE_FULL:        return "SW_E_QUEUE_FULL";
    default:                     return "SW_E_UNKNOWN";
    }
}

sw_result SW_CALL sw_init(const sw_config*, sw_ctx** out_ctx)
{
    clear_out(out_ctx);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_shutdown(sw_ctx*) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_open(sw_ctx*, const char*, sw_file** out_file)
{
    clear_out(out_file);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_close(sw_file*) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_file_size(const sw_file*, uint64_t* out_size)
{
    clear_out(out_size);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_stat(sw_ctx*, const char*, sw_file_info* out_info)
{
    clear_sized(out_info);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_dir_open(sw_ctx*, const char*, sw_dir** out_dir)
{
    clear_out(out_dir);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_dir_next(sw_dir*, sw_dir_entry* out_entry)
{
    clear_sized(out_entry);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_dir_close(sw_dir*) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_read(sw_file*, uint64_t, void*, uint64_t, const sw_read_options*,
                          uint64_t* out_bytes_read)
{
    clear_out(out_bytes_read);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_read_async(sw_file*, uint64_t, void*, uint64_t, const sw_read_options*,
                                sw_read_cb, void*, sw_request_id* out_id)
{
    clear_out(out_id);  // erro: o callback nunca é chamado
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_ensure_async(sw_file*, uint64_t, uint64_t, const sw_read_options*, sw_read_cb,
                                  void*, sw_request_id* out_id)
{
    clear_out(out_id);
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_cancel(sw_ctx*, sw_request_id) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_query(sw_file*, uint64_t, uint64_t, sw_residency* out_residency)
{
    if (out_residency != nullptr) *out_residency = SW_RESIDENCY_ABSENT;
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_hint_files(sw_ctx*, const char* const*, size_t, uint32_t)
{
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_hint_group(sw_ctx*, const char*, uint32_t) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_hint_position(sw_ctx*, const sw_position_hint*)
{
    return SW_E_NOT_IMPLEMENTED;
}

sw_result SW_CALL sw_set_game_state(sw_ctx*, sw_game_state) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_set_p2p_enabled(sw_ctx*, int32_t) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_set_upload_limit(sw_ctx*, uint64_t) { return SW_E_NOT_IMPLEMENTED; }

sw_result SW_CALL sw_get_stats(sw_ctx*, sw_stats* out_stats)
{
    clear_sized(out_stats);
    return SW_E_NOT_IMPLEMENTED;
}
