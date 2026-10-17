/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alexandre Aparecido Rodrigues Peres
 *
 * swarmstream.h — API pública do sw-core (contrato v0).
 *
 * Estado: RASCUNHO PARA REVISÃO (F0-06). Depois de aprovado pelos dois
 * sêniores, fica congelado para a Fase 1; toda mudança passa a exigir ADR
 * (ver docs/adr/0014-contrato-swarmstream-h-v0.md).
 *
 * Regras gerais do contrato
 * -------------------------
 * - C99, sem dependência além de <stddef.h>, <stdint.h> e <string.h>.
 * - Handles opacos. Nenhuma struct da biblioteca é alocada pelo chamador,
 *   exceto as de configuração/saída, que começam por `struct_size`.
 * - `struct_size`: o chamador preenche com sizeof() da versão do header que
 *   ele compilou. Na entrada, a biblioteca lê só os campos que cabem nesse
 *   tamanho (o resto assume o padrão). Na saída, escreve só até esse tamanho.
 *   É assim que campos novos entram no fim das structs sem quebrar binários.
 * - Strings: UTF-8, terminadas em NUL. Caminhos virtuais seguem
 *   docs/03-formato-de-conteudo.md §6: relativos à raiz do jogo, NFC,
 *   separador '/', sem '/' inicial ou final, sem "." nem "..", no máximo
 *   1024 bytes, diferenciam maiúsculas de minúsculas.
 *   Ex.: res://textures/rocha_4k.png => "textures/rocha_4k.png".
 * - Toda função que pode falhar devolve sw_result. Erro é sempre < 0.
 *   Em erro, todo ponteiro de saída recebe NULL ou 0. Nenhuma falha é
 *   silenciosa.
 * - Threads: todas as funções podem ser chamadas de qualquer thread, ao
 *   mesmo tempo, exceto onde indicado (sw_shutdown, sw_close, sw_dir_*).
 * - Caminho quente: sw_read, sw_read_async e sw_query não alocam memória
 *   quando os dados estão no cache.
 */
#ifndef SWARMSTREAM_H
#define SWARMSTREAM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Exportação e convenção de chamada
 * ------------------------------------------------------------------------ */
#if defined(_WIN32)
#  define SW_CALL __cdecl
#  if defined(SW_STATIC)
#    define SW_API
#  elif defined(SW_BUILDING_LIBRARY)
#    define SW_API __declspec(dllexport)
#  else
#    define SW_API __declspec(dllimport)
#  endif
#else
#  define SW_CALL
#  if defined(SW_BUILDING_LIBRARY) && !defined(SW_STATIC)
#    define SW_API __attribute__((visibility("default")))
#  else
#    define SW_API
#  endif
#endif

#if defined(__cplusplus)
#  define SW_INLINE inline
#elif defined(_MSC_VER)
#  define SW_INLINE __inline
#else
#  define SW_INLINE inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Versão
 * ------------------------------------------------------------------------
 * MAJOR muda com mudança incompatível (exige ADR). MINOR muda com adição
 * compatível (função nova, campo novo no fim de struct, código de erro
 * novo; também exige ADR). v0 = MAJOR 0.
 */
#define SW_API_VERSION_MAJOR 0u
#define SW_API_VERSION_MINOR 0u
#define SW_API_VERSION ((SW_API_VERSION_MAJOR << 16) | SW_API_VERSION_MINOR)

/* Versão da biblioteca carregada (pode diferir do header compilado). */
SW_API uint32_t SW_CALL sw_api_version(void);

/* ------------------------------------------------------------------------
 * Resultados
 * ------------------------------------------------------------------------
 * 0 = sucesso; > 0 = sucesso com informação; < 0 = erro.
 * Valores nunca são reaproveitados. Código negativo desconhecido deve ser
 * tratado como erro genérico.
 */
typedef int32_t sw_result;

enum {
    SW_OK                    = 0,
    SW_DONE                  = 1,   /* fim de iteração (sw_dir_next) */

