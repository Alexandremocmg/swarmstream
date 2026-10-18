# Backlog inicial (F0, F1 e F2)

Tarefas prontas para virar issues. Cada uma tem trilha, nível mínimo para liderar, tamanho, dependências e critério de aceite.

**Tamanho:** P = até 2 dias · M = 3–5 dias · G = maior que 5 dias (o sênior quebra antes de começar).
**Etiquetas:** `boa-primeira-tarefa` = indicada para quem está chegando; `critico` = exige revisão de sênior/arquiteto.

Modelo de issue: [templates/tarefa-template.md](templates/tarefa-template.md).

## F0 — Fundação

| ID | Tarefa | Trilha | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- | --- |
| F0-01 | Criar monorepo com pastas `sw-core`, `sw-pack`, `sw-godot`, `sw-netlab`, `sw-services`, `docs` | T6 | P | P | — | Estrutura no repositório; README de cada pasta — **feito em 2026-10-17** |
| F0-02 | CMake + vcpkg com build vazio de `sw-core` e `sw-pack` no Windows e Linux | T6 | P | M | F0-01 | `cmake --build` funciona nos dois sistemas — **feito em 2026-10-17** (o caminho do Windows é exercitado pelo CI; localmente só o Unix foi verificado) |
| F0-03 | CI: build + testes em Windows e Linux a cada PR, ASan/UBSan no Linux | T6 | P | M | F0-02 | PR com teste falhando fica vermelho — **feito em 2026-10-17** (workflow `.github/workflows/ci.yml`; prova vermelha executada localmente com `ctest -S cmake/RedProbe.cmake`, exit ≠ 0) |
| F0-04 | clang-format, clang-tidy, modelo de PR, CODEOWNERS | T6 | J | P | F0-01 | `boa-primeira-tarefa`; formatação checada no CI |
| F0-05 | ~~Aceitar ADRs 0001–0008~~ (feito em 2026-10-17). Escrever os ADRs pendentes: backend, launcher, serialização do manifesto, hash dos blocos; criar `LICENSE` (Apache-2.0), `NOTICE` e `SECURITY.md` | — | A | P | — | Cada ADR novo com status "Aceita" |
| F0-06 | `swarmstream.h` v0 compilando, com funções retornando "não implementado" | T2 | A | M | F0-02 | Header revisado por 2 sêniores · `critico` |
| F0-07 | Interfaces `IContentSource` e `ICacheStore` + mocks em memória | T2/T4 | S | M | F0-06 | Mocks usados em pelo menos 1 teste |
| F0-08 | Especificação v0 do manifesto e do layout de bloco/peça | T1 | S | M | F0-05 | Documento atualizado em `03-formato-de-conteudo.md` · `critico` |
| F0-09 | Projeto Godot de teste com 2–5 GB de assets CC0 em 4+ regiões | T3 | J | M | — | `boa-primeira-tarefa`; abre e roda no Godot 4; licenças listadas |
| F0-10 | Rota de câmera gravada e reprodutível (modo "robô") | T3 | J | P | F0-09 | Duas execuções da rota passam pelos mesmos pontos |
| F0-11 | Servidor HTTP Range com latência, banda e perda configuráveis | T6 | J | M | — | `boa-primeira-tarefa`; perfis `fibra_300`, `cabo_50`, `adsl_10` |
| F0-12 | Baseline: FPS, quadros > 33 ms e tempo de carga do mapa lendo do SSD | T6 | P | P | F0-10 | Relatório em `docs/medicoes/baseline.md` |
| F0-13 | Guia de ambiente de desenvolvimento no CONTRIBUTING | T6 | J | P | F0-02 | Pessoa nova compila em < 1 h seguindo o guia |

## F1 — Engine + cache + rede simulada

