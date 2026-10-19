# Métricas e testes

"Medir antes de afirmar" é princípio do projeto. Toda fase tem métricas de saída ([09-roadmap-fases.md](09-roadmap-fases.md)).

## Métricas principais

| Métrica | Definição | Meta inicial (a validar) |
| --- | --- | --- |
| **Taxa de falta** | Leituras do jogo (`sw_read` e `sw_read_async`) que não estavam locais / total de leituras; é `Δread_misses / Δreads` entre dois `sw_get_stats` na janela da medição ([abaixo](#espera-do-jogo-numa-falta)). `sw_ensure_async` e dicas não contam como leitura | < 1% jogando; 0% em lógica/colisão |
| **Espera do jogo** p50/p95/p99 | **Espera numa falta**: do pedido de leitura até os dados ficarem locais, **mesmo quando o jogo não esperou** (`timeout_ms = 0` ou timeout). Definição exata e como medir: [abaixo](#espera-do-jogo-numa-falta) | p99 < 300 ms com rede de 50 Mbps |
| **Quadros com stutter** | Quadros > 33 ms atribuídos a I/O | Limites absolutos sobre o baseline com SSD local ([medicoes/baseline.md §3](medicoes/baseline.md)): > 33 ms ≤ 2 e > 100 ms = 0 por rodada de 60 s, p99 ≤ baseline + 2 ms |
| **Pop-in** | Tempo com mip/LOD rebaixado na tela | Medir; meta definida na Fase 1 |
| **% de bytes de peers** | Bytes de peers / bytes totais baixados | 60–80% em regime (proposta original) — **a provar** na Fase 3 |
| **Tempo até jogar** | Do clique em instalar até o primeiro controle | < 10 min com 100 Mbps para jogo de 150 GB |
| **Impacto no jogador** | Aumento de RTT com upload ligado | < 20 ms |
| **Peças inválidas** | Peças rejeitadas na verificação | Contar e alertar; 0 aceitas |

## Eventos de telemetria (cliente → coletor)

`read_miss_start` (`request_id`, `path`, `offset`, `size`, `deadline_ms`, `timeout_ms`, `t0_us`), `read_miss` (`request_id`, `t1_us`, `miss_wait_us`, `call_result`, `outcome`, `source`, `error`), `piece_done` (fonte, bytes, duração), `piece_invalid` (peer, motivo), `eviction` (peças, bytes), `qos_change` (limite novo, motivo), `frame_stutter` (duração, causa atribuída), `session` (modo, teto de cache, tipo de NAT).

`read_miss_start` e `read_miss` são o par que mede a **espera do jogo numa falta**; os campos exatos, em inglês e `snake_case` (protocolo aberto), estão na próxima seção. Leitura que acerta o cache não emite evento (só os contadores de `sw_stats`).

Inspirado na telemetria do porte de referência: FPS, decomposição dos piores quadros, esperas de I/O e "leituras lentas" com o nome do arquivo e se havia dica para ela.

### Espera do jogo numa falta

Definição igual à do [`swarmstream.h`](../sw-core/include/swarmstream.h) (comentário de `sw_stats`; decisão H13 em [revisoes/F0-revisao-adrs-0010-0016.md](revisoes/F0-revisao-adrs-0010-0016.md)). A F1-33 e a F1-34 usam esta definição para medir o Portão 1.

**Definição**
- **Falta:** leitura (`sw_read` ou `sw_read_async`) em que, no instante em que o pedido é aceito, pelo menos um byte do intervalo pedido não está local (nem na RAM nem no disco).
- **t0:** instante da **entrada da chamada** (`sw_read`/`sw_read_async`). O evento `read_miss_start` é emitido quando a leitura é classificada como falta, mas leva o `t0_us` da entrada.
- **t1:** instante em que o núcleo passa a poder entregar os bytes ao jogo, isto é, uma nova leitura do mesmo intervalo seria acerto. Hoje é o **passo 6, "Publicação"**, de [04 §3](04-cache-local.md#3-protocolo-de-escrita-acid-durabilidade-e-atomicidade), **depois dos dois `fdatasync`**: o custo dos flushes fica dentro da espera, de propósito. Se a F1-11 ou a F1-28 passarem a servir a falta da memória antes do `fsync`, t1 é esse instante, sem mudar a definição.
- **Espera numa falta = t1 − t0**, em microssegundos, no mesmo relógio monotônico do processo.

**O que não muda a medida**
- **`timeout_ms = 0`:** `sw_read` devolve `SW_E_NOT_LOCAL` na hora (a chamada dura quase nada), mas o pedido de rede foi feito e a espera continua até t1. O mesmo vale quando o timeout esgota (`SW_E_TIMEOUT`; "o pedido continua"). Não é o tempo que a chamada ficou bloqueada: este é outra medida (duração da chamada) e **não** é a métrica. Com `timeout_ms = 0` o jogo não trava; a espera mostra por quanto tempo ele ficou com o recurso rebaixado (mip ou LOD menor), que é a ponte para o **pop-in**.
- **Leitura que cai numa peça já pedida** (por exemplo, por prefetch, ainda em voo): é falta; para o jogo, os dados não estavam lá.
- **Leitura que cruza várias peças:** é uma falta; t1 é a chegada da última peça.
- **Várias leituras da mesma peça pendente:** cada leitura tem o seu t0 e conta uma vez; o pedido de rede é um só. Um cliente que fizesse polling a cada quadro inflaria a taxa de falta; o loader do [ADR-0016](adr/0016-ponto-de-intercepcao-godot.md) não faz polling (espera o callback do `sw_ensure_async`).
- **Acerto no cache (RAM ou disco):** não é falta e não entra nos percentis.
- **`sw_ensure_async` e dicas:** não são leitura; não geram falta.

**Janela da medição.** A janela é a **rota do robô com `SW_GAME_STATE_PLAYING`**: do início ao fim da rota, depois da carga inicial (o critério do Portão 1 é "jogando", [16](16-briefing-fase-1a.md)). A carga inicial é reportada à parte e **não entra** na taxa de falta nem no p99. Taxa de falta e p99 são calculados **por rodada**; o relatório traz também o valor das rodadas juntas.

> **Pendente do dono do produto:** no Portão 1, passam só se **todas** as rodadas cumprirem o critério, ou vale o valor das rodadas juntas? Isso toca o critério fixado no doc 16 e **não está decidido aqui**. Até a decisão, o relatório mostra os dois e só dá `passou` quando os dois critérios concordam.

**Taxa de falta (uma fonte só, com conferência)**
- Taxa = `Δread_misses / Δreads`, entre dois `sw_get_stats` (um no início e outro no fim da janela), porque os contadores de `sw_stats` são acumulados desde o `sw_init`.
- O número de eventos `read_miss_start` com t0 dentro da janela **tem de ser igual** a `Δread_misses`. Se diferir, houve perda de evento de telemetria e a rodada é **incompleta** (nenhuma falha de telemetria pode passar em silêncio).

**Falta que não termina bem, e a drenagem no fim da rodada**
- Ao fim da rota, o robô para de gerar leituras e **espera até 30 s** que as faltas pendentes terminem; só então chama `sw_shutdown`. Sem isso, o desligamento cancelaria o prefetch em voo e toda rodada sairia incompleta.
- Uma falta **não concluída** é a que não terminou na drenagem (`pending_at_end`) ou terminou em erro (`failed`: `SW_E_UNAVAILABLE`, `SW_E_CORRUPT`, `SW_E_IO`...). O robô da bancada não cancela leituras; se aparecer uma **cancelada** (`sw_cancel` ou `sw_close`; `cancelled`), a rodada também fica incompleta até o relatório explicar a origem. Sem essa regra, cancelar leituras lentas melhoraria o p99.
- Falta não concluída não entra nos percentis (não tem t1 de entrega), mas **não pode sumir**: a rodada fica **incompleta**, nunca "passou". O p99 completo exige que todas as faltas da janela tenham `outcome = completed`.

**Percentis**
- `miss_wait_p50_us`, `miss_wait_p95_us` e `miss_wait_p99_us` de `sw_stats` valem sobre os **últimos 60 s**, contados pelo instante t1 da falta. É o único instante em que a espera é conhecida; efeito aceito: uma falta presa não aparece no p99 enquanto não termina, então o overlay mostra `pending_requests` ao lado.
- Os relatórios da bancada (F1-33, F1-34) calculam os percentis sobre as faltas da **janela da medição** (por t0), a partir dos eventos, por rodada e juntando as rodadas, pelo método do posto mais próximo (índice `ceil(p × N)` das esperas ordenadas). Com menos de 100 faltas concluídas o p99 é só o máximo: o relatório informa `N` ao lado do percentil.

**Como medir (eventos)**

Campos em ASCII, `snake_case`, em inglês: os eventos são o protocolo aberto de telemetria ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)).

| Marca | Evento | Quando | Campos |
| --- | --- | --- | --- |
| Início (t0) | `read_miss_start` | quando a leitura é classificada como falta (com o `t0_us` da entrada da chamada) | `request_id`, `path`, `offset`, `size`, `deadline_ms`, `timeout_ms`, `t0_us` |
| Fim (t1) | `read_miss` | quando o núcleo pode entregar os bytes (passo 6), ou quando o pedido termina sem isso | `request_id`, `t1_us`, `miss_wait_us` (= `t1_us − t0_us`; só com `outcome = completed`), `call_result` (`SW_OK`, `SW_E_NOT_LOCAL` ou `SW_E_TIMEOUT`: o que o jogo recebeu), `outcome` (`completed`, `failed`, `cancelled` ou `pending_at_end`), `source` (`cdn` ou `peer`, da última peça a chegar), `error` (código `SW_E_*`, só com `outcome = failed`) |

- Os dois eventos se casam por `request_id`. O de `sw_read` vem do **mesmo contador** dos ids de `sw_read_async`, para nunca repetir na mesma rodada. Um `read_miss_start` sem `read_miss` no fim da drenagem recebe `outcome = pending_at_end` na gravação final; sem isso, o relatório trata como perda de evento.
- Emitir evento no caminho da leitura **não pode alocar nem bloquear** (regra do header). Requisito da F1-30: buffer circular pré-alocado, gravado por outra thread.

## Bancada de testes `sw-netlab`

| Componente | Função |
| --- | --- |
| Servidor CDN simulado | HTTP Range com latência, banda e perda configuráveis |
| Perfis de rede | `fibra_300`, `cabo_50`, `adsl_10`, `4g_instavel`, `cgnat` — via `tc netem` em Linux/containers |
| Peers sintéticos | N containers rodando o `sw-core` em modo headless, com mistura de seeders completos e jogadores |
| Jogador robô | Godot headless percorrendo uma rota gravada no mapa de teste, gerando leituras reais |
| Relatório | Script que junta os eventos e gera as métricas acima por rodada |

## Tipos de teste

| Tipo | Onde | Quem costuma escrever |
| --- | --- | --- |
| Unidade (chunker, Merkle, índice, agendador) | CI em todo PR | Todos; júnior e pleno principalmente |
| Propriedades / fuzzing (manifesto, mensagens de rede, journal) | CI noturno | Pleno/sênior |
| Recuperação de falhas (matar processo no meio da escrita, disco cheio) | CI noturno | Pleno |
| Integração (núcleo + CDN simulada) | CI | Pleno |
| Rodada de bancada (rota completa, perfis de rede) | Sob demanda e semanal | Sênior define, pleno/júnior executa |
| Teste com jogadores reais | Fase 5 | Produto + toda a equipe |

## Mapa de teste (PoC)
Projeto Godot com 2–5 GB de assets pesados de licença livre (ex.: Poly Haven, CC0), divididos em regiões, com uma rota de câmera gravada para repetir as rodadas de forma idêntica.
