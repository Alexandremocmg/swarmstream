# Quadro de tarefas

Gerado por `python tarefas/gerar_quadro.py` a partir dos cartões. **Não edite à mão.** Como funciona: [README.md](README.md). Decisões: [decisoes.md](decisoes.md).

| EM_ANDAMENTO | EM_REVISAO | AGUARDANDO_DONO | BLOQUEADA | ABERTA | FEITA |
| --- | --- | --- | --- | --- | --- |
| 3 | 0 | 2 | 3 | 25 | 31 |

## Espera o dono do produto
- [F1-04](cartoes/F1-04.md): Geração do manifesto v0, assinado com a chave de teste (ADR-0012)
- [F1-44](cartoes/F1-44.md): Refazer o baseline com 10 rodadas e recalcular os limites pela mesma regra

## EM_ANDAMENTO
- [F1-20](cartoes/F1-20.md) (R1, sessão: Arquiteto): GDExtension com `ResourceFormatLoader` na frente sobre o `sw-core` (com mock), conforme o ADR-0016
- [F1-33](cartoes/F1-33.md) (R3, sessão: Pleno): Script de relatório da rodada (métricas por perfil)
- [F1-47](cartoes/F1-47.md) (R3, sessão: Pleno): Validar a bancada com uma mudança conhecida (rede mais lenta tem de piorar a espera)

## BLOQUEADA
- [F1-58](cartoes/F1-58.md) (R2, sessão: (a atribuir) sessão nova só para este cartão): Remedir o manifesto v0 com libsodium real e atender (ou renegociar) tempo, tamanho e memória
- [F1-59](cartoes/F1-59.md) (R2, sessão: (a atribuir) sessão nova só para este cartão): Integrar a telemetria às leituras reais (`sw_read`) e o `telemetry_path` ao `sw_init`
- [F1-60](cartoes/F1-60.md) (R1, sessão: (a atribuir) sessão nova só para este cartão): API C real (`sw_init`, `sw_open`, `sw_read`, `sw_shutdown`) sobre cache, agendador, fonte e VFS

