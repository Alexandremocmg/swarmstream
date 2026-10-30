# ADR-0017: Semântica do VFS (F1-17): conflito arquivo/diretório e leitura além do fim

**Status:** Aceita (2026-10-29: escolhas técnicas e aceite do texto pelo dono do produto, na apresentação do PR #26; mesclado como `5522c9c106df956f25a366b3135f21a7d5c9040d`) · **Data:** 2026-10-29 · **Autor:** Planejador (a pedido do dono)

## Contexto
O VFS do `sw-core` ([F1-17](../../tarefas/cartoes/F1-17.md)) traduz caminho, offset e tamanho em trechos de blocos e lista diretórios virtuais. Ele não lê cache nem rede. O contrato [`swarmstream.h`](../../sw-core/include/swarmstream.h) v0 e o formato ([03](../03-formato-de-conteudo.md)) deixam lacunas que o VFS precisou fechar. Este ADR registra **o que já estava escrito**, **o que apenas esclarece** e **o que acrescenta comportamento**, e como os estados internos do VFS viram códigos públicos **existentes** (sem código novo, sem editar o header, sem editar o `docs/03`).

Em todos os pontos o VFS devolve um estado próprio e **não** escolhe `sw_result`; o mapeamento é aplicado no [F1-60](../../tarefas/cartoes/F1-60.md).

## Decisão

### 1. Regras já expressas no contrato (nada a decidir)
| Regra | Onde está |
|---|---|
| `offset == tamanho` devolve `SW_OK` com 0 bytes; `offset > tamanho` devolve `SW_E_OUT_OF_RANGE`; fora disso a leitura é tudo ou nada | `swarmstream.h`, comentário de leitura (L315–317) |
| Raiz do diretório virtual é `""`; ordem estável por nome em bytes UTF-8; `SW_DONE` ao fim | `swarmstream.h` (L295–298) |
| `sw_open` devolve `SW_E_NOT_FOUND` se o caminho não existir no manifesto | `swarmstream.h` (L268–269) |
| Caminhos do manifesto: forma canônica, sem `/` inicial ou final, estritamente crescentes, sem colisão ignorando maiúsculas | `docs/03` §6 e §8 item 7 |

### 2. Esclarecimentos (fixam o que o texto já diz)
- **A. Caminhos:** comparação exata em bytes com os caminhos do manifesto, sem normalização. `maps/`, `/config.ini`, `Config.ini` e `maps//level1.bin` não existem: `NotFound`. Ponteiro nulo **não** é caminho inexistente: argumento inválido segue o contrato da API C (`SW_E_INVALID_ARGUMENT`, F1-60); o VFS recebe `string_view` e não tem semântica de nulo.
- **B1.** `open_dir("")` abre a raiz (L295).
- **D2a.** `open_dir` em caminho inexistente (inclusive com `/` final ou nome parcial) devolve `NotFound`.
- **E1.** `offset == tamanho` → Ok, 0 bytes, para qualquer `size`; `offset > tamanho` → `OutOfRange`, **inclusive com `size == 0`**.
- **F1.** A listagem usa a ordem por bytes UTF-8 do nome, com arquivos e subdiretórios misturados numa só ordem (`sub` < `sub-1.bin` < `sub.bin`, embora no manifesto `sub-1.bin` e `sub.bin` venham antes de `sub/x`).

### 3. Comportamentos adicionais aprovados (acrescentam regra que o texto não dava)
- **B2.** `stat("")` é diretório de tamanho 0; `open("")` é `IsDirectory`; a raiz existe mesmo sem arquivos.
- **C.** `open` de um diretório devolve o estado interno `IsDirectory`, distinto de `NotFound`. `SW_E_NOT_FOUND` fica reservado ao caminho inexistente.
- **D2b.** `open_dir` de um arquivo devolve `NotDirectory`. Diretórios são derivados dos caminhos dos arquivos; **não existe diretório vazio** além da raiz.
- **E2.** `size == 0` com `offset <= tamanho` → Ok, 0 trechos.
- **E3. Leitura que passa do fim.** Com `offset < tamanho`, se `size > tamanho - offset`, o pedido é **recusado integralmente** com `OutOfRange`: zero bytes entregues, destino intocado, sem leitura curta. `size == tamanho - offset` é Ok. O intervalo é validado **sem** somar `offset + size` (que pode estourar `uint64_t`). O F1-60 não pode truncar em silêncio. Isto **estende o sentido** de `SW_E_OUT_OF_RANGE`, cujo comentário no header diz "offset além do fim do arquivo", para "o intervalo pedido ultrapassa o fim"; o texto do header **não é editado** nesta etapa.
- **F2.** Entradas de diretório na listagem têm `size = 0`.
- **D. Conflito arquivo/diretório.** *Definição exata:* há conflito se existirem, no manifesto, um caminho `P` e um caminho `Q` com `Q.size() > P.size()`, `Q` começando por `P` e `Q[P.size()] == '/'`. Prefixo sem barra (`a` e `ab`, `a` e `a0`, `a` e `a-1`) **não** é conflito; `a` e `a/` não coexistem (o §6 proíbe `/` final). A construção do índice do VFS (`Vfs::build`, executada no `sw_init`) **recusa** o manifesto com `FileDirectoryConflict`. **Escolha do dono:** validar no VFS, sem alterar `sw-format` nem `sw-pack` nesta entrega.

**Distinção que deve ficar clara na documentação:** um manifesto pode ser **estruturalmente aceito pelo leitor** (`ManifestView::open` e o §8 do `docs/03` aceitam `a` e `a/b` juntos) e **não ser utilizável como sistema de arquivos virtual**. A recusa de conflito é regra do VFS, **não** uma validação já existente no formato. O `docs/03` não é editado nesta etapa.

### 4. Mapeamento dos erros internos para códigos públicos existentes
Nenhum código público novo (ADR-0014).

| Estado interno do VFS | `sw_result` na API C |
|---|---|
| `NotFound` | `SW_E_NOT_FOUND` (reservado ao caminho inexistente) |
| `IsDirectory` | `SW_E_INVALID_ARGUMENT` |
| `NotDirectory` | `SW_E_INVALID_ARGUMENT` |
| `OutOfRange` | `SW_E_OUT_OF_RANGE` |
| `OutOfMemory` (só `open_dir`) | `SW_E_OUT_OF_MEMORY` |
| `FileDirectoryConflict` (em `Vfs::build`, no `sw_init`) | `SW_E_MANIFEST_INVALID` |
| Ponteiro nulo ou argumento inválido | `SW_E_INVALID_ARGUMENT` (contrato; fora do VFS) |

## Alternativas consideradas
- **Conflito: apertar o `sw-format` (`ManifestView::open`) ou o `sw-pack`.** Fora desta entrega por escolha do dono: mudaria o leitor, os vetores e o construtor. Fica como evolução possível, em ADR e card próprios.
- **Conflito: precedência (arquivo vence diretório).** Descartada: esconde erro do produtor do pacote e cria nomes ambíguos.
- **Leitura que passa do fim: leitura curta.** Descartada pelo dono; "tudo ou nada" vale fora do EOF exato.

## Consequências
- (+) Nenhum manifesto ambíguo chega a `sw_stat`/`sw_dir_*`; o erro aparece na abertura.
- (+) `swarmstream.h` v0, `sw-format` e `docs/03` não mudam.
- (−) Enquanto o `sw-format`/`sw-pack` não tratarem o caso, o construtor pode produzir um manifesto que o cliente recusa em `sw_init`.
- (−) O comentário de `SW_E_OUT_OF_RANGE` no header fica mais estreito do que o uso. Atualizar o comentário é mudança de header e segue o processo do ADR-0014.

## Verificação
Testes do F1-17 em `sw-core/tests/test_vfs.cpp`: `test_paths`, `test_empty_manifest_and_conflict`, `test_file_directory_conflict_definition`, `test_directory_listing`, `test_resolve_edges`, `test_every_file_against_random_reads`, `test_block_limit_boundaries`, com mutantes das fronteiras. A tabela por ponto (exemplos e teste) está no cartão do F1-17.