### T1 — Conteúdo e build
| ID | Tarefa | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- |
| F1-01 | Chunker FastCDC com tamanhos mín/médio/máx configuráveis | S | M | F0-08 | Mudar 1 byte num arquivo de 1 GB altera ≤ 3 blocos |
| F1-02 | Testes do chunker (arquivos vazios, pequenos, enormes, aleatórios) | J | P | F1-01 | `boa-primeira-tarefa`; cobertura dos casos de borda |
| F1-03 | Compressão por bloco Zstd/LZ4 com escolha por tipo de arquivo | P | M | F1-01 | Taxa e tempo por tipo no relatório da CLI |
| F1-04 | Geração do manifesto v0, assinado com a chave de teste ([ADR-0012](adr/0012-serializacao-do-manifesto.md)) | P | M | F1-01 | Manifesto lido de volta é idêntico; adulterado é recusado; tamanho do arquivo e memória residente medidos |
| F1-05 | Classificação core/grupo/opcional por regras de pasta | P | M | F1-04 | Mapa de teste exportado com core < 15% |
| F1-06 | Ordenação de blocos em peças por localidade (por pasta/região) | S | M | F1-04 | Rota usa menos peças que com ordem alfabética |
| F1-07 | Mips/LODs baixos sempre no core | P | M | F1-05 | Toda textura tem mip ≤ 256 px local |
| F1-08 | Relatório da CLI: tamanho por grupo, nº de peças, taxa de compressão | J | P | F1-04 | `boa-primeira-tarefa` |

### T2 — Cache e VFS
| ID | Tarefa | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- |
| F1-10 | Arquivos de store pré-alocados e slots | S | M | F0-07 | Alocar/liberar 1 milhão de slots sem fragmentar · `critico` |
| F1-11 | Índice + journal com ordem dados → flush → journal → flush | S | G | F1-10 | Teste de queda (F1-31) sem peça corrompida · `critico` |
| F1-12 | Thread dona do cache e fila MPSC de pedidos | S | M | F1-10 | Sem data race no TSan com 16 threads leitoras · `critico` |
| F1-13 | Leases com contagem de referência | S | M | F1-12 | Peça com lease nunca é removida (teste de estresse) · `critico` |
| F1-14 | Remoção CLOCK com marcas 95%/85% e conjunto fixo | P | M | F1-13 | Uso do teto nunca passa de 100%; core nunca sai |
| F1-15 | Cache de blocos descomprimidos em RAM + pool de buffers | P | M | F1-12 | Zero alocações por leitura no caminho quente (medido) |
| F1-16 | Checksum XXH3 por bloco na leitura | P | P | F1-11 | Bloco adulterado em disco é detectado e pedido de novo |
| F1-17 | VFS: caminho + offset + tamanho → blocos | P | M | F1-04 | Leituras que cruzam blocos retornam bytes corretos |
| F1-18 | Ferramenta `sw-cache-inspect` (listar peças, uso, estado) | J | P | F1-11 | `boa-primeira-tarefa` |

### T3 — Engine (Godot)
| ID | Tarefa | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- |
| F1-20 | GDExtension com `ResourceFormatLoader` na frente sobre o `sw-core` (com mock), conforme o [ADR-0016](adr/0016-ponto-de-intercepcao-godot.md) | S | G | F0-06 | **Portão primeiro:** textura BCn 4K com mips, malha com 2 LODs e áudio OGG materializados a partir de bytes do mock, sem `FileAccess`, com troca no lugar. Depois: mapa de teste carrega pelo loader; `_load` nunca espera a rede (medido) |
| F1-21 | Política de falta para textura (mip menor + troca ao chegar) | S | M | F1-20 | Sem travar o frame numa falta de textura |
| F1-22 | Política de falta para malha (LOD menor / esconder) | P | M | F1-20 | Idem para malhas |
| F1-23 | Nó `SwarmHintArea3D` chamando `sw_hint_group` | P | P | F1-20 | Entrar na área dispara prefetch do grupo |
| F1-24 | Overlay de debug (acertos, faltas, fila, bytes por fonte) | J | M | F1-20 | `boa-primeira-tarefa`; liga/desliga por tecla |
| F1-25 | Gravação de traces de leitura (posição, tempo, peça) | J | P | F1-20 | Arquivo de trace gerado em cada rodada |
| F1-26 | Índice espacial dos traces e `sw_hint_position` | S | G | F1-25 | Taxa de falta cai em relação a só `SwarmHintArea3D` |