## Abertas na F1a
| Cartão | Papel | Modelo e esforço | Crítico | Pode começar? | Tarefa |
| --- | --- | --- | --- | --- | --- |
| [F1-02](cartoes/F1-02.md) | R3 | Sonnet 5.5, esforço médio | não | sim | Testes do chunker (arquivos vazios, pequenos, enormes, aleatórios) |
| [F1-05](cartoes/F1-05.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-04 | Classificação core/grupo/opcional por regras de pasta |
| [F1-07](cartoes/F1-07.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-05 | Mips/LODs baixos sempre no core |
| [F1-08](cartoes/F1-08.md) | R3 | Haiku 5.5, esforço médio | não | espera F1-04 | Relatório da CLI: tamanho por grupo, nº de peças, taxa de compressão |
| [F1-21](cartoes/F1-21.md) | R1 | Opus 5.5, esforço alto | não | espera F1-20 | Política de falta para textura (mip menor + troca ao chegar) |
| [F1-24](cartoes/F1-24.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-20 | Overlay de debug (acertos, faltas, fila, bytes por fonte) |
| [F1-34](cartoes/F1-34.md) | R3 | Haiku 5.5, esforço médio | não | espera F1-33 | Rodadas semanais nos 3 perfis + comparação com o baseline |
| [F1-37](cartoes/F1-37.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-04 | Vetor do envelope com payload FlatBuffers real |
| [F1-45](cartoes/F1-45.md) | R2 | Sonnet 5.5, esforço médio | não | sim | Formatar o C++ existente com clang-format (PR mecânico) depois do congelamento |
| [F1-50](cartoes/F1-50.md) | R1 | Opus 5.5, esforço alto | sim | espera F1-20 | Trocar uma thread por peça e por ensure por um pool de threads (loader e mock) |
| [F1-52](cartoes/F1-52.md) | R2 | Sonnet 5.5, esforço baixo | não | sim | Teste do chunker em C++ cobre o vetor de bordas edges_small_params |
| [F1-57](cartoes/F1-57.md) | R2 | Sonnet 5.5, esforço alto | não | sim | Teste instável do agendador: destrutor atrás de callback lento (test_deadline_scheduler.cpp:962) |
| [F1-62](cartoes/F1-62.md) | R2 | Sonnet 5.5, esforço médio | não | sim | Medir o custo das funções de compressão (LZ4) e de assinatura (libsodium) no binário do cliente e avaliar alternativas |
| [F1-63](cartoes/F1-63.md) | R2 | Sonnet 5.5, esforço alto | não | sim | Garantia de `Vfs::build` em caso de falha (inclusive ao reconstruir um `Vfs` já válido) |
| [F1-64](cartoes/F1-64.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-63 | Reforçar os testes do VFS: byte alto após o prefixo de diretório, bloco de 262144 bytes e offsets acima de 4 GiB |
| [F1-65](cartoes/F1-65.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-63 | Precondição de uso de `VfsFile` válido e proteção no F1-60 contra handles inválidos |
| [F1-66](cartoes/F1-66.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-63 | Atualizar as referências ao ADR-0017 no código e a descrição do PR #25 |

## Abertas na F1b
| Cartão | Papel | Modelo e esforço | Crítico | Pode começar? | Tarefa |
| --- | --- | --- | --- | --- | --- |
| [F1-06](cartoes/F1-06.md) | R1 | Opus 5.5, esforço alto | não | espera F1-04 | Ordenação de blocos em peças por localidade (por pasta/região) |
| [F1-22](cartoes/F1-22.md) | R2 | Sonnet 5.5, esforço alto | não | espera F1-20 | Política de falta para malha (LOD menor / esconder) |
| [F1-23](cartoes/F1-23.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-20 | Nó `SwarmHintArea3D` chamando `sw_hint_group` |
| [F1-25](cartoes/F1-25.md) | R3 | Sonnet 5.5, esforço médio | não | espera F1-20 | Gravação de traces de leitura (posição, tempo, peça) |
| [F1-26](cartoes/F1-26.md) | R1 | Opus 5.5, esforço alto | não | espera F1-25 | Índice espacial dos traces e `sw_hint_position` |
| [F1-35](cartoes/F1-35.md) | R2 | Sonnet 5.5, esforço médio | não | espera F1-33 | Experimento: blocos de 64, 128 e 256 KB |
| [F1-38](cartoes/F1-38.md) | R2 | Sonnet 5.5, esforço médio | não | sim | Segunda implementação independente de Zstd nos vetores |
| [F1-40](cartoes/F1-40.md) | R2 | Sonnet 5.5, esforço médio | não | sim | Carga fria de verdade, depois de reiniciar o Windows |

## Feitas
- [F1-01](cartoes/F1-01.md): Chunker `swarmstream-fastcdc-v0` em C++ (definição aprovada: 03 §11.1, vetores)
- [F1-03](cartoes/F1-03.md): Compressão por bloco Zstd/LZ4 com escolha por tipo de arquivo
- [F1-10](cartoes/F1-10.md): Arquivos de store pré-alocados e slots
- [F1-11](cartoes/F1-11.md): Índice + journal com ordem dados → flush → journal → flush
- [F1-12](cartoes/F1-12.md): Thread dona do cache e fila MPSC de pedidos
- [F1-13](cartoes/F1-13.md): Leases com contagem de referência
- [F1-14](cartoes/F1-14.md): Remoção CLOCK com marcas 95%/85% e conjunto fixo
- [F1-15](cartoes/F1-15.md): Cache de blocos descomprimidos em RAM + pool de buffers
- [F1-16](cartoes/F1-16.md): Checksum XXH3 por bloco na leitura
- [F1-17](cartoes/F1-17.md): VFS: caminho + offset + tamanho → blocos
- [F1-18](cartoes/F1-18.md): Ferramenta `sw-cache-inspect` (listar peças, uso, estado)
- [F1-27](cartoes/F1-27.md): Fonte HTTP Range implementando `IContentSource`, com conexões persistentes
- [F1-28](cartoes/F1-28.md): Agendador por prazo v1 (uma fonte, fila por prazo, cancelamento)
- [F1-29](cartoes/F1-29.md): Estimador de banda e RTT da fonte
- [F1-30](cartoes/F1-30.md): Eventos de telemetria `read_miss`, `eviction`, `frame_stutter` gravados em arquivo
- [F1-31](cartoes/F1-31.md): Teste de queda: matar o processo em pontos aleatórios da escrita
- [F1-32](cartoes/F1-32.md): Teste de disco cheio e de erro de escrita
- [F1-36](cartoes/F1-36.md): Corrigir a checagem vazia do `baseline.py --check`: relatório sem as métricas esperadas (ou com chaves erradas) hoje imprime "within limits" e sai com código 0. Deve falhar quando faltar `frames_over_33ms_steady`, `frames_over_100ms`, `p99_frame_ms` ou `load_ms`, e ter teste para isso
- [F1-39](cartoes/F1-39.md): Ampliar o lint do CI para as ferramentas do baseline e os vetores
- [F1-41](cartoes/F1-41.md): Alinhar a spec 03 (§7.1, §7.2, §8) ao ADR-0012: format_version só do envelope, min_reader no payload
- [F1-42](cartoes/F1-42.md): Alinhar o doc 04 ao ADR-0013: checksum XXH3 por bloco, não por peça, no índice
- [F1-43](cartoes/F1-43.md): Definir no doc 08 a espera numa falta como no swarmstream.h
- [F1-46](cartoes/F1-46.md): Trocar assert por verificação que não some com NDEBUG nos testes do sw-core
- [F1-48](cartoes/F1-48.md): Método do percentil e mínimo de amostras para o p99 da espera no Portão 1
- [F1-49](cartoes/F1-49.md): CI compila a extensão do Godot (loader) e roda o portão sem janela
- [F1-51](cartoes/F1-51.md): Vetor de bordas do chunker e esclarecimentos da spec 03 §11.1 e do README dos vetores
- [F1-53](cartoes/F1-53.md): Corrigir a limitação de banda do sw-netlab (fatias antes de enviar e banda do enlace inteiro)
- [F1-54](cartoes/F1-54.md): Reduzir o custo do CI sem tirar checks dos PRs
- [F1-55](cartoes/F1-55.md): Reduzir o custo do CI da extensão do Godot (item D do F1-54)
- [F1-56](cartoes/F1-56.md): MemoryContentSource trata o prazo como prioridade, não como limite
- [F1-61](cartoes/F1-61.md): Separar o `sw-format` (leitura, descompressão e verificação) do `sw-pack`
