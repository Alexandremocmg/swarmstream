# sw-pack

Ferramenta de empacotamento de conteúdo do Swarmstream — trilha **T1
(Conteúdo/build)**. Divide o build do jogo em blocos comprimidos, identificados
por hash, e gera o manifesto assinado que a publisher distribui.

> **Estado:** Fase 1a. Existem o chunker `swarmstream-fastcdc-v0` (F1-01), o SHA-256 que ele
> usa e a compressão por bloco (F1-03); o resto abaixo ainda não tem código.

## O que já existe

- `include/swarmstream/chunker.hpp`, `src/chunker.cpp`: chunker de [docs/03 §11.1](../docs/03-formato-de-conteudo.md).
  `Chunker::make(params)` valida os parâmetros; `cut(janela)` devolve o tamanho do primeiro bloco
  (sem alocação); `split(buffer)` devolve todos. A referência são os vetores de
  [testdata/vectors-v0.json](testdata/README.md), reproduzidos por `tests/test_chunker.cpp`.
- `include/swarmstream/sha256.hpp`, `src/sha256.cpp`: SHA-256 pequeno, sem dependências.
- `include/swarmstream/codec.hpp`, `src/codec.cpp`: compressão por bloco ([docs/03 §4](../docs/03-formato-de-conteudo.md)).
  `compress_block` usa Zstd (frame padrão, com `Frame_Content_Size`, sem dicionário) ou LZ4 (block
  format) e cai para NONE se não reduzir; `decompress_block` exige saída com exatamente `raw_size`
  bytes. `codec_for_path` escolhe o codec pela extensão (já comprimido: NONE; binário de engine:
  LZ4; texto e o resto: Zstd). Zstd 1.5.6 e LZ4 1.10.0 vêm por `FetchContent`, com hash fixo.
- `sw-pack-report <pasta> [--codec auto|none|zstd|lz4] [--zstd-level N]`: comprime e descomprime cada
  bloco de uma pasta, confere que volta idêntico e imprime, por tipo de arquivo, taxa e MB/s de
  compressão e descompressão. Use com `--codec zstd` e `--codec lz4` para comparar e ajustar a
  tabela de extensões de `codec.cpp` com os assets reais.
- Teste pesado de estabilidade em 1 GiB (mudar, inserir ou remover 1 byte altera ≤ 3 blocos), fora do
  CTest padrão: `cmake -DSW_PACK_HEAVY_TESTS=ON ...` e `ctest -L heavy` (cerca de 1 min em Release).

## O que vai morar aqui

- Geração e leitura do manifesto v0 (F1-04), classificação core/grupo/opcional (F1-05)
- Ordenação de blocos em peças por localidade (F1-06)
- Relatório de tamanho por grupo, nº de peças e taxa de compressão (F1-08)
- Torrent v2 por pacote com web seed (F2-03)

## Contrato

Só conversa com o resto do sistema pelos contratos de
[docs/02-arquitetura.md](../docs/02-arquitetura.md); não inclui cabeçalhos de
`sw-core` diretamente sem passar por esses contratos.

## Compilação

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Referências

- [docs/03-formato-de-conteudo.md](../docs/03-formato-de-conteudo.md)
- [docs/09-roadmap-fases.md](../docs/09-roadmap-fases.md)
