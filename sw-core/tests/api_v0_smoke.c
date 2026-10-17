/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alexandre Aparecido Rodrigues Peres
 *
 * Teste de fumaça do swarmstream.h v0 (F0-06), compilado como C.
 * Prova que o header compila em C, que toda função existe e linka, e que o
 * stub cumpre o contrato em erro (saídas zeradas). Também trava o layout das
 * structs de tamanho fixo: mudou o layout, este teste quebra, e a mudança
 * precisa de ADR (ADR-0014). */
#include "swarmstream.h"

#include <stdio.h>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: falhou: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

#define NI(expr) CHECK((expr) == SW_E_NOT_IMPLEMENTED)

/* Layout congelado das structs sem ponteiros (iguais em toda plataforma). */
_Static_assert(sizeof(sw_read_options) == 16, "layout de sw_read_options mudou");
_Static_assert(sizeof(sw_file_info) == 24, "layout de sw_file_info mudou");
_Static_assert(sizeof(sw_vec3) == 12, "layout de sw_vec3 mudou");
_Static_assert(sizeof(sw_stats) == 8 + 16 * 8, "layout de sw_stats mudou");
_Static_assert(sizeof(sw_result) == 4, "sw_result precisa ter 32 bits");
_Static_assert(sizeof(sw_request_id) == 8, "sw_request_id precisa ter 64 bits");

static void SW_CALL never_called(void* user, sw_request_id id, sw_result r, uint64_t n)
{
    (void)id; (void)r; (void)n;
    *(int*)user = 1;
}

int main(void)
{
    sw_config cfg;
    sw_read_options opts;
    sw_file_info info;
    sw_dir_entry entry;
    sw_stats stats;
    sw_ctx* ctx = (sw_ctx*)(uintptr_t)1;      /* valores lixo: o stub deve zerar */
    sw_file* file = (sw_file*)(uintptr_t)1;
    sw_dir* dir = (sw_dir*)(uintptr_t)1;
    uint64_t n = 42;
    sw_request_id id = 42;
    sw_residency res = SW_RESIDENCY_LOCAL;
    sw_vec3 pos = { 1.0f, 2.0f, 3.0f };
    const char* paths[1] = { "textures/rocha_4k.png" };
    int cb_called = 0;
    char buf[16];

    CHECK(sw_api_version() == SW_API_VERSION);
    CHECK(strcmp(sw_result_name(SW_E_TIMEOUT), "SW_E_TIMEOUT") == 0);
    CHECK(strcmp(sw_result_name(-9999), "SW_E_UNKNOWN") == 0);
    CHECK(strcmp(sw_result_name(SW_E_MANIFEST_ROLLBACK), "SW_E_MANIFEST_ROLLBACK") == 0);
    CHECK(SW_FAILED(SW_E_NOT_IMPLEMENTED) && SW_SUCCEEDED(SW_DONE));

    sw_config_init(&cfg);
    CHECK(cfg.struct_size == sizeof(cfg) && cfg.api_version == SW_API_VERSION);
    sw_read_options_init(&opts);
    CHECK(opts.deadline_ms == 0 && opts.timeout_ms == SW_WAIT_FOREVER);

    NI(sw_init(&cfg, &ctx));                 CHECK(ctx == NULL);
    ctx = (sw_ctx*)(uintptr_t)1;

    NI(sw_open(ctx, paths[0], &file));       CHECK(file == NULL);
    file = (sw_file*)(uintptr_t)1;
    NI(sw_file_size(file, &n));              CHECK(n == 0);

    info.struct_size = sizeof(info); info.size = 7;
    NI(sw_stat(ctx, paths[0], &info));
    CHECK(info.struct_size == sizeof(info) && info.size == 0);

    NI(sw_dir_open(ctx, "", &dir));          CHECK(dir == NULL);
    dir = (sw_dir*)(uintptr_t)1;
    entry.struct_size = sizeof(entry); entry.name = "x";
    NI(sw_dir_next(dir, &entry));            CHECK(entry.name == NULL);
    NI(sw_dir_close(dir));

    n = 42;
    NI(sw_read(file, 0, buf, sizeof(buf), &opts, &n));  CHECK(n == 0);
    NI(sw_read(file, 0, buf, sizeof(buf), NULL, NULL));
    NI(sw_read_async(file, 0, buf, sizeof(buf), &opts, never_called, &cb_called, &id));
    CHECK(id == 0 && cb_called == 0);
    id = 42;
    NI(sw_ensure_async(file, 0, sizeof(buf), NULL, never_called, &cb_called, &id));
    CHECK(id == 0 && cb_called == 0);
    NI(sw_cancel(ctx, 1));
    NI(sw_query(file, 0, sizeof(buf), &res)); CHECK(res == SW_RESIDENCY_ABSENT);

    NI(sw_hint_files(ctx, paths, 1, 30000));
    NI(sw_hint_group(ctx, "regiao_norte", 60000));
    NI(sw_hint_position(ctx, &pos, NULL));

    NI(sw_set_game_state(ctx, SW_GAME_STATE_ONLINE_MATCH));
    NI(sw_set_p2p_enabled(ctx, 0));
    NI(sw_set_upload_limit(ctx, UINT64_MAX));

    stats.struct_size = sizeof(stats); stats.reads = 9;
    NI(sw_get_stats(ctx, &stats));           CHECK(stats.reads == 0);

    NI(sw_close(file));
    NI(sw_shutdown(ctx));

    puts("api_v0_smoke: ok");
    return 0;
}
