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

**Regra do Portão 1** (decidida pelo dono do produto em 2026-10-19; critério em [16](16-briefing-fase-1a.md#critério-do-portão-1-definido-agora-para-ninguém-ajustar-depois)), no perfil `cabo_50`:
- **Taxa de falta: < 1% em cada rodada.** Uma rodada acima do limite reprova, mesmo que a média das rodadas fique abaixo.
- **Zero faltas em lógica e colisão.** Medido como zero faltas em arquivos do grupo `core`, onde por regra ficam lógica e colisão ([06](06-integracao-engine-sdk.md)). Como contar exatamente (por exemplo, pela flag do arquivo `core`) é parte do F1-48.
- **Espera p99 < 300 ms sobre os eventos individuais reunidos** de pelo menos **3 rodadas completas e comparáveis**: junta-se a espera de cada falta concluída de todas as rodadas e calcula-se um p99 só. **Não se usa a média dos p99 das rodadas.**
- **Ausência de faltas não é p99 = 0.** Com zero faltas, o p99 da rodada é `n/d`. Se o total reunido não atingir o mínimo de amostras, o p99 agregado é `incompleto`, nunca `passou`.
- **Método do percentil e mínimo de amostras:** hoje o relatório usa o posto mais próximo (abaixo). A confirmação do método, o número mínimo de faltas reunidas e o que torna rodadas comparáveis são a proposta técnica do R1 no cartão **F1-48**, que precisa estar aceita **antes das medições finais**.
- **Registro obrigatório:** por rodada, o número de leituras, de faltas e o p99; e o resultado agregado com o número de amostras.
- Stutter e carga seguem os limites já aprovados ([baseline.md](medicoes/baseline.md)).
- **Métricas automáticas não aprovam sozinhas o Portão 1:** a avaliação visual registrada de pop-in continua obrigatória.

**Taxa de falta (uma fonte só, com conferência)**
- Taxa = `Δread_misses / Δreads`, entre dois `sw_get_stats` (um no início e outro no fim da janela), porque os contadores de `sw_stats` são acumulados desde o `sw_init`.
- O número de eventos `read_miss_start` com t0 dentro da janela **tem de ser igual** a `Δread_misses`. Se diferir, houve perda de evento de telemetria e a rodada é **incompleta** (nenhuma falha de telemetria pode passar em silêncio).

**Falta que não termina bem, e a drenagem no fim da rodada**
- Ao fim da rota, o robô para de gerar leituras e **espera até 30 s** que as faltas pendentes terminem; só então chama `sw_shutdown`. Sem isso, o desligamento cancelaria o prefetch em voo e toda rodada sairia incompleta.
- Uma falta **não concluída** é a que não terminou na drenagem (`pending_at_end`) ou terminou em erro (`failed`: `SW_E_UNAVAILABLE`, `SW_E_CORRUPT`, `SW_E_IO`...). O robô da bancada não cancela leituras; se aparecer uma **cancelada** (`sw_cancel` ou `sw_close`; `cancelled`), a rodada também fica incompleta até o relatório explicar a origem. Sem essa regra, cancelar leituras lentas melhoraria o p99.
- Falta não concluída não entra nos percentis (não tem t1 de entrega), mas **não pode sumir**: a rodada fica **incompleta**, nunca "passou". O p99 completo exige que todas as faltas da janela tenham `outcome = completed`.

**Percentis**
- `miss_wait_p50_us`, `miss_wait_p95_us` e `miss_wait_p99_us` de `sw_stats` valem sobre os **últimos 60 s**, contados pelo instante t1 da falta. É o único instante em que a espera é conhecida; efeito aceito: uma falta presa não aparece no p99 enquanto não termina, então o overlay mostra `pending_requests` ao lado.
- Os relatórios da bancada (F1-33, F1-34) calculam os percentis sobre as faltas da **janela da medição** (por t0), a partir dos eventos, por rodada e juntando as rodadas, pelo método do posto mais próximo (índice `ceil(p × N)` das esperas ordenadas), com o mínimo de amostras e a cota superior de 95% definidos [abaixo](#método-do-percentil-amostras-e-rodadas-comparáveis-f1-48). O relatório informa `N` ao lado do percentil.

**Como medir (eventos)**

Campos em ASCII, `snake_case`, em inglês: os eventos são o protocolo aberto de telemetria ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)).

| Marca | Evento | Quando | Campos |
| --- | --- | --- | --- |
| Início (t0) | `read_miss_start` | quando a leitura é classificada como falta (com o `t0_us` da entrada da chamada) | `request_id`, `path`, `offset`, `size`, `deadline_ms`, `timeout_ms`, `t0_us` |
| Fim (t1) | `read_miss` | quando o núcleo pode entregar os bytes (passo 6), ou quando o pedido termina sem isso | `request_id`, `t1_us`, `miss_wait_us` (= `t1_us − t0_us`; só com `outcome = completed`), `call_result` (`SW_OK`, `SW_E_NOT_LOCAL` ou `SW_E_TIMEOUT`: o que o jogo recebeu), `outcome` (`completed`, `failed`, `cancelled` ou `pending_at_end`), `source` (`cdn` ou `peer`, da última peça a chegar), `error` (código `SW_E_*`, só com `outcome = failed`) |

- Os dois eventos se casam por `request_id`. O de `sw_read` vem do **mesmo contador** dos ids de `sw_read_async`, para nunca repetir na mesma rodada. Um `read_miss_start` sem `read_miss` no fim da drenagem recebe `outcome = pending_at_end` na gravação final; sem isso, o relatório trata como perda de evento.
- Emitir evento no caminho da leitura **não pode alocar nem bloquear** (regra do header). Requisito da F1-30: buffer circular pré-alocado, gravado por outra thread.

### Método do percentil, amostras e rodadas comparáveis (F1-48)

**Estado: proposta técnica do R1, aguardando revisão independente e o aceite do dono do produto. Vale para as medições finais do Portão 1.** A regra do portão está nos docs [16](16-briefing-fase-1a.md) e acima: p99 < 300 ms sobre os eventos individuais reunidos de pelo menos 3 rodadas completas e comparáveis. Esta seção define como calcular esse p99, quantas faltas são necessárias e quando rodadas podem ser reunidas. As contas são reproduzíveis com `python sw-godot/tools/p99_bounds.py`.

**1. Método do percentil: posto mais próximo (confirmado).** Ordenam-se as esperas das faltas concluídas e o percentil `p` é o valor de posto `k = ⌈p × N⌉` (contando a partir de 1). Para o p99: `k = ⌈0,99 × N⌉`. Fica este método, e não uma interpolação, por três motivos:
- o resultado é sempre uma espera que aconteceu, nunca um valor entre duas;
- é igual em qualquer linguagem e biblioteca: as interpolações têm pelo menos nove variantes, e cada ferramenta usa uma por padrão;
- não suaviza a cauda, que é o que o portão quer ver.

O `round_report.py` já implementa assim (`nearest_rank`).

**2. Mínimo: N ≥ 300 faltas concluídas, reunidas de pelo menos 3 rodadas.** As contas abaixo não supõem nenhuma distribuição: o número de esperas abaixo do p99 verdadeiro segue uma Binomial(N; 0,99).

| N | posto do p99 | esperas acima do p99 | cota superior de 95% do p99 verdadeiro |
| --- | --- | --- | --- |
| 100 | 99 | 1 | não existe |
| 200 | 198 | 2 | não existe |
| 299 | 297 | 2 | posto 299 (o máximo) |
| **300** | **297** | **3** | posto 300 (o máximo) |
| 1000 | 990 | 10 | posto 996 (percentil amostral 99,6) |
| 2000 | 1980 | 20 | posto 1988 (percentil amostral 99,4) |

Por que 300:
- **(a)** Abaixo de 299 não existe nenhuma cota superior de 95% para o p99: nem a maior espera observada garante, com 95% de confiança, que o p99 verdadeiro está abaixo dela. Com menos de 299 faltas, o dado não sustenta nenhuma afirmação sobre o p99.
- **(b)** Com 300, ficam 3 esperas acima do posto do p99, que deixa de ser a maior espera: um caso isolado (uma pausa do sistema operacional, por exemplo) não decide o portão sozinho.
- **(c)** Mesmo com N grande, o p99 amostral fica abaixo do verdadeiro em cerca de metade das vezes (de 54% a 65% nos valores da tabela, por `p99_bounds.py`). Por isso o relatório mostra, ao lado do p99, a **cota superior de 95%** e o N.

**Regra de decisão proposta (opção A, recomendada):** o critério do portão é o p99 por posto mais próximo < 300 ms com N ≥ 300, como o dono decidiu. A cota de 95% é informativa. Quando o p99 passa e a cota não (cota ≥ 300 ms), o relatório escreve `passou (margem não demonstrada)`, e isso vai para o dono na reunião do portão.

**Opção B, mais estrita, para o dono escolher:** exigir também a cota de 95% < 300 ms. O custo: com N = 300 a cota é a própria maior espera, então uma única falta lenta reprova. A opção B só faz sentido com N ≥ 1000, ou seja, mais rodadas.

**3. O que torna rodadas comparáveis.** Rodadas só são reunidas quando todas valem ao mesmo tempo:
- **Ambiente idêntico**, conferido pelo relatório (`environment`):
  - os campos que o `round_report.py` já confere (`machine`, `gpu`, `build` = `git rev-parse HEAD`, `route_sha256`);
  - mais a versão do Godot (`godot`), o perfil de rede e a sua configuração no `sw-netlab` (`network_profile`), e o hash do `assets.lock.json` (`assets`).

  Os campos novos entram no formato da rodada pela F1-33.
- **Mesmo procedimento:**
  - cache no mesmo estado inicial (só o grupo `core`, e o resto vazio, apagado antes de cada rodada);
  - uma rodada de aquecimento descartada por sessão de medição;
  - a mesma duração de rota;
  - a máquina na tomada;
  - sem outras sessões, builds ou benchmarks rodando, com a CPU registrada nos 30 s antes de cada rodada. A máquina parada é a regra do CLAUDE.md.
- **Rodada completa:** drenagem feita; nenhuma falta `failed`, `cancelled` ou `pending_at_end`; e `read_miss_start` com t0 na janela igual a `Δread_misses`, ou seja, sem perda de evento ([acima](#espera-do-jogo-numa-falta)).
- **Sem escolher rodadas:** o número de rodadas é declarado antes de medir, e **todas** as rodadas completas daquela sessão entram. Uma rodada incompleta é refeita, e o motivo é registrado; ela nunca é simplesmente descartada.

Não proponho agora um teste estatístico de homogeneidade entre rodadas, como uma variação máxima do p50 entre elas. Ainda não há uma única medição real da espera, e um limite escolhido sem dados seria arbitrário. Ele entra, se fizer falta, depois das primeiras rodadas da F1-34.

**4. Menos amostras que o mínimo.**
- Com N < 300, o p99 agregado é `incompleto`, nunca `passou`. Sem nenhuma falta concluída, ele é `n/d` com N = 0, nunca p99 = 0.
- Taxa de falta e p99 são critérios separados: uma rodada pode passar na taxa (< 1%) e o p99 agregado continuar `incompleto`.
- Para chegar a 300, acrescentam-se rodadas comparáveis, todas contadas, até o limite de **10 rodadas** por sessão de medição.
- Se mesmo assim N < 300, o resultado é `incompleto por poucas faltas`, e a decisão vai para o dono. Poucas faltas é uma boa notícia para a taxa, mas não certifica a espera.
- A saída que proponho para esse caso, se o dono aceitar, é uma **rodada de faltas forçadas**: mesma rota, cache frio e prefetch desligado, só para medir a distribuição da espera no caminho da falta. Ela não entra na taxa de falta.

**5. Limiares da comparação entre perfis (`compare_profiles.py`, F1-47).**

| Limiar | Proposta da F1-47 | R1 | Motivo |
| --- | --- | --- | --- |
| Razão da espera p50 (lento/rápido) | ≥ 2,0× | **confirmo** | A razão esperada só da rede é ≈ 4,66× ([protocolo](medicoes/protocolo-mudanca-conhecida.md)); a metade dela deixa folga para o prefetch esconder parte da espera e ainda detecta uma bancada que não acompanha a rede |
| Razão da carga (mediana de 3 rodadas) | ≥ 1,2× | **confirmo** | Entra como "e/ou" com a espera; na cena atual, a leitura é só uma parte da carga ([baseline.md](medicoes/baseline.md) §4) |
| Ruído tolerado (`--noise`) | 10% | **trocar por 20%** | A carga variou **20,4%** entre rodadas quentes no baseline (§2). Com 10%, uma diferença dentro do ruído medido daria `incoerente` falso |
| Mínimo de amostras (`--min-samples`) | não definido | **300 por perfil** | O relatório compara também p95 e p99; com o mesmo mínimo do portão, o p99 de cada perfil tem a mesma garantia. Para o p50 bastariam 100 (intervalo de 95% entre os percentis 40 e 61) |
| Aviso de surpresa | razão > 3× a esperada | **confirmo** | Sinaliza sem reprovar |

Depois do aceite, a F1-33 (`round_report.py`) e a F1-47 (`compare_profiles.py`) passam a usar esses valores por padrão. Até lá, o mínimo é passado na linha de comando e registrado no relatório.

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
