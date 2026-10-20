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

**Estado: proposta técnica do R1, revisada pela sessão Pleno (aprovada com ajustes, já aplicados) e à espera do aceite do dono do produto. Vale para as medições finais do Portão 1.** A regra do portão está nos docs [16](16-briefing-fase-1a.md) e acima: p99 < 300 ms sobre os eventos individuais reunidos de pelo menos 3 rodadas completas e comparáveis. Esta seção define como calcular esse p99, quantas faltas são necessárias e quando rodadas podem ser reunidas. As contas são reproduzíveis com `python sw-godot/tools/p99_bounds.py`.

**1. Método do percentil: posto mais próximo (confirmado).** Ordenam-se as esperas das faltas concluídas e o percentil `p` é o valor de posto `k = ⌈p × N⌉` (contando a partir de 1). Para o p99: `k = ⌈0,99 × N⌉`. Fica este método, e não uma interpolação, por três motivos:
- o resultado é sempre uma espera que aconteceu, nunca um valor entre duas;
- é igual em qualquer linguagem e biblioteca: as interpolações têm pelo menos nove variantes, e cada ferramenta usa uma por padrão;
- não suaviza a cauda, que é o que o portão quer ver.

O `round_report.py` já implementa assim (`nearest_rank`).

**2. Mínimo: N ≥ 300 faltas concluídas, reunidas de pelo menos 3 rodadas, por perfil.** As contas não supõem nenhuma distribuição: o número de esperas abaixo do p99 verdadeiro segue uma Binomial(N; 0,99).

| N | posto do p99 | esperas acima do p99 | cota superior de 95% do p99 verdadeiro | p99 amostral abaixo do verdadeiro |
| --- | --- | --- | --- | --- |
| 100 | 99 | 1 | não existe | 74% das vezes |
| 200 | 198 | 2 | não existe | 68% |
| 299 | 297 | 2 | posto 299 (a maior espera) | 42% |
| **300** | **297** | **3** | posto 300 (a maior espera) | 65% |
| 500 | 495 | 5 | posto 499 | 62% |
| 1000 | 990 | 10 | posto 996 (percentil amostral 99,6) | 58% |
| 2000 | 1980 | 20 | posto 1988 (percentil amostral 99,4) | 56% |

Por que 300:
- **(a) É o menor N redondo em que o dado sustenta alguma afirmação sobre o p99.** Abaixo de 299 não existe cota superior de 95% para o p99 verdadeiro: nem a maior espera observada garante, com 95% de confiança, que ele está abaixo dela (a cota existe quando `1 − 0,99^N ≥ 0,95`, isto é, N ≥ 299).
- **(b) O p99 amostral é um estimador ruidoso da cauda.** Ele fica abaixo do p99 verdadeiro em 42% a 74% das vezes nos valores da tabela, perto da metade quando N cresce. Por isso o relatório mostra, ao lado do p99, a **cota superior de 95%** e o N.

**Regra de decisão proposta (opção A, recomendada):** o critério do portão é o p99 por posto mais próximo < 300 ms com N ≥ 300, como o dono decidiu. A cota de 95% é informativa. Quando o p99 passa e a cota não (cota ≥ 300 ms), o relatório escreve `passou (margem não demonstrada)`, e isso vai para o dono na reunião do portão.

**Opção B, mais estrita, para o dono escolher:** exigir também a cota de 95% < 300 ms. O custo está na tabela. Com N = 300, a cota de 95% é a própria maior espera (posto 300), enquanto o p99 tem 3 esperas acima dele. Uma única falta lenta decide a cota (opção B), mas não o p99 (opção A). A cota só sai da maior espera perto de N = 500 (posto 499) e fica no percentil amostral 99,6 com N = 1000 (posto 996). A opção B só faz sentido com N ≥ 1000, ou seja, mais rodadas.

