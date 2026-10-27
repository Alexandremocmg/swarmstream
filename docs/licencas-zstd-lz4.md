# Licenças do Zstd 1.5.6 e do LZ4 1.10.0 (F1-03)

Conferido em 2026-10-27 **nos arquivos reais** baixados pelo `FetchContent` (`build/_deps/zstd-src` e `lz4-src`), não de memória. Isto é uma análise técnica, não parecer jurídico (o ADR-0008 já recomenda consulta jurídica antes da publicação pública).

**O `NOTICE` não foi alterado.** Ele é arquivo do dono do produto: a seção da §4 é uma **PROPOSTA — aguarda aprovação do dono**.

## 1. O que é baixado e o que é compilado

| | Zstd | LZ4 |
| --- | --- | --- |
| Versão / URL | 1.5.6, `https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz` | 1.10.0, `https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz` |
| SHA-256 do tarball | `30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7` | `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b` |
| Alvo CMake que usamos | `libzstd_static` (`build/cmake/lib`) | `lz4_static` (`build/cmake/lib`) |
| Compilado | só `lib/` (common, compress, decompress, dictBuilder) | só `lib/`: `lz4.c`, `lz4hc.c`, `lz4frame.c`, `lz4file.c`, `xxhash.c` |
| **Não** compilado | `programs/` (`ZSTD_BUILD_PROGRAMS=OFF`), `contrib/` (`ZSTD_BUILD_CONTRIB=OFF`), `tests/` (`ZSTD_BUILD_TESTS=OFF`), decoders legados (`ZSTD_LEGACY_SUPPORT=OFF`, acrescentado neste commit) | `programs/` (`LZ4_BUILD_CLI=OFF`), `contrib/`, `tests/`, `examples/` |

Conferência: os objetos gerados em `build/_deps/{zstd,lz4}-build` vêm todos de `lib/`; nada de `programs/` ou `contrib/` aparece no `build.ninja`. O `sw-pack` só chama `ZSTD_compress`, `ZSTD_decompress`, `ZSTD_getFrameContentSize`, `ZSTD_getDictID_fromFrame`, `LZ4_compress_default` e `LZ4_decompress_safe`; os demais arquivos entram na biblioteca estática mas não são usados.

## 2. Licença de cada parte compilada

**Zstd (`lib/`).** Cada arquivo diz: *"licensed under both the BSD-style license (LICENSE) and the GPLv2 (COPYING). You may select, at your option, one of the above-listed licenses."* **Escolhemos a BSD** (arquivo `LICENSE` do Zstd, BSD-3-Clause na prática: 3 cláusulas, a 3ª proíbe usar os nomes Facebook/Meta para endossar). Motivo: a GPLv2 obrigaria a distribuir o `sw-core`/jogo sob GPL; a BSD é compatível com Apache-2.0 (e já é a opção prevista no ADR-0008). Copyright: *"Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved."*

Três pontos dentro de `lib/` do Zstd que **não** são Meta:
- `lib/dictBuilder/divsufsort.c` e `.h`: libdivsufsort-lite, **MIT**, *Copyright (c) 2003-2008 Yuta Mori*. Compilado (faz parte do dictBuilder, que não usamos, mas está na biblioteca). A MIT exige o aviso e o texto da permissão em "todas as cópias ou partes substanciais"; tratamos o binário como cópia e reproduzimos o texto (ver §3). Texto em `third_party/licenses/zstd-1.5.6-divsufsort-LICENSE-MIT.txt`.
- `lib/common/threading.c` e `.h`: *Copyright (c) 2016 Tino Reichardt*, **mesma dupla BSD/GPLv2 "à sua escolha"** do Zstd; escolhemos a BSD. Com `ZSTD_MULTITHREAD_SUPPORT=OFF` o código de threads fica inativo.
- `lib/common/xxhash.c`: *Copyright (c) Yann Collet - Meta Platforms, Inc*, mesma dupla BSD/GPLv2.

**LZ4 (`lib/`).** `lib/LICENSE` do LZ4: *"all files in the lib directory use a BSD 2-Clause license"*; `programs/` e o resto são GPL-2.0-or-later (**não compilados**). Copyrights nos arquivos compilados: `lz4.c` (C) 2011-2023, `lz4hc.c` e `lz4frame.c` 2011-2020, `xxhash.c` 2012-2016, todos Yann Collet; `lz4file.c` (C) 2022 **Xiaomi Inc.**, também BSD-2-Clause (cabeçalho do arquivo). O `lib/LICENSE` do LZ4 traz o aviso "LZ4 Library, Copyright (c) 2011-2020, Yann Collet".

## 3. O que as licenças exigem

- **Redistribuir binário (BSD-3 do Zstd, BSD-2 do LZ4, MIT do divsufsort):** reproduzir o aviso de copyright, a lista de condições e o aviso de isenção na documentação ou em outros materiais que acompanhem a distribuição. Vale para o `sw-pack`, para a GDExtension e para qualquer binário que linke o `sw-pack`/codec. Cumprimos entregando os três textos em `third_party/licenses/` e listando-os no `NOTICE`.
- **Redistribuir fonte:** manter o aviso nos arquivos (o FetchContent baixa o fonte original e não o copiamos para o repositório, então nada a fazer agora; se um dia o fonte for vendorizado, manter os cabeçalhos).
- **Zstd, 3ª cláusula:** não usar os nomes Facebook/Meta nem de contribuintes para promover o produto sem permissão.
- Sem exigência de abrir o código do Swarmstream (BSD/MIT são permissivas), sem cláusula de patentes na BSD do Zstd (o tarball 1.5.6 não traz arquivo PATENTS; não analisei patentes).

