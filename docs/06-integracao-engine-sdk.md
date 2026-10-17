# Integração com a engine e SDK

Responsável pela camada 1. **É a parte de maior risco do projeto**: um sistema de streaming feito para SSD (latência < 1 ms) vai encontrar faltas de 50 ms a vários segundos.

## API do núcleo (rascunho do `swarmstream.h`)

> **Substituído.** O contrato v0 está em [`sw-core/include/swarmstream.h`](../sw-core/include/swarmstream.h) ([ADR-0014](adr/0014-contrato-swarmstream-h-v0.md)): leitura com prazo e limite de espera, `stat`/listagem, cancelamento, erros explícitos em todas as funções. O rascunho abaixo fica como registro.

```c
// ciclo de vida
sw_result sw_init(const sw_config* cfg, sw_ctx** out);
void      sw_shutdown(sw_ctx*);

// arquivos
sw_result sw_open(sw_ctx*, const char* path, sw_file** out);
sw_result sw_read(sw_file*, uint64_t offset, void* dst, uint64_t size, uint64_t* read);   // bloqueante
sw_result sw_read_async(sw_file*, uint64_t offset, void* dst, uint64_t size,
                        sw_priority prio, sw_read_cb cb, void* user);                     // não bloqueante
sw_status sw_query(sw_file*, uint64_t offset, uint64_t size);   // LOCAL | PENDENTE | AUSENTE
void      sw_close(sw_file*);

// dicas para o prefetch
void sw_hint_files(sw_ctx*, const char* const* paths, size_t n, uint32_t deadline_ms);
void sw_hint_group(sw_ctx*, const char* group, uint32_t deadline_ms);   // "regiao_norte"
void sw_hint_position(sw_ctx*, float x, float y, float z, float vx, float vy, float vz);

// estado do jogo (afeta QoS e prioridades)
void sw_set_game_state(sw_ctx*, sw_game_state state);   // MENU | LOADING | JOGANDO | PARTIDA_ONLINE

// telemetria
void sw_get_stats(sw_ctx*, sw_stats* out);
```

Regras da API:
- Thread-safe; nenhuma função de leitura aloca no caminho quente.
- `sw_read` bloqueante existe porque engines antigas leem de forma síncrona; o jogo deve chamá-lo **fora** da thread de render.
- Toda função retorna erro explícito. Nenhuma falha silenciosa.

## Política de falta

O jogo declara, por tipo de asset, o que fazer se o dado não estiver local:

| Tipo | Política recomendada |
| --- | --- |
| Textura | Usar mip menor já local; trocar quando o mip alto chegar. **Os mips baixos de todas as texturas ficam no grupo `core`.** |
| Malha | LOD mais baixo local; se nenhum, impostor ou esconder até chegar. |
| Áudio de ambiente | Pular ou começar atrasado. |
| Áudio de diálogo / cutscene | Esperar com indicador; segurar a cutscene. |
| Lógica, scripts, colisão | **Nunca em streaming**: sempre no `core`. |
| Nova região/fase | Tela de loading ou "porta lenta" até o grupo mínimo estar local. |

O `sw-pack` precisa saber disso para montar o `core`: **mips baixos, LODs baixos, colisão e lógica sempre locais**.

## Prefetch

Três fontes de informação, combinadas:

1. **Dicas explícitas do jogo** (`sw_hint_*`): o jogo sabe que o jogador entrou na zona de transição, que a próxima missão é X. Fonte mais confiável.
2. **Traces gravados**: em sessões de teste, gravamos quais peças foram lidas em que posição/momento. O `sw-pack` gera um índice espacial (grade ou octree) "posição → peças". Em runtime, `sw_hint_position` consulta esse índice.
3. **Heurística de movimento**: projeta a posição pela velocidade; a janela de tempo depende da banda medida (5–10 s em rede rápida, 30–60 s em rede lenta).

## SDK Godot (`sw-godot`) — primeira integração

- GDExtension em C++.
- Ponto de entrada: implementar `FileAccess` / `DirAccess` customizados para que `res://` (ou um prefixo próprio, ex.: `swarm://`) leia pelo `sw-core`.
- Integração com o carregamento em thread do Godot (`ResourceLoader.load_threaded_request`) para não travar a thread principal.
- Nós de ajuda: `SwarmHintArea3D` (ao entrar, chama `sw_hint_group`), `SwarmStats` (overlay de debug).
- Plugin de editor: marcar pastas/recursos como `core`, grupo ou opcional; exportar para o `sw-pack`.

## Outras engines (Fase 6)

| Engine | Ponto de integração provável |
| --- | --- |
| Unreal 5 | Camada de plataforma de arquivos (`IPlatformFile`) e/ou backend de IoStore. Avaliar sobreposição com o conteúdo sob demanda nativo da Epic. |
| Unity | `AssetBundle`/Addressables com provider customizado. |

## Questões em aberto
- No Godot, interceptar via `FileAccess` customizado ou via packs (`.pck`) virtuais? **Respondida pelo spike F0:** [spikes/F0-spike-fileaccess-godot.md](spikes/F0-spike-fileaccess-godot.md) (uma GDExtension não implementa `FileAccess`; o loader na frente cobre só o `ResourceLoader`). Decisão em [ADR-0016](adr/0016-ponto-de-intercepcao-godot.md).
- Como gerar mips/LODs baixos automaticamente no export, para estúdios que não os tenham?
