# sw-pack

Ferramenta de empacotamento de conteúdo do Swarmstream — trilha **T1
(Conteúdo/build)**. Divide o build do jogo em blocos comprimidos, identificados
por hash, e gera o manifesto assinado que a publisher distribui.

> **Estado:** Fase 1a. Existem o chunker `swarmstream-fastcdc-v0` (F1-01), a assinatura Ed25519 (libsodium;
> o `sw-pack` não tem criptografia escrita à mão), a compressão por bloco (F1-03) e a escrita do manifesto
> v0 assinado (F1-04); o resto abaixo ainda não tem código.
>
> **Leitura, descompressão e verificação não moram aqui:** são do [`sw-format`](../sw-format/README.md)
> (F1-61), que é o que o cliente liga. O `sw-pack` é a ferramenta de build e **depende** do `sw-format`
> (`sw-pack` → `sw-format`, nunca o contrário); os cabeçalhos `manifest.hpp`, `codec.hpp` e `ed25519.hpp`
> daqui incluem os do `sw-format`, então quem escreve e relê usa um include só.

## O que já existe

- `include/swarmstream/chunker.hpp`, `src/chunker.cpp`: chunker de [docs/03 §11.1](../docs/03-formato-de-conteudo.md).
  `Chunker::make(params)` valida os parâmetros; `cut(janela)` devolve o tamanho do primeiro bloco
  (sem alocação); `split(buffer)` devolve todos. A referência são os vetores de
  [testdata/vectors-v0.json](testdata/README.md), reproduzidos por `tests/test_chunker.cpp`.
- `include/swarmstream/manifest.hpp`, `src/manifest_build.cpp`: **escrita** do manifesto v0 de
  [docs/03 §7](../docs/03-formato-de-conteudo.md) ([ADR-0012](../docs/adr/0012-serializacao-do-manifesto.md)).
  `build_manifest(modelo, semente)` codifica o payload FlatBuffers (escritor à mão, esquema fixo e pequeno),
  assina com Ed25519 e **lê de volta** o resultado, com o MESMO `ManifestView::open` do cliente (do
  `sw-format`; não há segunda implementação do leitor), antes de devolvê-lo.
- `include/swarmstream/ed25519.hpp`, `src/ed25519.cpp`: **assinar** (`ed25519_public_key`, `ed25519_sign`)
  sobre o **libsodium** (`crypto_sign_seed_keypair`, `crypto_sign_detached`). A verificação é do `sw-format`.
  A chave de teste (semente `00..1f`) fica só em `testdata/` e nos testes; nenhum build de release pode
  confiá-la (ADR-0012, item 4).
- `tools/sw_manifest_bench.cpp`: mede tamanho do arquivo, custo de abrir e memória residente
  (`sw-manifest-bench generate --blocks N --out m.bin`, depois `sw-manifest-bench open m.bin`).
  Números e extrapolação para 150 GB em [docs/medicoes/manifesto-v0.md](../docs/medicoes/manifesto-v0.md).
- Conferência independente (não usa código do sw-pack): `python -I testdata/verify_manifest_independent.py m.bin`
  verifica a assinatura com `cryptography`/OpenSSL e lê o payload com o runtime oficial do FlatBuffers.
- `include/swarmstream/codec.hpp`, `src/codec.cpp`: **compressão** por bloco ([docs/03 §4](../docs/03-formato-de-conteudo.md)).
  `compress_block` usa Zstd (frame padrão, com `Frame_Content_Size`, sem dicionário) ou LZ4 (block
  format) e cai para NONE se não reduzir. `codec_for_path` escolhe o codec pela extensão (já comprimido:
  NONE; binário de engine: LZ4; texto e o resto: Zstd). `decompress_block` (e o enum `Codec`) é do
  `sw-format`. Zstd 1.5.6 e LZ4 1.10.0 vêm por `FetchContent` (em `sw-format/CMakeLists.txt`), com hash fixo.
- `sw-pack-report <pasta> [--codec auto|none|zstd|lz4] [--zstd-level N]`: comprime e descomprime cada
  bloco de uma pasta, confere que volta idêntico e imprime, por tipo de arquivo, taxa e MB/s de
  compressão e descompressão. Use com `--codec zstd` e `--codec lz4` para comparar e ajustar a
  tabela de extensões de `codec.cpp` com os assets reais.
- Teste pesado de estabilidade em 1 GiB (mudar, inserir ou remover 1 byte altera ≤ 3 blocos), fora do
  CTest padrão: `cmake -DSW_PACK_HEAVY_TESTS=ON ...` e `ctest -L heavy` (cerca de 1 min em Release).

## O que vai morar aqui

- Classificação core/grupo/opcional (F1-05); o `piece_hash` (Merkle da peça) e a montagem do manifesto a
  partir de arquivos ainda não existem: a F1-04 recebe o modelo já pronto
- Ordenação de blocos em peças por localidade (F1-06)
- Relatório de tamanho por grupo, nº de peças e taxa de compressão (F1-08)
- Torrent v2 por pacote com web seed (F2-03)

## Contrato

Só conversa com o resto do sistema pelos contratos de
[docs/02-arquitetura.md](../docs/02-arquitetura.md); não inclui cabeçalhos de
`sw-core` diretamente sem passar por esses contratos.

## Compilação

Precisa do **libsodium** (ADR-0012): com o toolchain do vcpkg (o `vcpkg.json` o declara) ele vem sozinho;
sem vcpkg, instale `libsodium-dev` e `pkg-config` (é o que os jobs de sanitizer e de lint fazem).

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake   # ou sem a opção, com libsodium-dev
cmake --build build
ctest --test-dir build --output-on-failure
```

## Referências

- [docs/03-formato-de-conteudo.md](../docs/03-formato-de-conteudo.md)
- [docs/09-roadmap-fases.md](../docs/09-roadmap-fases.md)
