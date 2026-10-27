# sw-pack

Ferramenta de empacotamento de conteúdo do Swarmstream — trilha **T1
(Conteúdo/build)**. Divide o build do jogo em blocos comprimidos, identificados
por hash, e gera o manifesto assinado que a publisher distribui.

> **Estado:** Fase 1a. Existem o chunker `swarmstream-fastcdc-v0` (F1-01), SHA-256 e Ed25519 (ambos
> libsodium; o `sw-pack` não tem criptografia escrita à mão) e o manifesto v0 assinado (F1-04); o resto abaixo ainda não tem código.

## O que já existe

- `include/swarmstream/chunker.hpp`, `src/chunker.cpp`: chunker de [docs/03 §11.1](../docs/03-formato-de-conteudo.md).
  `Chunker::make(params)` valida os parâmetros; `cut(janela)` devolve o tamanho do primeiro bloco
  (sem alocação); `split(buffer)` devolve todos. A referência são os vetores de
  [testdata/vectors-v0.json](testdata/README.md), reproduzidos por `tests/test_chunker.cpp`.
- `include/swarmstream/sha256.hpp`, `src/sha256.cpp`: SHA-256 sobre `crypto_hash_sha256_*` do libsodium. Se o
  libsodium não iniciar o programa termina com mensagem (último recurso: as entradas públicas conferem
  `crypto_ready()` antes e devolvem erro).
- `include/swarmstream/manifest.hpp`, `src/manifest.cpp`: manifesto v0 de
  [docs/03 §7](../docs/03-formato-de-conteudo.md) ([ADR-0012](../docs/adr/0012-serializacao-do-manifesto.md)).
  `build_manifest(modelo, semente)` codifica o payload FlatBuffers, assina com Ed25519 e **lê de volta**
  o resultado antes de devolvê-lo. `ManifestView::open(bytes, opções, saída)` é o leitor, na ordem do §7.1
  (cabeçalho → chave → **assinatura** → sondagem do `min_reader` → verificador e validações do §8),
  sem cópia das tabelas colunares. O FlatBuffers é escrito e verificado à mão (esquema fixo e pequeno),
  então não há dependência nova; o layout é o do FlatBuffers e foi lido com o runtime oficial (ver abaixo).
- `include/swarmstream/crypto.hpp`, `src/crypto.cpp`: o **único** ponto que chama `sodium_init()`
  (`crypto_ready()`). Toda entrada pública com criptografia o confere e devolve erro se falhar; há um gancho
  só para testes (`testing::force_crypto_failure`) que simula a falha.
- `include/swarmstream/ed25519.hpp`, `src/ed25519.cpp`: interface fina (3 funções) sobre o **libsodium**
  (`crypto_sign_seed_keypair`, `crypto_sign_detached`, `crypto_sign_verify_detached`, a verificação estrita).
  Se o libsodium não iniciar: falha vira "assinatura inválida" ou `nullopt`, nunca aviso. O libsodium assina
  mensagem contígua, então com várias partes a interface monta uma cópia temporária da mensagem (o payload).
  A chave de teste (semente `00..1f`) fica só em `testdata/` e nos testes; nenhum build de release pode
  confiá-la (ADR-0012, item 4).
- `tools/sw_manifest_bench.cpp`: mede tamanho do arquivo, custo de abrir e memória residente
  (`sw-manifest-bench generate --blocks N --out m.bin`, depois `sw-manifest-bench open m.bin`).
  Números e extrapolação para 150 GB em [docs/medicoes/manifesto-v0.md](../docs/medicoes/manifesto-v0.md).
- Conferência independente (não usa código do sw-pack): `python -I testdata/verify_manifest_independent.py m.bin`
  verifica a assinatura com `cryptography`/OpenSSL e lê o payload com o runtime oficial do FlatBuffers.
- Teste pesado de estabilidade em 1 GiB (mudar, inserir ou remover 1 byte altera ≤ 3 blocos), fora do
  CTest padrão: `cmake -DSW_PACK_HEAVY_TESTS=ON ...` e `ctest -L heavy` (cerca de 1 min em Release).

## O que vai morar aqui

- Compressão por bloco Zstd/LZ4 por tipo de arquivo (F1-03)
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