    SW_E_NOT_IMPLEMENTED     = -1,
    SW_E_INVALID_ARGUMENT    = -2,
    SW_E_VERSION_MISMATCH    = -3,  /* MAJOR do header != MAJOR da biblioteca */
    SW_E_OUT_OF_MEMORY       = -4,
    SW_E_NOT_FOUND           = -5,  /* caminho, grupo ou pedido desconhecido */
    SW_E_OUT_OF_RANGE        = -6,  /* offset além do fim do arquivo */
    SW_E_NOT_LOCAL           = -7,  /* timeout_ms = 0 e os dados não estão no cache */
    SW_E_TIMEOUT             = -8,  /* timeout_ms esgotado; o pedido continua */
    SW_E_CANCELLED           = -9,
    SW_E_BUSY                = -10, /* há handles abertos, ou chamada vinda de callback */
    SW_E_SHUTTING_DOWN       = -11,
    SW_E_IO                  = -12, /* erro de disco local */
    SW_E_DISK_FULL           = -13,
    SW_E_CORRUPT             = -14, /* dados falharam na verificação depois das novas tentativas */
    SW_E_UNAVAILABLE         = -15, /* nenhuma fonte (CDN ou peers) entregou */
    SW_E_MANIFEST_INVALID    = -16, /* formato, Verifier ou validação semântica (ADR-0012) */
    SW_E_SIGNATURE_INVALID   = -17, /* assinatura ausente, inválida ou de chave não confiável */
    SW_E_CACHE_LOCKED        = -18, /* outro processo é dono da pasta de cache (ADR-0011) */
    SW_E_INTERNAL            = -19,
    SW_E_MANIFEST_UNSUPPORTED = -20, /* min_reader maior que o suportado: atualizar o cliente (03 §8) */
    SW_E_MANIFEST_ROLLBACK   = -21  /* build_seq menor que o do manifesto instalado (03 §8) */
};

#define SW_SUCCEEDED(r) ((r) >= 0)
#define SW_FAILED(r)    ((r) < 0)

/* Nome do código ("SW_E_TIMEOUT"), ASCII, estático. Nunca devolve NULL:
 * código desconhecido devolve "SW_E_UNKNOWN". */
SW_API const char* SW_CALL sw_result_name(sw_result result);

/* ------------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------------ */
typedef struct sw_ctx  sw_ctx;   /* uma instância do núcleo; uma pasta de cache */
typedef struct sw_file sw_file;  /* arquivo virtual aberto */
typedef struct sw_dir  sw_dir;   /* iteração de diretório virtual */

typedef uint64_t sw_request_id;  /* 0 nunca é um id válido */

/* ------------------------------------------------------------------------
 * Log e alocação
 * ------------------------------------------------------------------------ */
typedef int32_t sw_log_level;
enum {
    SW_LOG_ERROR = 0,
    SW_LOG_WARN  = 1,
    SW_LOG_INFO  = 2,
    SW_LOG_DEBUG = 3,
    SW_LOG_TRACE = 4
};

/* Chamado de qualquer thread interna. `message` (UTF-8) só vale durante a
 * chamada. Não pode bloquear nem chamar funções sw_*. */
typedef void (SW_CALL *sw_log_fn)(void* user, sw_log_level level, const char* message);

/* Alocador opcional da engine. Os dois ponteiros juntos ou nenhum (NULL =
 * alocador do sistema). Precisa ser thread-safe. `alignment` é potência de 2.
 * `size` em free_fn é o mesmo pedido em alloc_fn. */
typedef struct sw_allocator {
    void* (SW_CALL *alloc_fn)(void* user, size_t size, size_t alignment);
    void  (SW_CALL *free_fn)(void* user, void* ptr, size_t size);
    void* user;
} sw_allocator;

/* ------------------------------------------------------------------------
 * Configuração e ciclo de vida
 * ------------------------------------------------------------------------ */
/* Não existe modo sem assinatura (ADR-0012): na Fase 1 e na bancada, use a
 * chave de teste de sw-pack/testdata/ em trusted_keys. */
enum {
    /* Começa com P2P desligado (só CDN). Pode mudar com sw_set_p2p_enabled. */
    SW_CONFIG_P2P_DISABLED = 1u << 0
};

typedef struct sw_config {
    uint32_t       struct_size;      /* = sizeof(sw_config) */
    uint32_t       api_version;      /* = SW_API_VERSION */
    uint32_t       flags;            /* SW_CONFIG_* */
    uint32_t       worker_threads;   /* 0 = automático */

    const char*    cache_dir;        /* obrigatório; o processo vira dono exclusivo (lock) */
    const char*    manifest_path;    /* obrigatório; manifest.bin local (03 §7, ADR-0012) */

    const uint8_t* trusted_keys;     /* trusted_key_count * 32 bytes: chaves públicas Ed25519 */
    uint32_t       trusted_key_count;
    uint32_t       reserved0;        /* 0 */

    uint64_t       disk_cache_capacity_bytes; /* teto do cache em disco; obrigatório, > 0 */
    uint64_t       ram_cache_bytes;           /* cache de blocos descomprimidos; 0 = padrão */

    const char*    cdn_base_url_override;     /* NULL = URLs do manifesto. Para a bancada. */
    const char*    telemetry_path;            /* NULL = sem arquivo de eventos (F1-30) */

    sw_log_fn      log_fn;           /* NULL = sem log */
    void*          log_user;
    sw_log_level   log_level;        /* máximo nível entregue a log_fn */
    uint32_t       reserved1;        /* 0 */

    sw_allocator   allocator;
} sw_config;

