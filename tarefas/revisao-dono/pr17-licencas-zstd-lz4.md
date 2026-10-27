# PR #17 (F1-03): licenças de Zstd e LZ4 — conteúdo para aprovação do dono

Gerado em 2026-10-27 a partir do branch `feat/F1-03-compressao-por-bloco` (head `ac16bf510df59d3c51c57314a274090061cf835d`), lendo os blobs do git. **Nada disto foi aplicado ao `NOTICE` da `main`.** Os textos de licença abaixo são cópias dos arquivos reais baixados pelo `FetchContent` (a sessão leu `build/_deps/zstd-src` e `lz4-src`); conferi na mesma origem os cabeçalhos do `xxhash.h` e do `threading.c`.

## 1. Dependências: versão, origem e licença escolhida

| | Zstandard | LZ4 |
| --- | --- | --- |
| Versão | 1.5.6 | 1.10.0 |
| Origem | `https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz` | `https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz` |
| SHA-256 do tarball (fixado no CMake) | `30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7` | `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b` |
| Licença oferecida | BSD-3-Clause **ou** GPL-2.0, à escolha de quem usa | `lib/`: BSD-2-Clause. `programs/`: GPL-2.0-or-later |
| **Licença escolhida** | **BSD-3-Clause** (a GPL-2.0 obrigaria a distribuir o `sw-core`/jogo sob GPL) | BSD-2-Clause (não há alternativa; o que usamos é só `lib/`) |

## 2. Componentes efetivamente compilados

- **Zstd:** só `lib/` (common, compress, decompress, dictBuilder), alvo `libzstd_static`. Desligados: `programs/`, `contrib/`, `tests/` e, neste PR, os decoders legados (`ZSTD_LEGACY_SUPPORT=OFF`). O `sw-pack` só chama `ZSTD_compress`, `ZSTD_decompress`, `ZSTD_getFrameContentSize` e `ZSTD_getDictID_fromFrame`, mas todo o `lib/` entra na biblioteca estática.
- **LZ4:** só `lib/` (`lz4.c`, `lz4hc.c`, `lz4frame.c`, `lz4file.c`, `xxhash.c`), alvo `lz4_static`. Desligados: `programs/` (`LZ4_BUILD_CLI=OFF`), `contrib/`, `tests/`, `examples/`.
- **Avisos de terceiros dentro do Zstd `lib/`:** `dictBuilder/divsufsort.c` (MIT, Yuta Mori 2003-2008); `common/threading.c` (Tino Reichardt 2016, mesma dupla BSD/GPL); `common/xxhash.h` (Yann Collet, Meta, mesma dupla BSD/GPL, confirmado no cabeçalho).
- **O que é redistribuído:** a sessão afirma que o aviso deve acompanhar o `sw-pack` e todo binário que linke o codec. **Ponto em aberto (decisão sua):** quais binários o projeto vai de fato distribuir ainda não está definido; a proposta de `NOTICE` cobre o caso mais amplo.

## 3. Diff exato proposto para o `NOTICE`

