# Roadmap por fases

Cada fase tem **objetivo, atividades por trilha, entregáveis e critério de saída**. Duas fases terminam em **portão de decisão** (seguir, ajustar ou parar), porque são os pontos em que a ideia pode se provar inviável e é barato descobrir isso cedo.

As durações supõem a equipe base de [10-equipe-e-niveis.md](10-equipe-e-niveis.md) (~6–7 pessoas) e são **estimativas iniciais**: revisar ao fim de cada fase. Os níveis (J = júnior, P = pleno, S = sênior, A = arquiteto/tech lead) indicam o nível mínimo recomendado para **liderar** a atividade; níveis abaixo podem participar em par.

> **Cronograma vigente (2026-10-17): equipe de 3 recursos.** As durações e a divisão da Fase 1 em F1a e F1b estão em [15-portao-fase-0-e-plano-3-recursos.md](15-portao-fase-0-e-plano-3-recursos.md). As durações abaixo são as do plano original para 7 pessoas.

## Visão geral

```
F0 Fundação ──► F1 Engine+Cache ──► ◆ Portão 1 ──► F2 Híbrido P2P ──► F3 Swarm em escala ──► ◆ Portão 2 ──► F4 MVP ──► F5 Piloto ──► F6 Expansão
  3 sem            8 sem                            8 sem                 6 sem                              22–24 sem   8–12 sem     contínuo
```

## Trilhas de trabalho

| Código | Trilha | Módulo principal |
| --- | --- | --- |
| **T1** | Formato de conteúdo e build | `sw-pack` |
| **T2** | Cache local e VFS | `sw-core` |
| **T3** | Integração com engine | `sw-godot` |
| **T4** | Rede e agendador | `sw-core` |
| **T5** | Serviços de backend e infra | `sw-services` |
| **T6** | Bancada, telemetria e qualidade | `sw-netlab` |
| **T7** | Launcher e experiência do jogador | `sw-launcher` |
| **T8** | Segurança e privacidade | transversal |