/* Preenche struct_size, api_version e os padrões. Use sempre antes de
 * preencher os campos. Inline de propósito: struct_size reflete o header
 * que o chamador compilou. */
static SW_INLINE void sw_config_init(sw_config* cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->api_version = SW_API_VERSION;
    cfg->log_level   = SW_LOG_WARN;
}

/* Cria o contexto: abre a pasta de cache (com lock exclusivo), lê e
 * verifica o manifesto (ordem do ADR-0012) e recupera o índice pelo
 * journal. Erros típicos: SW_E_VERSION_MISMATCH, SW_E_INVALID_ARGUMENT,
 * SW_E_CACHE_LOCKED, SW_E_MANIFEST_INVALID, SW_E_MANIFEST_UNSUPPORTED,
 * SW_E_MANIFEST_ROLLBACK, SW_E_SIGNATURE_INVALID, SW_E_IO. */
SW_API sw_result SW_CALL sw_init(const sw_config* cfg, sw_ctx** out_ctx);

/* Encerra o contexto e solta o lock da pasta de cache.
 * Pré-condições: todos os sw_file e sw_dir fechados (senão SW_E_BUSY e nada
 * muda); nenhuma outra chamada em curso com este ctx; não chamar de dentro
 * de callback (SW_E_BUSY). Pedidos de hint pendentes são descartados.
 * Depois de SW_OK, `ctx` é inválido. */
SW_API sw_result SW_CALL sw_shutdown(sw_ctx* ctx);

/* ------------------------------------------------------------------------
 * Arquivos e diretórios virtuais
 * ------------------------------------------------------------------------ */
enum {
    SW_FILE_CORE      = 1u << 0,  /* grupo core: sempre local */
    SW_FILE_GROUP     = 1u << 1,  /* pertence a um grupo de conteúdo */
    SW_FILE_OPTIONAL  = 1u << 2,
    SW_FILE_DIRECTORY = 1u << 3
};

typedef struct sw_file_info {
    uint32_t struct_size;      /* = sizeof(sw_file_info), preenchido pelo chamador */
    uint32_t flags;            /* SW_FILE_* */
    uint64_t size;             /* bytes descomprimidos; 0 para diretório */
    int64_t  build_time_unix;  /* created_at do manifesto, em segundos */
} sw_file_info;

/* Abre um arquivo do manifesto. Não baixa nada. SW_E_NOT_FOUND se o
 * caminho não existir no manifesto. */
SW_API sw_result SW_CALL sw_open(sw_ctx* ctx, const char* path, sw_file** out_file);

/* Fecha o arquivo. Cancela as leituras assíncronas pendentes dele e só
 * volta depois que todos os callbacks delas terminaram (cada um recebe
 * SW_E_CANCELLED ou o resultado, se já tinha concluído). Não chamar de
 * dentro de callback (SW_E_BUSY). Não usar `file` em outra thread durante
 * a chamada. Depois de SW_OK, `file` é inválido. */
SW_API sw_result SW_CALL sw_close(sw_file* file);

SW_API sw_result SW_CALL sw_file_size(const sw_file* file, uint64_t* out_size);

/* Informações de um caminho (arquivo ou diretório) sem abrir. */
SW_API sw_result SW_CALL sw_stat(sw_ctx* ctx, const char* path, sw_file_info* out_info);

typedef struct sw_dir_entry {
    uint32_t    struct_size;   /* = sizeof(sw_dir_entry), preenchido pelo chamador */
    uint32_t    flags;         /* SW_FILE_* */
    uint64_t    size;
    const char* name;          /* só o nome, sem caminho; vale até o próximo sw_dir_next/sw_dir_close */
} sw_dir_entry;

/* Itera um diretório virtual ("" = raiz). Ordem estável por nome (bytes
 * UTF-8). Um sw_dir é usado por uma thread por vez. */