## 4. PROPOSTA — aguarda aprovação do dono (NÃO aplicada ao `NOTICE`)

Substituir, no `NOTICE`, a frase *"No third-party library source code is included in this repository yet. Dependencies are fetched by vcpkg at build time."* por este texto e manter o parágrafo da regra:

```
-------------------------------------------------------------------------
Third-party libraries
-------------------------------------------------------------------------

The sw-pack tool and every binary that links it contain the libraries
below. Their source is NOT in this repository: CMake downloads the exact
tarballs given here (checked by SHA-256) when building. Full license
texts are in third_party/licenses/ and must accompany any binary
distribution.

  Zstandard 1.5.6 ............................... BSD-3-Clause
      Copyright (c) Meta Platforms, Inc. and affiliates.
      https://github.com/facebook/zstd
      tarball SHA-256 30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7
      Zstandard is offered under BSD-3-Clause or GPL-2.0, at the user's
      option; Swarmstream uses it under BSD-3-Clause. Only lib/ is built
      (no programs/, no contrib/).
      Text: third_party/licenses/zstd-1.5.6-LICENSE-BSD.txt
      Includes libdivsufsort-lite ........ MIT
          Copyright (c) 2003-2008 Yuta Mori
          Text: third_party/licenses/zstd-1.5.6-divsufsort-LICENSE-MIT.txt
      Includes code Copyright (c) 2016 Tino Reichardt (threading) and
      xxHash, Copyright (c) Yann Collet, under the same BSD-3-Clause.

  LZ4 1.10.0 (library only, lib/) ............... BSD-2-Clause
      Copyright (c) 2011-2023, Yann Collet
      (lz4file.c: Copyright (c) 2022, Xiaomi Inc.)
      https://github.com/lz4/lz4
      tarball SHA-256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
      The LZ4 programs/ directory is GPL-2.0-or-later and is NOT built or
      distributed.
      Text: third_party/licenses/lz4-1.10.0-lib-LICENSE-BSD-2-Clause.txt
```

Os textos em `third_party/licenses/` são cópias dos arquivos reais (Zstd: `LICENSE`; LZ4: `lib/LICENSE`; divsufsort: cabeçalho do `.c`, que não tem arquivo separado). Esta pasta é nova e **não** altera `LICENSE`, `NOTICE` nem `SECURITY.md`; se o dono preferir outro lugar para os textos, é só mover.

## 5. O CI baixa só o que o hash fixa?

Sim para o `sw-pack`: as duas declarações `FetchContent_Declare` usam `URL` + `URL_HASH SHA256=...`; hash diferente derruba a configuração. Observações:
- O hash é do **tarball** gerado pelo GitHub (`archive/refs/tags/...`); o GitHub já mudou o formato desses arquivos no passado, o que quebraria o CI (falha segura, não passa código diferente). Se acontecer, trocar pela URL de release (`zstd-1.5.6.tar.gz` do release, que é estável).
- Nenhum outro download novo: os jobs de ASan/TSan/lint e o vcpkg continuam como antes. O `vcpkg.json` não foi alterado.
- O `sw-godot/extension` (workflow `extension.yml`) não liga o `sw-pack`; não afetado. Não verifiquei se algum outro workflow faz configure da raiz sem rede.

## 6. Riscos

1. **GPL por engano.** Ligar `ZSTD_BUILD_PROGRAMS`, `ZSTD_BUILD_CONTRIB`, `ZSTD_BUILD_TESTS` ou `LZ4_BUILD_CLI` compilaria código GPL. Estão `OFF` (com `FORCE`) em `sw-pack/CMakeLists.txt`. Ninguém deveria vendorizar `programs/` do LZ4 nem copiar trechos de `contrib/`.
2. **Escolha da licença do Zstd** precisa ficar registrada (§2): sem o `NOTICE` dizendo "usado sob BSD-3-Clause", um terceiro poderia supor GPL.
3. **divsufsort (MIT)** e **threading (Tino Reichardt)**: avisos extras fáceis de esquecer; estão na proposta.
4. **Código de terceiros em testes:** o `test_codec.cpp` usa só `zstd.h` (ligado, não copiado) e um vetor de bytes (`zeros_4096`) gerado pelo projeto (`gen_vectors.py`), sem trecho de terceiros. O Python de vetores usa `zstandard`/`lz4` só localmente, não distribuído.
5. **Binários já distribuídos:** a regra do `NOTICE` ("o PR que adiciona dependência enviada em binário atualiza este arquivo") vale: este PR só deve entrar na `main` junto com a decisão do dono sobre a §4.
6. **Download em tempo de build:** exige rede; reprodutibilidade depende de o GitHub manter os tarballs (hash protege contra troca, não contra remoção).