**O mínimo vale por perfil**, sempre sobre as faltas reunidas das rodadas daquele perfil:
- no portão, só o perfil `cabo_50`;
- na comparação de perfis (`compare_profiles.py`), cada perfil comparado precisa de 300 faltas suas.

O p99 de uma rodada isolada não tem mínimo; é só informação.

**3. O que torna rodadas comparáveis.** Rodadas só são reunidas quando todas as condições abaixo valem e estão gravadas no arquivo da rodada.

**Ambiente idêntico** (objeto `environment` de cada rodada; qualquer diferença entre rodadas, ou campo ausente, torna a reunião `incompleto`):

| Campo | Valor exato | De onde sai |
| --- | --- | --- |
| `machine` | nome da máquina | já existe |
| `gpu` | `renderer` da rodada | já existe |
| `build` | `git rev-parse HEAD` | já existe |
| `build_clean` | `true`: `git status --porcelain --untracked-files=no` vazio antes de compilar | novo. Árvore suja ⇒ rodada não comparável |
| `route_sha256` | SHA-256 do `route.log` | já existe |
| `godot` | a linha impressa por `godot --version`, sem alteração (hoje `4.7.2.stable.official.ed1daf0bf`) | novo |
| `network_profile` | objeto `{"name": "cabo_50", "bandwidth_mbps": 50.0, "latency_ms": 25.0, "loss_percent": 0.2}`, com os números do `PROFILES` de `sw-netlab/server.py` efetivamente usados | novo. Só o nome não basta: uma mudança no `PROFILES` passaria despercebida |
| `assets` | SHA-256 dos bytes de `sw-godot/assets.lock.json` no commit `build` | novo |
| `cache_state` | `"core-only"`: antes da rodada o cache foi apagado e deixado só com o grupo `core`, mais `cache_bytes_at_start` (bytes do cache na partida) | novo. Outro valor ⇒ rodada não comparável |
| `cpu_busy_pct_30s` | média de uso total da CPU nos 30 s antes da rodada: 30 amostras de 1 s, ver o método abaixo | novo. **Limite: < 5%**; acima ⇒ rodada não comparável |

**Como medir `cpu_busy_pct_30s`.** O método não pode depender do idioma do Windows. O `typeperf` com o nome do contador em inglês (`\Processor(_Total)\% Processor Time`) **falha** num Windows em português: o nome do contador é traduzido. Por isso:
- **Windows (qualquer idioma), sem privilégio de administrador:** a classe CIM `Win32_PerfFormattedData_PerfOS_Processor`, instância `_Total`, campo `PercentProcessorTime`, amostrada 30 vezes, uma por segundo, gravando a média:
  ```powershell
  $v = 1..30 | ForEach-Object { (Get-CimInstance -ClassName Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime; Start-Sleep -Seconds 1 }
  ($v | Measure-Object -Average).Average
  ```
  Conferido em 2026-10-20 num Windows 11 em português: o `typeperf` com o contador em inglês falhou, e o CIM devolveu as amostras sem elevação de privilégio.
- **Linux:** a diferença de `/proc/stat` (linha `cpu`) entre o início e o fim dos 30 s: `100 × (1 − Δidle/Δtotal)`, com `idle` = campos `idle` + `iowait`.

O arquivo da rodada guarda a média e as 30 amostras.

**Mesmo procedimento:** uma rodada de aquecimento descartada no início de cada sessão de medição; a mesma duração de rota; a máquina na tomada e sem outras sessões, builds ou benchmarks rodando (regra do CLAUDE.md, verificada pelo `cpu_busy_pct_30s`).