```diff
--- a/NOTICE
+++ b/NOTICE
@@ -41,10 +41,34 @@
 Third-party libraries
 -------------------------------------------------------------------------
 
-No third-party library source code is included in this repository yet.
-Dependencies are fetched by vcpkg at build time. Binary distributions must
-carry the notices of the libraries they link; the expected set is listed in
-docs/adr/0008-licenca-open-source.md.
+The sw-pack tool and every binary that links it contain the libraries
+below. Their source is NOT in this repository: CMake downloads the exact
+tarballs given here (checked by SHA-256) when building. Full license
+texts are in third_party/licenses/ and must accompany any binary
+distribution.
+
+  Zstandard 1.5.6 ............................... BSD-3-Clause
+      Copyright (c) Meta Platforms, Inc. and affiliates.
+      https://github.com/facebook/zstd
+      tarball SHA-256 30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7
+      Zstandard is offered under BSD-3-Clause or GPL-2.0, at the user's
+      option; Swarmstream uses it under BSD-3-Clause. Only lib/ is built
+      (no programs/, no contrib/).
+      Text: third_party/licenses/zstd-1.5.6-LICENSE-BSD.txt
+      Includes libdivsufsort-lite ........ MIT
+          Copyright (c) 2003-2008 Yuta Mori
+          Text: third_party/licenses/zstd-1.5.6-divsufsort-LICENSE-MIT.txt
+      Includes code Copyright (c) 2016 Tino Reichardt (threading) and
+      xxHash, Copyright (c) Yann Collet, under the same BSD-3-Clause.
+
+  LZ4 1.10.0 (library only, lib/) ............... BSD-2-Clause
+      Copyright (c) 2011-2023, Yann Collet
+      (lz4file.c: Copyright (c) 2022, Xiaomi Inc.)
+      https://github.com/lz4/lz4
+      tarball SHA-256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
+      The LZ4 programs/ directory is GPL-2.0-or-later and is NOT built or
+      distributed.
+      Text: third_party/licenses/lz4-1.10.0-lib-LICENSE-BSD-2-Clause.txt
 
 Rule: the pull request that vendors third-party code, or that adds a
 dependency shipped inside a binary distribution, updates this file.
```

## 4. Arquivos que entrariam em `third_party/licenses/` (textos exatos)

### `third_party/licenses/zstd-1.5.6-LICENSE-BSD.txt`
Zstandard 1.5.6, BSD-3-Clause (arquivo LICENSE do Zstd)

```text
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### `third_party/licenses/zstd-1.5.6-divsufsort-LICENSE-MIT.txt`
libdivsufsort-lite dentro do Zstd, MIT (cabeçalho de lib/dictBuilder/divsufsort.c)

```text
libdivsufsort-lite (embutido no Zstd 1.5.6, lib/dictBuilder/divsufsort.c; licença MIT, texto copiado do cabeçalho do arquivo)

divsufsort.c for libdivsufsort-lite
Copyright (c) 2003-2008 Yuta Mori All Rights Reserved.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
```

### `third_party/licenses/lz4-1.10.0-lib-LICENSE-BSD-2-Clause.txt`
LZ4 1.10.0, só a biblioteca lib/, BSD-2-Clause (arquivo lib/LICENSE do LZ4)

```text
LZ4 Library
Copyright (c) 2011-2020, Yann Collet
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## 5. Riscos e limites

1. **Não é parecer jurídico.** O ADR-0008 recomenda consulta jurídica antes da publicação.
2. **GPL por engano:** ligar `ZSTD_BUILD_PROGRAMS`, `ZSTD_BUILD_CONTRIB`, `ZSTD_BUILD_TESTS` ou `LZ4_BUILD_CLI` compilaria código GPL. Estão `OFF` (com `FORCE`) em `sw-pack/CMakeLists.txt`.
3. **Hash do tarball:** é o tarball gerado pelo GitHub; se o formato mudar, o CI falha (falha segura).
4. **Patentes:** não analisadas.
5. **Relacionado ao #18:** o libsodium (vcpkg) será distribuído em qualquer binário que verifique ou assine manifesto. Ele é ISC; o `NOTICE` precisará do aviso dele, e a proposta virá do texto real do fonte, no #18. Não está nesta proposta.

## 6. Decisão que peço

1. Aprova a escolha de **BSD-3-Clause** para o Zstd?
2. Aprova o diff do `NOTICE` da §3 como está, ou com ajustes (diga quais)?
3. Aprova a pasta `third_party/licenses/` com esses três arquivos?
4. Aprova a cobertura mais ampla ("todo binário que linke o codec") enquanto não se define o que será distribuído?

Com a aprovação, a sessão da F1-03 aplica o diff ao `NOTICE` no branch do PR #17 (um commit), o CI roda e peço a sua autorização do merge para o head final. Sem ela, o PR #17 não entra.