As trilhas só se tocam pelos **contratos** de [02-arquitetura.md](02-arquitetura.md#contratos-entre-módulos), o que permite trabalho em paralelo.

---

## F0 — Fundação (≈ 3 semanas)

**Objetivo:** todos conseguem compilar, testar e trabalhar em paralelo sem pisar uns nos outros.

| Atividade | Trilha | Nível |
| --- | --- | --- |
| Criar repositório (monorepo), estrutura de pastas, CMake + vcpkg, CI (Windows + Linux) | T6 | P |
| Padrões de código (clang-format, clang-tidy), modelo de PR, regras de revisão | T6 | P |
| Escrever os ADRs pendentes (backend, launcher, manifesto, hash) e preparar `LICENSE` (Apache-2.0), `NOTICE` e `SECURITY.md`; ADRs 0001–0008 já aceitos | — | A |
| Escrever o header `swarmstream.h` v0 e as interfaces `IContentSource` / `ICacheStore` | T2/T4 | A + S |
| Especificação v0 do manifesto e do layout de bloco/peça | T1 | S |
| Montar o projeto Godot de teste com 2–5 GB de assets CC0 divididos em regiões | T3 | J (com P revisando) |
| Gravar uma rota de câmera repetível no mapa de teste | T3 | J |
| Servidor HTTP Range com latência/banda configuráveis (primeira versão do `sw-netlab`) | T6 | J/P |
| Baseline: medir FPS, stutter e tempo de carga do mapa lendo direto do SSD | T6 | P |

**Entregáveis:** repositório com CI verde, contratos v0 revisados, mapa de teste, servidor de rede simulada, números do baseline.
**Critério de saída:** um desenvolvedor novo clona, compila e roda o mapa de teste em menos de 1 hora seguindo o [CONTRIBUTING](../CONTRIBUTING.md).

---

## F1 — Engine + cache + rede simulada, sem P2P (≈ 8 semanas)

**Objetivo:** provar que um jogo roda bem lendo só do cache, com faltas atendidas por uma "CDN" lenta. **É a validação do maior risco.**

| Atividade | Trilha | Nível |
| --- | --- | --- |
| `sw-pack` v1: chunking FastCDC, compressão Zstd/LZ4 por bloco, manifesto assinado com chave de teste (ADR-0012) | T1 | S (P implementa partes) |
| Classificação `core` / grupo / opcional a partir de pastas do projeto Godot | T1 | P |
| Gerar mips/LODs baixos no `core` | T1/T3 | P |
| Cache em disco: slots, índice, journal, recuperação após queda | T2 | S |
| Leases e thread dona do cache; fila MPSC de pedidos | T2 | S |
| Remoção CLOCK com marcas d'água; conjunto fixo | T2 | P |
| Cache de blocos descomprimidos em RAM, pool de buffers | T2 | P |
| VFS: caminho + offset → blocos | T2 | P |
| Fonte "CDN" HTTP Range implementando `IContentSource` | T4 | P |
| Agendador por prazo v1 (uma fonte só, mas já com fila por prazo) | T4 | S |
| GDExtension: `ResourceFormatLoader` na frente lendo pelo `sw-core` (ADR-0016) | T3 | S |
| Política de falta no Godot: textura com mip menor, malha com LOD menor | T3 | S/P |
| Prefetch v1: `sw_hint_group` em áreas do mapa (`SwarmHintArea3D`) | T3 | P |
| Gravação de traces de leitura durante a rota | T3/T6 | J/P |
| Prefetch v2: índice espacial gerado dos traces | T1/T3 | S |
| Overlay de debug no jogo (acertos, faltas, fila, bytes) | T3 | J |
| Testes unitários do chunker, índice, journal, remoção | T1/T2 | J/P |
| Teste de queda: matar o processo durante escrita, disco cheio | T6 | P |
| Rodadas da bancada nos perfis `fibra_300`, `cabo_50`, `adsl_10` e relatório | T6 | P (J executa) |

**Entregáveis:** demo do mapa de teste em modo compacto, relatório de métricas por perfil de rede, medições de tamanho de bloco (64/128/256 KB).

### ◆ Portão 1 — a ideia funciona no lado do jogo?
Seguir se, em `cabo_50`: taxa de falta < 1%, p99 de espera < 300 ms, stutter dentro dos limites absolutos do baseline ([medicoes/baseline.md §3](medicoes/baseline.md): > 33 ms ≤ 2 e > 100 ms = 0 por rodada, p99 ≤ baseline + 2 ms), pop-in aceitável na avaliação visual.
Se não: ajustar prefetch/`core` (mais 4 semanas) ou **repensar o produto** (ex.: focar só em instalação e patches via P2P, sem streaming em runtime).

---

## F2 — Híbrido P2P + CDN (≈ 8 semanas)

**Objetivo:** peers servem parte do tráfego sem piorar nenhuma métrica da F1.

| Atividade | Trilha | Nível |
| --- | --- | --- |
| Integrar libtorrent 2.x ao build | T4 | P |
| Storage customizado do libtorrent gravando nos slots do cache | T4/T2 | S |
| `sw-pack` gera torrent v2 por pacote com web seed | T1 | P |
| Fonte P2P implementando `IContentSource` com `set_piece_deadline` | T4 | S |
| Agendador v2: escolha de fonte por folga, *hedging* com a CDN | T4 | S/A |
| Penalizar peers que entregam dados inválidos | T4/T8 | P |
| Tracker de teste (open source) e STUN (coturn) na bancada | T5 | P |
| Peers sintéticos headless em containers | T6 | P |
| Assinatura Ed25519 do manifesto e verificação no cliente | T8/T1 | S |
| Fuzzing do parser do manifesto | T8 | P |
| QoS v1: limite de upload fixo e por estado do jogo | T4 | P |
| Telemetria: eventos `piece_done`, `piece_invalid`, fonte de cada byte | T6 | J/P |

**Entregáveis:** demo com 1 jogador + 5–10 peers; relatório "% de bytes de peers" e comparação das métricas da F1.
**Critério de saída:** métricas da F1 mantidas; algum tráfego de peers em todos os perfis; nenhuma peça inválida aceita (teste com peer malicioso).

---

## F3 — Swarm em escala simulada (≈ 6 semanas)

**Objetivo:** medir o que a proposta promete (60–80% de tráfego pelo swarm) em condições realistas.

| Atividade | Trilha | Nível |
| --- | --- | --- |
| 50–200 peers em containers com mistura de perfis de rede e NAT | T6 | S |
| Simular dia de lançamento (cold-start) e dia de patch | T6 | P |
| Localidade no tracker (LAN → ASN → região) | T5 | P |
| Medir sucesso de hole punching por tipo de NAT; UPnP/NAT-PMP | T4 | S |
| QoS v2: reduzir upload quando o RTT sobe | T4 | P |
| Modelo de custo: CDN vs. P2P por cenário (planilha com os números medidos) | — | A + produto |
| Relatório técnico consolidado F1–F3 | — | A |

**Entregáveis:** relatório com curva de % de peers por dia simulado, custo estimado por GB, taxa de conexão direta por NAT.

### ◆ Portão 2 — vale construir o produto?
Seguir se o modelo de custo + retenção justifica o MVP para pelo menos um perfil de cliente (indie, AA, publisher). É também o momento de mostrar a investidores ou ao primeiro parceiro.

---

## F4 — MVP (≈ 22–24 semanas)

**Objetivo:** um jogo de terceiro pode ser distribuído e jogado com a plataforma, no Windows.

| Atividade | Trilha | Nível |
| --- | --- | --- |
| Launcher: instalar, modos compacto/completo/personalizado, teto do cache, configurações de upload | T7 | P (J nas telas) |
| Atualização por patch: diferença de blocos entre manifestos | T1/T2 | S |
| "Peças estáveis" entre versões | T1 | S |
| Manifest service, tracker próprio, coletor de telemetria, deploy em nuvem | T5 | S/P |
| Backend multi-cliente: contas de estúdio, isolamento entre clientes, medição de uso e cobrança ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)) | T5 | S |
| Painel do estúdio, monitoramento, alertas e plantão | T5/T6 | S/P |
| Documentação pública dos protocolos (manifesto, tracker, telemetria) com versionamento | T5/T1 | S/P |
| Painel de telemetria (dashboards) | T6/T5 | J/P |
| Plugin de editor Godot para marcar `core`/grupos e exportar | T3 | P |
| Assinatura de código, instalador, regras de firewall | T7/T8 | P |
| Detecção de conexão com franquia; upload em horário ocioso | T4/T7 | P |
| Cifra opcional por pacote + interface com serviço de licença | T8 | S |
| Documentação do SDK para estúdios e exemplo completo | T3 | P/J |
| Linux | T6 | P |
| Teste de longa duração (sessões de 4 h+, cache cheio, muitos patches) | T6 | P |