### T4 — Rede
| ID | Tarefa | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- |
| F1-27 | Fonte HTTP Range implementando `IContentSource`, com conexões persistentes | P | M | F0-07, F0-11 | Baixa peças da bancada em todos os perfis |
| F1-28 | Agendador por prazo v1 (uma fonte, fila por prazo, cancelamento) | S | M | F1-27 | Pedido urgente passa na frente de prefetch · `critico` |
| F1-29 | Estimador de banda e RTT da fonte | P | P | F1-27 | Erro < 20% contra o perfil configurado |

### T6 — Bancada e qualidade
| ID | Tarefa | Nível | Tam. | Depende de | Critério de aceite |
| --- | --- | --- | --- | --- | --- |
| F1-30 | Eventos de telemetria `read_miss`, `eviction`, `frame_stutter` gravados em arquivo | P | M | F1-12 | Eventos com os campos de `08-metricas-e-testes.md` |
| F1-31 | Teste de queda: matar o processo em pontos aleatórios da escrita | P | M | F1-11 | 1.000 quedas sem peça corrompida aceita |
| F1-32 | Teste de disco cheio e de erro de escrita | P | P | F1-11 | Erro tratado; nada indexado com falha |
| F1-33 | Script de relatório da rodada (métricas por perfil) | J | M | F1-30 | `boa-primeira-tarefa`; tabela pronta para o Portão 1 |
| F1-34 | Rodadas semanais nos 3 perfis + comparação com o baseline | J | P (recorrente) | F1-33 | Relatório da semana publicado |
| F1-35 | Experimento: blocos de 64, 128 e 256 KB | P | M | F1-33 | Recomendação registrada em ADR-0002 |

## F2 — Híbrido P2P (resumo; detalhar no planejamento da fase)

| ID | Tarefa | Trilha | Nível | Tam. |
| --- | --- | --- | --- | --- |
| F2-01 | Integrar libtorrent 2.x no build | T4 | P | M |
| F2-02 | Storage customizado do libtorrent nos slots do cache | T4/T2 | S | G · `critico` |
| F2-03 | Torrent v2 por pacote com web seed no `sw-pack` | T1 | P | M |
| F2-04 | Fonte P2P com `set_piece_deadline` | T4 | S | G · `critico` |
| F2-05 | Agendador v2: escolha de fonte e *hedging* | T4 | S/A | G · `critico` |
| F2-06 | Assinatura Ed25519 do manifesto | T8/T1 | S | M · `critico` |
| F2-07 | Fuzzing do manifesto e das mensagens de rede | T8 | P | M |
| F2-08 | Peer malicioso de teste (envia lixo) e penalização | T8/T4 | P | M |
| F2-09 | Tracker de teste + coturn na bancada | T5 | P | P |
| F2-10 | Peers sintéticos headless em Docker | T6 | P | M |
| F2-11 | QoS v1: limite de upload e desligar em `PARTIDA_ONLINE` | T4 | P | M |
| F2-12 | Telemetria de fonte por byte e painel simples | T6 | J | M · `boa-primeira-tarefa` |

## Distribuição sugerida na F1 (equipe base)

| Pessoa | Tarefas |
| --- | --- |
| Sênior engine | F1-20, F1-21, F1-26, F1-06 |
| Sênior redes/sistemas | F1-10 a F1-13, F1-28 |
| Pleno 1 | F1-03, F1-04, F1-05, F1-07, F1-17 |
| Pleno 2 | F1-14, F1-15, F1-16, F1-27, F1-29, F1-30 a F1-32, F1-35 |
| Júnior 1 | F1-24, F1-25, F1-18, F1-22 (em par) |
| Júnior 2 | F1-02, F1-08, F1-33, F1-34 |
| Arquiteto | F1-01, revisão de todo `critico`, Portão 1 |