SW_API sw_result SW_CALL sw_dir_open(sw_ctx* ctx, const char* path, sw_dir** out_dir);
/* SW_OK com uma entrada, ou SW_DONE quando acabou. */
SW_API sw_result SW_CALL sw_dir_next(sw_dir* dir, sw_dir_entry* out_entry);
SW_API sw_result SW_CALL sw_dir_close(sw_dir* dir);

/* ------------------------------------------------------------------------
 * Leitura
 * ------------------------------------------------------------------------
 * Toda leitura carrega um prazo (ADR-0003). O prazo diz ao agendador
 * quando os bytes serão necessários; o timeout diz quanto sw_read pode
 * bloquear. São coisas diferentes:
 *   - deadline_ms = 0: o jogo precisa agora (falta de cache).
 *   - timeout_ms = 0: não bloqueia. Se os dados não estão no cache, devolve
 *     SW_E_NOT_LOCAL na hora, sem copiar nada, mas o pedido de rede É FEITO
 *     com o prazo dado. É o caso da política de falta: usar o mip menor
 *     agora e trocar quando o maior chegar.
 *   - timeout esgotado: SW_E_TIMEOUT; o pedido continua e os dados entram
 *     no cache quando chegarem.
 * Leitura curta só no fim do arquivo: offset == tamanho devolve SW_OK com
 * 0 bytes; offset > tamanho devolve SW_E_OUT_OF_RANGE. Fora isso, a
 * leitura é tudo ou nada.
 */
#define SW_WAIT_FOREVER 0xFFFFFFFFu

typedef struct sw_read_options {
    uint32_t struct_size;   /* = sizeof(sw_read_options) */
    uint32_t deadline_ms;   /* relativo a agora */
    uint32_t timeout_ms;    /* só sw_read; ignorado em sw_read_async */
    uint32_t flags;         /* reservado no v0: 0 */
} sw_read_options;

/* Padrão quando `opts` é NULL: deadline_ms = 0, timeout_ms = SW_WAIT_FOREVER. */
static SW_INLINE void sw_read_options_init(sw_read_options* opts)
{
    if (opts == NULL) return;
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = (uint32_t)sizeof(*opts);
    opts->timeout_ms  = SW_WAIT_FOREVER;
}

/* Leitura bloqueante. Existe para engines que leem de forma síncrona;
 * chame FORA da thread de render. Não chamar de dentro de callback
 * (SW_E_BUSY). */
SW_API sw_result SW_CALL sw_read(sw_file* file, uint64_t offset, void* dst, uint64_t size,
                                 const sw_read_options* opts, uint64_t* out_bytes_read);

/* Callback de leitura assíncrona. Chamado exatamente uma vez por pedido
 * aceito, numa thread interna. Não pode bloquear. Pode chamar sw_read_async,
 * sw_query, sw_cancel e sw_hint_*; não pode chamar sw_read, sw_close nem
 * sw_shutdown. */
typedef void (SW_CALL *sw_read_cb)(void* user, sw_request_id id, sw_result result,
                                   uint64_t bytes_read);

/* Leitura não bloqueante. Se devolver SW_OK, `cb` será chamado exatamente
 * uma vez (com sucesso, erro ou SW_E_CANCELLED), possivelmente antes de
 * sw_read_async voltar; *out_id é escrito antes disso. Se devolver erro,
 * `cb` nunca é chamado. `dst` precisa continuar válido até o callback.
 * `out_id` pode ser NULL se o chamador não for cancelar. */
SW_API sw_result SW_CALL sw_read_async(sw_file* file, uint64_t offset, void* dst, uint64_t size,
                                       const sw_read_options* opts, sw_read_cb cb, void* user,
                                       sw_request_id* out_id);

/* Garante que o trecho fique no cache, sem copiar bytes. Mesmas regras de
 * sw_read_async (callback exatamente uma vez; bytes_read = bytes do trecho
 * que ficaram locais). Serve a loaders que trabalham com o recurso inteiro
 * (abordagem A do spike docs/spikes/F0-spike-fileaccess-godot.md): pedem o
 * arquivo com prazo e só leem quando o callback disser que está local. */
SW_API sw_result SW_CALL sw_ensure_async(sw_file* file, uint64_t offset, uint64_t size,
                                         const sw_read_options* opts, sw_read_cb cb, void* user,
                                         sw_request_id* out_id);

