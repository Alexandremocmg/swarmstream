# PR #18 (F1-04): aviso e licença do libsodium — diff para aprovação do dono

Head do branch `feat/F1-04-manifesto-v0`: `ad4feec6bd60a9d65a45a42fba49adad20e8c8a1`. Gerado em 2026-10-27 lendo os blobs do git. **O PR #18 não foi mesclado e não há autorização de merge.** A F1-04 continua EM_REVISAO.

## 1. Origem do texto da licença

- Lido com `gh api repos/jedisct1/libsodium/contents/LICENSE?ref=1.0.22-RELEASE`, sem baixar tarball.
- `git hash-object` do arquivo local = `95c22fbadfa457cd7898ebe96aaa05147589b655`, igual ao blob do GitHub (relato da sessão).
- A tag `1.0.22-RELEASE` é uma tag anotada (objeto `bc5892b`). O `vcpkg` usa o port `libsodium` 1.0.22#1, baseline `e456309491fd875487bf02b8a0dca76c1b1b00cc`.
- O CI ganhou um passo **informativo** (Linux, `build+test`): imprime `share/libsodium/copyright` instalado pelo `vcpkg` e compara por `diff` com o arquivo guardado; **não falha o job**, e ainda não foi visto rodar.

## 2. Diff exato do `NOTICE` (contra a `main` atual)

```diff
diff --git a/NOTICE b/NOTICE
index 8f77df8..45f92f4 100644
--- a/NOTICE
+++ b/NOTICE
@@ -46,5 +46,38 @@ Dependencies are fetched by vcpkg at build time. Binary distributions must
 carry the notices of the libraries they link; the expected set is listed in
 docs/adr/0008-licenca-open-source.md.
 
+  libsodium 1.0.22 .............................. ISC
+      Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>
+      https://libsodium.org  (source: https://github.com/jedisct1/libsodium,
+      tag 1.0.22-RELEASE)
+      Used by: sw-pack (Ed25519 signature and SHA-256 of the content manifest,
+      docs/adr/0012-serializacao-do-manifesto.md).
+      How it is obtained: NOT vendored. vcpkg builds it at build time from the
+      manifest (vcpkg.json: dependency "libsodium", builtin-baseline
+      e456309491fd875487bf02b8a0dca76c1b1b00cc, port libsodium 1.0.22#1). CI jobs
+      that do not use vcpkg use the operating-system package (libsodium-dev), so
+      the version there is the distribution's.
+      Full license text (the LICENSE file of tag 1.0.22-RELEASE, git blob
+      95c22fbadfa457cd7898ebe96aaa05147589b655):
+        third_party/licenses/libsodium-1.0.22-LICENSE-ISC.txt
+      Any binary distribution that links libsodium (tools, launcher, game
+      integration) must carry this notice and that license text.
+      The library embeds code under other terms, with their own notices in the
+      libsodium source files (found by reading representative files of tag
+      1.0.22-RELEASE; not an exhaustive audit of every file):
+        - SHA-256 and SHA-512 reference implementations
+          (src/libsodium/crypto_hash/sha256/cp, sha512/cp): BSD-2-Clause,
+          Copyright 2005,2007,2009 Colin Percival. These are the SHA-256 used
+          through crypto_hash_sha256_*.
+        - scrypt (src/libsodium/crypto_pwhash/scryptsalsa208sha256): BSD-style,
+          Copyright 2013 Alexander Peslyak (and files by Colin Percival).
+        - Argon2 reference code (crypto_pwhash/argon2): CC0 1.0, Daniel Dinu and
+          Dmitry Khovratovich, 2015.
+        - BLAKE2b reference code (crypto_generichash/blake2b/ref): CC0 / public
+          domain dedication.
+      Of these, only SHA-256 (and SHA-512, used inside Ed25519) is called by
+      this project. The BSD notices must be reproduced in binary distributions
+      that carry the statically linked library.
+
 Rule: the pull request that vendors third-party code, or that adds a
 dependency shipped inside a binary distribution, updates this file.
```

## 3. Texto integral da licença (`third_party/licenses/libsodium-1.0.22-LICENSE-ISC.txt`, 823 bytes)

```text
/*
 * ISC License
 *
 * Copyright (c) 2013-2026
 * Frank Denis <j at pureftpd dot org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
```

## 4. Pontos para a sua decisão

1. **Avisos de componentes embutidos.** A sessão leu cabeçalhos de 9 arquivos representativos do tag (**não é auditoria de todos**). Encontrou: SHA-256 e SHA-512 de referência (`crypto_hash/sha256/cp` e `sha512/cp`), BSD-2-Clause, Copyright 2005, 2007, 2009 Colin Percival (**é o SHA-256 que usamos** via `crypto_hash_sha256_*`); scrypt (Alexander Peslyak, BSD-style); Argon2 e BLAKE2b (CC0). Nos arquivos ed25519 ref10, poly1305 donna e siphash que ela leu, não achou cabeçalho de licença próprio. Hoje os avisos BSD aparecem só **descritos** no `NOTICE`. **Proposta:** adicionar a `third_party/licenses/` o texto integral dos avisos BSD da Percival e da Peslyak, copiados dos arquivos do tag (por `gh api`, com o SHA do blob), porque a BSD exige reproduzir o aviso em binários que carreguem a biblioteca. Você aprova?
2. **Auditoria completa dos cabeçalhos do libsodium.** A proposta acima cobre só o que foi lido. Uma varredura de todos os arquivos do tag por script (como no #17) reduziria o risco. Quer que eu peça?
3. **Conflito com o #17.** O #17 e o #18 editam a mesma seção do `NOTICE` e também `sw-pack/CMakeLists.txt` e `sw-pack/README.md` (a união dos dois dá conflito de conteúdo nos três). O **segundo a entrar** precisa fazer merge da `main`, resolver mantendo os dois lados, e passa a ter head novo, novo CI e nova autorização sua. Qual dos dois entra primeiro?
4. **Erro no relato da sessão.** Ela disse que o #17 não editou o `NOTICE` no branch dele. Editou (commit `d845100`). O conflito acima é real.

## 5. O que este diff NÃO resolve

Os critérios de tempo, tamanho e memória do manifesto continuam pendentes (F1-58). Nada foi remedido.
