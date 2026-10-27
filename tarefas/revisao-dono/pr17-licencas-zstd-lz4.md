# PR #17 (F1-03): licenças de Zstd e LZ4 — versão final para autorização do dono

Head do branch `feat/F1-03-compressao-por-bloco`: `d845100a8db0ae625c1dcfe0f760d9bbf37219e8`. Gerado em 2026-10-27 lendo os blobs do git. O PR #17 continua em **rascunho**. Nada foi mesclado na `main`.

Você aprovou: BSD-3-Clause (Zstd), BSD-2-Clause (LZ4 lib), a pasta `third_party/licenses/` e o acompanhamento dos avisos pelos binários. Esta versão corrige os pontos que você pediu.

## 1. O que foi corrigido (seus 4 ajustes)

1. **MIT do divsufsort completo.** O texto estava truncado depois de "ARISING". Agora é o cabeçalho inteiro de `divsufsort.c`, até "OTHER DEALINGS IN THE SOFTWARE.", e a sessão relata `diff` vazio contra o original.
2. **Textos conferidos contra as versões usadas.** Relato da sessão: `diff` vazio de `zstd-src/LICENSE`, `lz4-src/lib/LICENSE` e do cabeçalho do `divsufsort.c`. Os dois arquivos `*-file-notices.txt` foram extraídos por script dos cabeçalhos reais e **não** passaram por `diff`.
3. **Avisos internos conferidos (por script, não arquivo por arquivo):** Zstd: 63 arquivos Meta/FSE (51 + 12) com a dupla BSD/GPLv2; `threading.c/.h` (Tino Reichardt, dupla BSD/GPLv2); `xxhash.c/.h` do Zstd (Yann Collet/Meta, dupla BSD/GPLv2); `divsufsort.c/.h` (MIT, Yuta Mori). LZ4: `lz4`, `lz4hc` e `lz4frame` (Yann Collet, BSD-2); `lz4file.c/.h` (Xiaomi 2022, BSD-2); `xxhash.c/.h` do LZ4 (Yann Collet 2012-2016, **BSD-2, não é dupla**). Não existe `lib/` do Zstd fora dessas famílias.
4. **`NOTICE` descreve o que é incorporado e como os textos acompanham os pacotes**, e diz que **ainda não existe mecanismo de empacotamento**. A frase "escolher a GPL obrigaria todo o sw-core/jogo a ser GPL" saiu de `docs/licencas-zstd-lz4.md` (era a única ocorrência).

## 2. Diff exato do `NOTICE`

```diff
diff --git a/NOTICE b/NOTICE
index 8f77df8..abeacd9 100644
--- a/NOTICE
+++ b/NOTICE
@@ -41,10 +41,35 @@ origin (repository and commit) of each one is in .claude/skills/FONTES.md.
 Third-party libraries
 -------------------------------------------------------------------------
 
-No third-party library source code is included in this repository yet.
-Dependencies are fetched by vcpkg at build time. Binary distributions must
-carry the notices of the libraries they link; the expected set is listed in
-docs/adr/0008-licenca-open-source.md.
+No third-party library source code is included in this repository. The
+sw-pack tool (and any binary that links it) contains the static libraries below, compiled from
+the lib/ directory of tarballs that CMake downloads at build time and checks
+by SHA-256 (see sw-pack/CMakeLists.txt). Nothing from programs/ or contrib/
+of either project is built.
+
+  Zstandard 1.5.6 ............................... BSD-3-Clause
+      Copyright (c) Meta Platforms, Inc. and affiliates.
+      https://github.com/facebook/zstd
+      tarball SHA-256 30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7
+      Zstandard is offered under BSD-3-Clause or GPL-2.0 at the user's
+      option; Swarmstream uses it under BSD-3-Clause. Only lib/ is built.
+      Contained in lib/: libdivsufsort-lite, MIT, Copyright (c) 2003-2008
+      Yuta Mori; code Copyright (c) 2016 Tino Reichardt and xxHash,
+      Copyright (c) Yann Collet - Meta Platforms, Inc, both under the same
+      BSD-3-Clause/GPL-2.0 choice.
+  LZ4 1.10.0 (library, lib/) .................... BSD-2-Clause
+      Copyright (c) 2011-2023, Yann Collet; lz4file.c Copyright (c) 2022,
+      Xiaomi Inc.; xxhash.c Copyright (c) 2012-2016, Yann Collet.
+      https://github.com/lz4/lz4
+      tarball SHA-256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
+      (The LZ4 programs/ directory is GPL-2.0-or-later and is not built.)
+
+The license texts and file notices, copied unchanged from those versions,
+are in third_party/licenses/. Redistribution of a binary that contains these
+libraries must reproduce them: third_party/licenses/ and this NOTICE must go
+inside every package or installer. No packaging or installer mechanism
+exists yet; when one is added it must include them. Details and checks:
+docs/licencas-zstd-lz4.md.
 
 Rule: the pull request that vendors third-party code, or that adds a
 dependency shipped inside a binary distribution, updates this file.
```

Observações de forma (não de conteúdo): falta uma linha em branco entre as entradas Zstd e LZ4, e há uma linha longa no primeiro parágrafo. Posso pedir a correção cosmética.

## 3. Arquivos que entram em `third_party/licenses/`

### `third_party/licenses/zstd-1.5.6-LICENSE-BSD.txt` (1549 bytes)
Zstandard 1.5.6, BSD-3-Clause: arquivo LICENSE do tarball (diff vazio contra o original)

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

