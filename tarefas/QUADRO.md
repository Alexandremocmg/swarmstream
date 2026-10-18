# Quadro de tarefas

Gerado por `python tarefas/gerar_quadro.py` a partir dos cartões. **Não edite à mão.** Como funciona: [README.md](README.md). Decisões: [decisoes.md](decisoes.md).

| EM_ANDAMENTO | EM_REVISAO | AGUARDANDO_DONO | BLOQUEADA | ABERTA | FEITA |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | 33 | 1 |

## Abertas na F1a
| Cartão | Papel | Modelo e esforço | Crítico | Pode começar? | Tarefa |
| --- | --- | --- | --- | --- | --- |
| [F1-01](cartoes/F1-01.md) | R2 | Sonnet 5.5, esforço alto (Opus 5.5 se travar) | não | sim | Chunker `swarmstream-fastcdc-v0` em C++ (definição aprovada: 03 §11.1, vetores) |
| [F1-02](cartoes/F1-02.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-01 | Testes do chunker (arquivos vazios, pequenos, enormes, aleatórios) |
| [F1-03](cartoes/F1-03.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-01 | Compressão por bloco Zstd/LZ4 com escolha por tipo de arquivo |
| [F1-04](cartoes/F1-04.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-01 | Geração do manifesto v0, assinado com a chave de teste (ADR-0012) |
| [F1-05](cartoes/F1-05.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-04 | Classificação core/grupo/opcional por regras de pasta |
| [F1-07](cartoes/F1-07.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-05 | Mips/LODs baixos sempre no core |
| [F1-08](cartoes/F1-08.md) | R3 | Haiku 5.5, esforço médio | não | espera F1-04 | Relatório da CLI: tamanho por grupo, nº de peças, taxa de compressão |
| [F1-10](cartoes/F1-10.md) | R1 | Opus 5.5, esforço alto | sim | sim | Arquivos de store pré-alocados e slots |
| [F1-11](cartoes/F1-11.md) | R1 | Opus 5.5, esforço muito alto | sim | espera F1-10 | Índice + journal com ordem dados → flush → journal → flush |
| [F1-12](cartoes/F1-12.md) | R1 | Opus 5.5, esforço alto | sim | espera F1-10 | Thread dona do cache e fila MPSC de pedidos |
| [F1-13](cartoes/F1-13.md) | R1 | Opus 5.5, esforço alto | sim | espera F1-12 | Leases com contagem de referência |
| [F1-14](cartoes/F1-14.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-13 | Remoção CLOCK com marcas 95%/85% e conjunto fixo |
| [F1-16](cartoes/F1-16.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-11 | Checksum XXH3 por bloco na leitura |
| [F1-17](cartoes/F1-17.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-04 | VFS: caminho + offset + tamanho → blocos |
| [F1-18](cartoes/F1-18.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-11 | Ferramenta `sw-cache-inspect` (listar peças, uso, estado) |
| [F1-20](cartoes/F1-20.md) | R1 | Opus 5.5, esforço muito alto | sim | sim | GDExtension com `ResourceFormatLoader` na frente sobre o `sw-core` (com mock), conforme o ADR-0016 |
| [F1-21](cartoes/F1-21.md) | R1 | Opus 5.5, esforço alto | não | espera F1-20 | Política de falta para textura (mip menor + troca ao chegar) |
| [F1-24](cartoes/F1-24.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-20 | Overlay de debug (acertos, faltas, fila, bytes por fonte) |
| [F1-27](cartoes/F1-27.md) | R2 | Sonnet 5.5, esforço alto | não | sim | Fonte HTTP Range implementando `IContentSource`, com conexões persistentes |
| [F1-28](cartoes/F1-28.md) | R1 | Opus 5.5, esforço muito alto | sim | espera F1-27 | Agendador por prazo v1 (uma fonte, fila por prazo, cancelamento) |
| [F1-29](cartoes/F1-29.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-27 | Estimador de banda e RTT da fonte |
| [F1-30](cartoes/F1-30.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-12 | Eventos de telemetria `read_miss`, `eviction`, `frame_stutter` gravados em arquivo |
| [F1-31](cartoes/F1-31.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-11 | Teste de queda: matar o processo em pontos aleatórios da escrita |
| [F1-32](cartoes/F1-32.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-11 | Teste de disco cheio e de erro de escrita |
| [F1-33](cartoes/F1-33.md) | R3 | Sonnet 5.5, esforço médio | não | sim | Script de relatório da rodada (métricas por perfil) |
| [F1-34](cartoes/F1-34.md) | R3 | Haiku 5.5, esforço médio | não | espera F1-33 | Rodadas semanais nos 3 perfis + comparação com o baseline |

## Abertas na F1b
| Cartão | Papel | Modelo e esforço | Crítico | Pode começar? | Tarefa |
| --- | --- | --- | --- | --- | --- |
| [F1-06](cartoes/F1-06.md) | R1 | Opus 5.5, esforço alto | não | espera F1-04 | Ordenação de blocos em peças por localidade (por pasta/região) |
| [F1-15](cartoes/F1-15.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-12 | Cache de blocos descomprimidos em RAM + pool de buffers |
| [F1-22](cartoes/F1-22.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-20 | Política de falta para malha (LOD menor / esconder) |
| [F1-23](cartoes/F1-23.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-20 | Nó `SwarmHintArea3D` chamando `sw_hint_group` |
| [F1-25](cartoes/F1-25.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-20 | Gravação de traces de leitura (posição, tempo, peça) |
| [F1-26](cartoes/F1-26.md) | R1 | Opus 5.5, esforço alto | não | espera F1-25 | Índice espacial dos traces e `sw_hint_position` |
| [F1-35](cartoes/F1-35.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-33 | Experimento: blocos de 64, 128 e 256 KB |

## Feitas
- [F1-36](cartoes/F1-36.md): Corrigir a checagem vazia do `baseline.py --check`: relatório sem as métricas esperadas (ou com chaves erradas) hoje imprime "within limits" e sai com código 0. Deve falhar quando faltar `frames_over_33ms_steady`, `frames_over_100ms`, `p99_frame_ms` ou `load_ms`, e ter teste para isso