**Critério de saída:** um estúdio externo integra o SDK seguindo só a documentação; nenhum bug crítico aberto.

---

## F5 — Piloto (≈ 8–12 semanas)

**Cliente-piloto decidido:** estúdio indie com jogo grande em Godot. Procurar candidatos durante as Fases 0 a 2 e fechar o acordo (escopo, métricas que serão compartilhadas, consentimento dos jogadores) até o fim da Fase 3.

- Um jogo real (preferência: estúdio parceiro com jogo grande em Godot) com jogadores reais, opt-in.
- Revisão jurídica: privacidade, termos, consentimento de upload.
- Plantão e processo de incidentes.
- Medir: retenção (desinstalações), tempo até jogar, % de peers, reclamações de conexão.
- Saída: estudo de caso com números reais para a venda B2B.

## F6 — Expansão (contínua)

- SDK Unreal e SDK Unity (cada um com sua própria mini-PoC antes).
- Avaliar transporte QUIC próprio (iroh/libp2p) se o libtorrent limitar.
- Incentivos ("Fast Lane") com estudo anti-fraude.
- DirectStorage no Windows.

## Paralelismo na prática

Na F1, por exemplo, a equipe se divide assim sem bloqueios:
- Sênior A: cache em disco (T2) — usa uma fonte falsa em memória.
- Sênior B: GDExtension + política de falta (T3) — usa um `sw-core` falso que lê do disco com atraso artificial.
- Pleno 1: `sw-pack` (T1) — só depende do contrato do manifesto.
- Pleno 2: fonte HTTP + agendador v1 (T4) — testa contra o servidor da bancada.
- Júnior 1: overlay de debug + traces (T3/T6).
- Júnior 2: testes unitários, rodadas da bancada e relatórios (T6).

A integração acontece uma vez por semana numa "build da semana" rodando a rota completa.