### `third_party/licenses/zstd-1.5.6-divsufsort-LICENSE-MIT.txt` (1188 bytes)
libdivsufsort-lite dentro do Zstd, MIT: cabeçalho COMPLETO de lib/dictBuilder/divsufsort.c (diff vazio contra o original; antes estava truncado)

```text
/*
 * divsufsort.c for libdivsufsort-lite
 * Copyright (c) 2003-2008 Yuta Mori All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
```

### `third_party/licenses/lz4-1.10.0-lib-LICENSE-BSD-2-Clause.txt` (1311 bytes)
LZ4 1.10.0, só lib/, BSD-2-Clause: arquivo lib/LICENSE (diff vazio contra o original)

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

### `third_party/licenses/zstd-1.5.6-file-notices.txt` (4664 bytes)
cabeçalhos de copyright e licença dos arquivos do Zstd, extraídos por script (NÃO conferidos por diff, conferência por script)

```text
Cabecalhos de copyright e licenca, copiados sem alteracao dos arquivos do Zstd 1.5.6 (tarball SHA-256 30f35f71...92ff7). O primeiro arquivo de cada grupo e reproduzido; os demais do grupo tem o mesmo cabecalho a menos do titulo.

========================================================================
Meta Platforms (padrao dos arquivos de lib/common, lib/compress, lib/decompress, lib/dictBuilder; BSD ou GPLv2 a escolha; usado sob BSD)
  zstd-src/lib/zstd.h
========================================================================
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

========================================================================
Variante FSE/Huff0 (copyright Meta; mesma dupla de licencas)
  zstd-src/lib/common/fse.h
========================================================================
/* ******************************************************************
 * FSE : Finite State Entropy codec
 * Public Prototypes declaration
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * You can contact the author at :
 * - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
****************************************************************** */

========================================================================
Tino Reichardt (threading.c e threading.h; mesma dupla de licencas; usado sob BSD)
  zstd-src/lib/common/threading.c
========================================================================
/**
 * Copyright (c) 2016 Tino Reichardt
 * All rights reserved.
 *
 * You can contact the author at:
 * - zstdmt source repository: https://github.com/mcmilk/zstdmt
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

========================================================================
xxHash dentro do Zstd (Yann Collet / Meta; mesma dupla de licencas; usado sob BSD)
  zstd-src/lib/common/xxhash.c
========================================================================
/*
 * xxHash - Extremely Fast Hash algorithm
 * Copyright (c) Yann Collet - Meta Platforms, Inc
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

========================================================================
libdivsufsort-lite (MIT): texto em zstd-1.5.6-divsufsort-LICENSE-MIT.txt
  zstd-src/lib/dictBuilder/divsufsort.h
========================================================================
/*
 * divsufsort.h for libdivsufsort-lite
 * Copyright (c) 2003-2008 Yuta Mori All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
```

### `third_party/licenses/lz4-1.10.0-file-notices.txt` (5687 bytes)
idem para o LZ4, incluindo Xiaomi/lz4file e xxhash do LZ4 (NÃO conferidos por diff)

```text
Cabecalhos de copyright e licenca, copiados sem alteracao dos arquivos do LZ4 1.10.0 (tarball SHA-256 53751290...5e8). Os demais arquivos de lib/ (lz4hc, lz4frame, .h) mudam apenas o titulo e os anos de copyright (Yann Collet, 2011-2023) e tem o mesmo texto BSD-2-Clause.

========================================================================
LZ4 (Yann Collet; BSD-2-Clause)
  lz4-src/lib/lz4.c
========================================================================
/*
   LZ4 - Fast LZ compression algorithm
   Copyright (C) 2011-2023, Yann Collet.

   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
    - LZ4 homepage : http://www.lz4.org
    - LZ4 source repository : https://github.com/lz4/lz4
*/

========================================================================
LZ4 file library (Xiaomi Inc.; BSD-2-Clause)
  lz4-src/lib/lz4file.c
========================================================================
/*
 * LZ4 file library
 * Copyright (C) 2022, Xiaomi Inc.
 *
 * BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You can contact the author at :
 * - LZ4 homepage : http://www.lz4.org
 * - LZ4 source repository : https://github.com/lz4/lz4
 */

========================================================================
xxHash dentro do LZ4 (Yann Collet; BSD-2-Clause)
  lz4-src/lib/xxhash.c
========================================================================
/*
*  xxHash - Fast Hash algorithm
*  Copyright (C) 2012-2016, Yann Collet
*
*  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions are
*  met:
*
*  * Redistributions of source code must retain the above copyright
*  notice, this list of conditions and the following disclaimer.
*  * Redistributions in binary form must reproduce the above
*  copyright notice, this list of conditions and the following disclaimer
*  in the documentation and/or other materials provided with the
*  distribution.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
*  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
*  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
*  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
*  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
*  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
*  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
*  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*  You can contact the author at :
*  - xxHash homepage: http://www.xxhash.com
*  - xxHash source repository : https://github.com/Cyan4973/xxHash
*/
```

## 4. O que NÃO está resolvido nem foi verificado

- Os dois `*-file-notices.txt` não passaram por `diff` (foram gerados por script a partir dos cabeçalhos reais).
- A conferência dos cabeçalhos internos foi por script, não arquivo por arquivo.
- Não há mecanismo de empacotamento ainda; o `NOTICE` registra que, quando existir, precisa incluir `third_party/licenses/` e o `NOTICE`.
- Não é parecer jurídico; o ADR-0008 recomenda consulta jurídica antes de publicar.
- O PR #18 (libsodium) edita a mesma seção do `NOTICE`; o segundo a entrar faz merge da `main` e mantém os dois avisos.
