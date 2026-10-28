# sw-format

O que o **cliente** precisa para **ler, descomprimir e verificar** o conteúdo do Swarmstream
([docs/03](../docs/03-formato-de-conteudo.md)). Biblioteca estática, C++20. Criada pelo cartão F1-61
(separada do `sw-pack`, sem mudar o formato, o ADR-0012 nem `swarmstream.h`).

| Arquivo | O que tem |
| --- | --- |
| `include/swarmstream/manifest_view.hpp`, `src/manifest_view.cpp` | `ManifestView::open` (a ÚNICA implementação do leitor: §7.1 e as validações do §8), `probe_min_reader`, `key_id_of`, o modelo em memória |
| `include/swarmstream/manifest_wire.hpp` | constantes do envelope que o leitor e o escritor (`sw-pack`) compartilham |
| `include/swarmstream/block_codec.hpp`, `src/block_codec.cpp` | `Codec`, `decompress_block` (Zstd/LZ4/None) |
| `include/swarmstream/ed25519_verify.hpp`, `src/ed25519_verify.cpp` | `ed25519_verify` (só verificar), sobre o libsodium |
| `include/swarmstream/sha256.hpp`, `src/sha256.cpp` | SHA-256 sobre o libsodium |
| `include/swarmstream/crypto.hpp`, `src/crypto.cpp` | `crypto_ready()`: o único ponto que chama `sodium_init()` |
| `include/swarmstream/message_parts.hpp` | a mensagem de uma assinatura em partes (usada ao verificar e ao assinar) |

**Não tem** chunker, construtor do manifesto, assinatura nem `compress_block`: isso é do
[`sw-pack`](../sw-pack/README.md), que depende deste alvo. O namespace continua `swarmstream::pack`
(trocá-lo mudaria todo chamador e teste; fica para quando a API C real usar o alvo, F1-60).

## Grafo de dependências
```
sw-core   ──(ainda não; F1-60)──► sw-format ──► libsodium (alvo sw-sodium), Zstd/LZ4 (estáticos, FetchContent)
sw-pack   ────────────────────► sw-format
```
`sw-format` não depende de `sw-core`, `sw-pack` nem `sw-godot`.

## Testes
- `sw-format-client-min`: executável que liga **só** o `sw-format`. Abre um manifesto VÁLIDO com assinatura
  Ed25519 real (`tests/client_min_fixture.hpp`, receita no cabeçalho), descomprime o bloco que ele descreve e confere o
  SHA-256, descomprime um quadro LZ4 e confere que o leitor recusa payload alterado, chave não confiável e lixo.
- `sw-format-symbols` (registrado no `sw-pack`, que enxerga os dois lados): inspeciona os símbolos desse
  executável (`nm`; no MSVC, o mapa do ligador) e as bibliotecas `.a`/`.lib`. Imprime a configuração de
  build (compilador, ligador, flags, tipo, LTO, `--gc-sections`) e, para cada símbolo, a CLASSE: definição (código no
  executável), referência indefinida `U` (biblioteca dinâmica) ou importação (DLL); no Windows lista também o que
  a `libsodium.dll` exporta (`dumpbin /EXPORTS`). O classificador tem autoteste (`sw-format-symbols-selftest`). **Exige** no executável o leitor, a
  descompressão e a verificação; **proíbe** o chunker, o construtor, a assinatura e `compress_block`; **só
  relata** os símbolos de compressão de terceiros (`ZSTD_compress*`, `LZ4_compress*`), porque o que o
  ligador descarta depende dele e das flags. Se a ferramenta faltar, o teste se declara PULADO (no CI isso é erro; nunca
  passa em silêncio). Os testes de leitura e verificação (ordem do ADR-0012, `crypto_ready()` em toda
  entrada, adulteração) continuam em `sw-pack/tests/test_manifest.cpp`, contra este alvo.