/* Pede o cancelamento (de sw_read_async ou sw_ensure_async).
 * SW_OK: o pedido estava pendente; o callback ainda
 * vem (com SW_E_CANCELLED, ou com o resultado se concluiu antes).
 * SW_E_NOT_FOUND: id desconhecido ou já concluído. */
SW_API sw_result SW_CALL sw_cancel(sw_ctx* ctx, sw_request_id id);

/* Onde estão os bytes do trecho, neste instante (pode mudar logo depois). */
typedef int32_t sw_residency;
enum {
    SW_RESIDENCY_LOCAL   = 0,  /* todo o trecho está no cache */
    SW_RESIDENCY_PENDING = 1,  /* falta algo, já pedido */
    SW_RESIDENCY_ABSENT  = 2   /* falta algo, não pedido */
};

SW_API sw_result SW_CALL sw_query(sw_file* file, uint64_t offset, uint64_t size,
                                  sw_residency* out_residency);

/* ------------------------------------------------------------------------
 * Dicas para o prefetch
 * ------------------------------------------------------------------------
 * Dicas são pedidos com prazo longo; nunca bloqueiam e nunca garantem
 * que os dados cheguem. Uma dica nova para o mesmo item substitui o prazo
 * anterior se for mais curta.
 */

/* Caminhos desconhecidos são ignorados; nesse caso devolve SW_E_NOT_FOUND,
 * mas os caminhos válidos foram registrados. */
SW_API sw_result SW_CALL sw_hint_files(sw_ctx* ctx, const char* const* paths, size_t count,
                                       uint32_t deadline_ms);

/* Grupo de conteúdo do manifesto ("regiao_norte"). */
SW_API sw_result SW_CALL sw_hint_group(sw_ctx* ctx, const char* group, uint32_t deadline_ms);

typedef struct sw_vec3 {
    float x, y, z;
} sw_vec3;

/* Posição e velocidade do jogador, nas unidades do mundo do jogo (as
 * mesmas dos traces gravados). Chamar algumas vezes por segundo basta.
 * `velocity` pode ser NULL. */
SW_API sw_result SW_CALL sw_hint_position(sw_ctx* ctx, const sw_vec3* position,
                                          const sw_vec3* velocity);

/* ------------------------------------------------------------------------
 * Estado do jogo e rede
 * ------------------------------------------------------------------------ */
typedef int32_t sw_game_state;
enum {
    SW_GAME_STATE_MENU         = 0,
    SW_GAME_STATE_LOADING      = 1,
    SW_GAME_STATE_PLAYING      = 2,
    SW_GAME_STATE_ONLINE_MATCH = 3   /* desliga o upload P2P enquanto durar */
};

SW_API sw_result SW_CALL sw_set_game_state(sw_ctx* ctx, sw_game_state state);

/* Liga ou desliga o P2P (download e upload). Desligado = só CDN. */
SW_API sw_result SW_CALL sw_set_p2p_enabled(sw_ctx* ctx, int32_t enabled);

/* Teto de upload em bytes/s. 0 = sem upload. UINT64_MAX = sem teto fixo
 * (a QoS automática continua valendo). */
SW_API sw_result SW_CALL sw_set_upload_limit(sw_ctx* ctx, uint64_t bytes_per_second);

/* ------------------------------------------------------------------------
 * Estatísticas
 * ------------------------------------------------------------------------
 * Contadores acumulados desde sw_init. Percentis de espera numa falta
 * calculados sobre os últimos 60 s. Definições em docs/08-metricas-e-testes.md.
 */
typedef struct sw_stats {
    uint32_t struct_size;            /* = sizeof(sw_stats), preenchido pelo chamador */
    uint32_t reserved0;

    uint64_t reads;                  /* leituras pedidas pelo jogo */
    uint64_t read_hits_ram;
    uint64_t read_hits_disk;
    uint64_t read_misses;

    uint64_t miss_wait_p50_us;
    uint64_t miss_wait_p95_us;
    uint64_t miss_wait_p99_us;

    uint64_t bytes_from_cdn;
    uint64_t bytes_from_peers;
    uint64_t bytes_uploaded;

    uint64_t pieces_invalid;         /* rejeitadas na verificação Merkle */
    uint64_t pieces_evicted;
    uint64_t bytes_evicted;

    uint64_t cache_used_bytes;
    uint64_t cache_capacity_bytes;
    uint64_t pending_requests;
} sw_stats;

SW_API sw_result SW_CALL sw_get_stats(sw_ctx* ctx, sw_stats* out_stats);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SWARMSTREAM_H */
