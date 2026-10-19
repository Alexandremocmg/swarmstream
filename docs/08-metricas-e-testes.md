# Métricas e testes

"Medir antes de afirmar" é princípio do projeto. Toda fase tem métricas de saída ([09-roadmap-fases.md](09-roadmap-fases.md)).

## Métricas principais

| Métrica | Definição | Meta inicial (a validar) |
| --- | --- | --- |
| **Taxa de falta** | Leituras do jogo (`sw_read` e `sw_read_async`) que não estavam locais / total de leituras; é `read_misses / reads` de `sw_stats`. `sw_ensure_async` e dicas não contam como leitura | < 1% jogando; 0% em lógica/colisão |
| **Espera do jogo** p50/p95/p99 | **Espera numa falta**: do pedido de leitura até os dados ficarem locais, **mesmo quando o jogo não esperou** (`timeout_ms = 0` ou timeout). Definição exata e como medir: [abaixo](#espera-do-jogo-numa-falta) | p99 < 300 ms com rede de 50 Mbps |
| **Quadros com stutter** | Quadros > 33 ms atribuídos a I/O | Limites absolutos sobre o baseline com SSD local ([medicoes/baseline.md §3](medicoes/baseline.md)): > 33 ms ≤ 2 e > 100 ms = 0 por rodada de 60 s, p99 ≤ baseline + 2 ms |
| **Pop-in** | Tempo com mip/LOD rebaixado na tela | Medir; meta definida na Fase 1 |
| **% de bytes de peers** | Bytes de peers / bytes totais baixados | 60–80% em regime (proposta original) — **a provar** na Fase 3 |
| **Tempo até jogar** | Do clique em instalar até o primeiro controle | < 10 min com 100 Mbps para jogo de 150 GB |
| **Impacto no jogador** | Aumento de RTT com upload ligado | < 20 ms |
| **Peças inválidas** | Peças rejeitadas na verificação | Contar e alertar; 0 aceitas |

## Eventos de telemetria (cliente → coletor)

`read_miss_start` (id do pedido, arquivo, offset, tamanho, prazo, timeout, `t0_us`), `read_miss` (id do pedido, `t1_us`, espera numa falta, resultado da chamada, desfecho, fonte), `piece_done` (fonte, bytes, duração), `piece_invalid` (peer, motivo), `eviction` (peças, bytes), `qos_change` (limite novo, motivo), `frame_stutter` (duração, causa atribuída), `session` (modo, teto de cache, tipo de NAT).

`read_miss_start` e `read_miss` são o par que mede a **espera do jogo numa falta**; os campos exatos estão na próxima seção. Leitura que acerta o cache não emite evento (só os contadores de `sw_stats`).

Inspirado na telemetria do porte de referência: FPS, decomposição dos piores quadros, esperas de I/O e "leituras lentas" com o nome do arquivo e se havia dica para ela.

### Espera do jogo numa falta

Definição igual à do [`swarmstream.h`](../sw-core/include/swarmstream.h) (comentário de `sw_stats`; decisão H13 em [revisoes/F0-revisao-adrs-0010-0016.md](revisoes/F0-revisao-adrs-0010-0016.md)). A F1-33 e a F1-34 usam esta definição para medir o Portão 1.

**Definição**
- **Falta:** leitura (`sw_read` ou `sw_read_async`) em que, no instante em que o pedido é aceito, pelo menos um byte do intervalo pedido não está local (nem na RAM nem no disco).
- **t0:** instante em que a leitura é aceita (entrada de `sw_read`/`sw_read_async`).
- **t1:** instante em que o **último** byte do intervalo fica local: peça verificada (Merkle), gravada no cache e legível.
- **Espera numa falta = t1 − t0**, em microssegundos, no mesmo relógio monotônico do processo.

**O que não muda a medida**
- **`timeout_ms = 0`:** `sw_read` devolve `SW_E_NOT_LOCAL` na hora (a chamada dura quase nada), mas o pedido de rede foi feito e a espera continua até t1. O mesmo vale quando o timeout esgota (`SW_E_TIMEOUT`; "o pedido continua"). Não é o tempo que a chamada ficou bloqueada: este é outra medida (duração da chamada) e **não** é a métrica. Com `timeout_ms = 0` o jogo não trava; a espera mostra por quanto tempo ele ficou com o recurso rebaixado (mip ou LOD menor), que é a ponte para o **pop-in**.
- **Leitura que cruza várias peças:** é uma falta; t1 é a chegada da última peça.
- **Várias leituras da mesma peça pendente:** cada leitura tem o seu t0 e conta uma vez; o pedido de rede é um só.
- **Acerto no cache (RAM ou disco):** não é falta e não entra nos percentis.
- **`sw_ensure_async` e dicas:** não são leitura; não geram falta.

**Falta que nunca termina** (pedido cancelado por `sw_close`/`sw_shutdown`, erro definitivo): não tem t1, então não entra nos percentis, mas **não pode sumir**: conta como *falta não concluída*. Uma rodada com falta não concluída **não passa** no critério de p99, porque excluí-la melhoraria o p99.

**Percentis**
- `miss_wait_p50_us`, `miss_wait_p95_us` e `miss_wait_p99_us` de `sw_stats` valem sobre os **últimos 60 s**, contados pelo instante t1 da falta.
- Os relatórios da bancada (F1-33, F1-34) calculam os percentis sobre **a rodada inteira**, a partir dos eventos, pelo método do posto mais próximo (índice `ceil(p × N)` das esperas ordenadas). Com menos de 100 faltas concluídas o p99 é só o máximo: o relatório informa `N` ao lado do percentil.

**Como medir (eventos)**

| Marca | Evento | Quando | Campos |
| --- | --- | --- | --- |
| Início (t0) | `read_miss_start` | no instante em que a leitura é classificada como falta | `request_id`, arquivo, `offset`, `size`, `deadline_ms`, `timeout_ms`, `t0_us` |
| Fim (t1) | `read_miss` | quando o último byte fica local, ou quando o pedido é abandonado | `request_id`, `t1_us`, `miss_wait_us` (= `t1_us − t0_us`; vazio se abandonado), `resultado_da_chamada` (`SW_OK`, `SW_E_NOT_LOCAL` ou `SW_E_TIMEOUT`: o que o jogo recebeu), `desfecho` (`concluida` ou `abandonada`), `fonte` (`cdn` ou `peer`, da última peça a chegar) |

Os dois eventos se casam por `request_id` (o `out_id` de `sw_read_async`; em `sw_read` o núcleo gera um id interno). Um `read_miss_start` sem `read_miss` no fim da rodada é uma falta não concluída: o relatório marca a rodada como **incompleta**, nunca como "passou". A taxa de falta é `read_miss_start / reads` (o número de leituras vem de `sw_stats`).

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