**Rodada completa:** drenagem feita; nenhuma falta `failed`, `cancelled` ou `pending_at_end`; e `read_miss_start` com t0 na janela igual a `Δread_misses`, ou seja, sem perda de evento ([acima](#espera-do-jogo-numa-falta)).

**Não se reúnem rodadas de sessões de medição diferentes.** Uma sessão de medição é uma sequência contínua de rodadas na mesma máquina, sem reinício, com o mesmo build e a mesma GPU. Rodadas de dois dias, ou de duas sessões com o mesmo build, não são reunidas: o estado da máquina muda (cache do sistema, temperatura, atualizações), e o ambiente gravado não captura tudo isso.

Não proponho agora um teste estatístico de homogeneidade entre rodadas, como uma variação máxima do p50 entre elas. Ainda não há uma única medição real da espera, e um limite escolhido sem dados seria arbitrário. Ele entra, se fizer falta, depois das primeiras rodadas da F1-34.

**4. Regra de parada e o que fazer com menos amostras que o mínimo.**
- **Antes de medir**, declara-se no arquivo da sessão o número de rodadas `K`, com 3 ≤ `K` ≤ 10. **Todas as `K` rodadas completas entram**; não se escolhe nenhuma. Uma rodada incompleta é refeita dentro da mesma sessão, com o motivo registrado; ela nunca é simplesmente descartada.
- Se, depois das `K` rodadas, o perfil tiver N < 300, é permitida **uma** extensão: declara-se o novo total `K′` (com `K′` ≤ 10), antes de rodar as rodadas extras, e todas entram.
- Com `K` = 10 e N < 300, o p99 agregado é `incompleto por poucas faltas`, e a decisão vai para o dono. Poucas faltas é uma boa notícia para a taxa, mas não certifica a espera. A saída que proponho para esse caso, se o dono aceitar, é uma **rodada de faltas forçadas**: mesma rota, cache frio e prefetch desligado, só para medir a distribuição da espera no caminho da falta. Ela não entra na taxa de falta.
- Em qualquer caso, com N < 300 o p99 agregado é `incompleto`, nunca `passou`. Sem nenhuma falta concluída, ele é `n/d` com N = 0, nunca p99 = 0. Taxa de falta e p99 são critérios separados: uma rodada pode passar na taxa (< 1%) e o p99 agregado continuar `incompleto`.

**5. Limiares da comparação entre perfis (`compare_profiles.py`, F1-47).**

| Limiar | Proposta da F1-47 | R1 | Motivo |
| --- | --- | --- | --- |
| Razão da espera p50 (lento/rápido) | ≥ 2,0× | **confirmo** | A razão esperada só da rede é ≈ 4,66× ([protocolo](medicoes/protocolo-mudanca-conhecida.md)); a metade dela deixa folga para o prefetch esconder parte da espera e ainda detecta uma bancada que não acompanha a rede |
| Razão da carga (mediana de 3 rodadas) | ≥ 1,2× | **confirmo** | Entra como "e/ou" com a espera; na cena atual, a leitura é só uma parte da carga ([baseline.md](medicoes/baseline.md) §4) |
| Ruído tolerado (`--noise`) | 10% | **confirmo 10%**, com uma condição: o ruído medido no controle A–A′ | É o mesmo limite de validade da bancada no `baseline.py`. Os 20,4% de variação da carga no baseline são o sinal de bancada fora da validade, não um ruído a aceitar. Alargar o limite esconderia a melhora do perfil lento que o protocolo procura. **Regra:** se, no controle A–A′, alguma métrica comparada variar mais de 10% (`|A′/A − 1| > 10%`), a comparação é `incompleto` (bancada não validada) |
| Mínimo de amostras (`--min-samples`) | não definido | **300 por perfil** | O relatório compara também p95 e p99; com o mesmo mínimo do portão, o p99 de cada perfil tem a mesma garantia. Para o p50 bastariam 100 (intervalo de 95% entre os percentis 40 e 61) |
| Aviso de surpresa | razão > 3× a esperada | **confirmo** | Sinaliza sem reprovar |

Depois do aceite, a F1-33 (`round_report.py`, incluindo os campos novos de `environment`) e a F1-47 (`compare_profiles.py`, incluindo a regra do controle A–A′) passam a usar esses valores por padrão. Até lá, o mínimo é passado na linha de comando e registrado no relatório.

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
