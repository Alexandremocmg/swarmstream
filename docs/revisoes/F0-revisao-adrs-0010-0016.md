# Revisão F0: ADRs 0010–0016 e `swarmstream.h` v0

Itens 6 e 7 da [lista de fechamento da Fase 0](../15-portao-fase-0-e-plano-3-recursos.md#lista-para-fechar-a-fase-0--1-semana). Regra usada: [revisão com 3 pessoas](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas).

| Papel | Quem | Data |
| --- | --- | --- |
| Revisão | R1 (líder técnico) | 2026-10-17 |
| Leitura e comentários | Revisor independente: um agente Claude que não participou da escrita, só com acesso de leitura. **Não foi a pessoa R2 (pleno).** | 2026-10-17 |
| Aprovação | Dono do produto: aprovou os 7 ADRs com os ajustes e o congelamento do header, aceitando a leitura independente como a do "outro recurso" | 2026-10-17 |

R1 é autor de 0010–0015. Por isso a leitura independente é a revisão que conta para esses seis; para o 0016, escrito na trilha de engine, R1 é o revisor.

## Veredito por ADR

| ADR | Veredito | O que mudou |
| --- | --- | --- |
| [0010](../adr/0010-linguagem-dos-servicos-de-backend.md) Go | Aceitar com ajustes | Backend adiado para depois do Portão 2 (doc 15); casos-limite Ed25519 (Wycheproof) nos vetores; leitor Go do manifesto com checagem de limites própria se não houver `Verifier` |
| [0011](../adr/0011-tecnologia-do-launcher.md) Tauri | Aceitar com ajustes | Lock do SO (`LockFileEx`, `fcntl`/`flock`), nunca arquivo de PID; tratamento de `SW_E_CACHE_LOCKED` até a F4 |
| [0012](../adr/0012-serializacao-do-manifesto.md) FlatBuffers | Aceitar com ajustes | `format_version` só para o envelope, `min_reader` para o payload; páginas devolvidas após a verificação; critério de abertura inclui assinatura e hash; Ed25519 no `sw_init` desde a F1 |
| [0013](../adr/0013-hash-dos-blocos.md) SHA-256 | Aceitar com ajustes | XXH3 **por bloco**; SHA pós-descompressão declarado como não sendo controle de segurança; assinatura alinhada a 03 §7.1 |
| [0014](../adr/0014-contrato-swarmstream-h-v0.md) Contrato | Aceitar com ajustes | Header corrigido (abaixo); testes com `offsetof`; só 64 bits; `sw_allocator` e `sw_vec3` congelados para sempre |
| [0015](../adr/0015-politica-de-contribuicao-dco.md) DCO | Aceitar | Só esclarece bots e commits pela interface web |
| [0016](../adr/0016-ponto-de-intercepcao-godot.md) Godot | Aceitar com ajustes | Portão obrigatório de materialização no início do F1-20; critério do protótipo C por taxa de falta e pop-in; `sw_ensure_async` citado; troca no lugar |

Nenhum ADR foi devolvido: todos os achados tinham correção possível sem mudar a decisão.

## Achados do header e respostas

| # | Origem | Achado | Resposta |
| --- | --- | --- | --- |
| H1 | R1 | `SW_RESIDENCY_LOCAL = 0` + "saída zerada em erro" fazia quem ignora um erro achar que o dado é local | Aceito: `ABSENT = 0`; teste trava isso |
| H2 | R1 | `sw_hint_files` devolvia erro já tendo efeito parcial | Aceito: `SW_PARTIAL = 2` |
| H3 | R1 | Callback podia chamar `sw_read_async`, mas a lista não citava `sw_ensure_async` | Aceito |
| H4 | Leitor | `sw_ensure_async` não segurava os dados: o que chega entra "frio" e pode sair antes do `_load` ler | Aceito: lease até o `sw_close`, conta no teto, `SW_E_DISK_FULL` se não couber. Recusado: ensure por caminho no nível do contexto (o loader já abre o arquivo; seria estado a mais sem ganho medido) |
| H5 | Leitor | `struct_size` maior/menor e bits de `flags` desconhecidos sem regra: ignorar calado | Aceito: regras explícitas no topo do header |
| H6 | Leitor | Corrida entre `sw_close` e callbacks que encadeiam leituras no mesmo arquivo | Aceito: pedidos novos durante o close devolvem `SW_E_CANCELLED` na hora; o cancelamento não espera rede |
| H7 | Leitor | Pool fixo de pedidos sem erro de pool cheio | Aceito: `SW_E_QUEUE_FULL` e `sw_config.max_pending_requests` (ocupa o antigo `reserved0`; layout de 128 bytes mantido) |
| H8 | Leitor | Bindings C#/Mono podem bloquear no callback | Aceito: callbacks em threads de conclusão dedicadas, nunca na thread dona do cache nem dentro da função que criou o pedido |
| H9 | Leitor | Dica vencida sem semântica | Aceito: é descartada, nunca vira urgente, não prende nada |
| H10 | Leitor | Posição sem nível/cena | Aceito: `sw_position_hint` com `struct_size` e `space_id` |
| H11 | Leitor | Tempo de vida de strings, chaves e alocador | Aceito: `sw_init` copia; log e alocador valem até `sw_shutdown`; `trusted_key_count = 0` é erro |
| H12 | Leitor | Funções inline não existem para Rust e C# | Aceito em parte: regra "padrão = zero", com as duas exceções escritas no header. Notas de binding C# (`LPUTF8Str`, delegate mantido vivo) vão para a doc do binding, não para o header |
| H13 | Leitor | `sw_stats` sem dizer o que conta e como medir a espera com `timeout_ms = 0` | Aceito: definido no header |

Respostas do leitor à lista de verificação do ADR-0014: `out_id` é implementável sem corrida (id por `fetch_add`, escrito antes de entrar na fila MPSC); `deadline_ms` em 32 bits basta; `sw_close` bloqueante passa a ser aceitável no `_load` com H6.

## Pendências de outros donos (não bloqueiam o aceite)

| Doc | O que alinhar | Dono |
| --- | --- | --- |
| [03](../03-formato-de-conteudo.md) §7.1, §7.2, §8 | `format_version` só para o envelope; `min_reader` para o payload (ADR-0012, item 5) | Trilha de conteúdo (F0-08) |
| [04](../04-cache-local.md) §4 e §5 | `checksum_xxh3` por bloco, não por peça (ADR-0013) | Trilha de cache (T2) |
| [08](../08-metricas-e-testes.md) | Definição da espera numa falta igual à do header (H13) | R2 |

## Documentos corrigidos pelo ADR-0016 (item 7)
- [ADR-0001](../adr/0001-poc-godot-libtorrent.md): justificativa "`FileAccess` interceptável" corrigida; decisão mantida.
- [06](../06-integracao-engine-sdk.md): seção do SDK Godot reescrita (loader na frente, `_load` sem espera, materialização por buffer, troca no lugar, regra do `core`).
- [Backlog F1-20](../11-backlog-inicial.md), [roadmap](../09-roadmap-fases.md), [skills](../13-skills-claude.md) e risco R16 em [12](../12-riscos.md).

## Verificação do header
- `gcc 16` (MinGW) com `-Wall -Wextra -Wpedantic -Werror`: o header compila em C99; o teste de fumaça (C11) e o teste C++20 passam; o teste trava `sizeof` e `offsetof` das structs.
- **Achado de R1 durante a verificação:** os testes do contrato não estavam no CMake, então o CI não os rodava, e a "trava por teste" do ADR-0014 não existia na prática. Também faltava `SW_STATIC` público na biblioteca estática (no MSVC, quem chamasse `sw_*` ia procurar `dllimport` e falhar no link). Corrigido em `sw-core/CMakeLists.txt`: alvo `sw-core-api-v0-tests` com `-Werror`/`/WX` e `target_compile_definitions(sw-core PUBLIC SW_STATIC)`. Build do repositório (CMake + Ninja + GCC 16, Debug): `ctest` 2 de 2 passam. **MSVC ainda não verificado:** só o CI do Windows prova.
