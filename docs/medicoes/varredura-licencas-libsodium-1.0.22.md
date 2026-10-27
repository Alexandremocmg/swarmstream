# Varredura de licenças do libsodium 1.0.22 (F1-04)

- Arquivos no tag (blobs): 714; binarios ignorados (NUL nos 8 KiB iniciais): 1
- Arquivos de texto varridos: 713
- Criterio: qualquer linha com marcador `copyright|licen[sc]e|permission is hereby|redistribution|public domain|\bCC0\b|\bGPL|\bBSD\b|\bISC\b|\bMIT\b` (sem distinguir maiusculas)
- Arquivos com ao menos um marcador: 72


Gerado por `python -I third_party/licenses/scan_libsodium_licenses.py 1.0.22-RELEASE <cache>` em 2026-10-27
(árvore do tag lida por `gh api .../git/trees/1.0.22-RELEASE?recursive=1`, sem truncar; cada blob lido por
`gh api .../git/blobs/<sha>`, sem baixar tarball; os 3 maiores precisaram de `--allow-escape-sequences`).

## Alcance da conferência (o que esta varredura é e o que não é)
- **Cobre** os 714 blobs do tag; 713 de texto varridos linha a linha pelo critério acima. **Não cobre:** o
  1 binário ignorado; arquivos que tenham licença **sem** nenhuma dessas palavras (o critério é por palavra-chave,
  não entende o texto); dependências do libsodium (não tem); o que o vcpkg aplica por cima (patches do port).
- **Não julga** a licença: o script lista; a leitura dos textos dos blocos aplicáveis foi feita por mim e está
  nos arquivos de `third_party/licenses/` (cada um com caminho e blob SHA). Uma pessoa deve conferir.
- Arquivos da biblioteca **sem** marcador (ex.: Ed25519 ref10, Poly1305, a maior parte do código) ficam sob
  a licença ISC do projeto (`LICENSE`); isso é **inferência** pela ausência de aviso próprio, não prova.

## Resultado por componente (arquivos de `src/libsodium/*.c|*.h` com aviso diferente do ISC)
| Componente | Licença/aviso | Arquivos | Texto preservado em |
| --- | --- | --- | --- |
| SHA-256 e SHA-512 de referência (Colin Percival) | BSD-2-Clause | `crypto_hash/sha256/cp`, `crypto_hash/sha512/cp` | `third_party/licenses/libsodium-1.0.22-BSD-Percival.txt` |
| PBKDF2-SHA256 do scrypt (Colin Percival) | BSD-2-Clause | `pbkdf2-sha256.c/.h` | idem |
| scrypt (Percival + Alexander Peslyak) | BSD-2-Clause (Percival) e BSD "redistribuição permitida" (Peslyak) | `crypto_scrypt*.{c,h}`, `nosse/`, `sse/`, `scrypt_platform.c` | `libsodium-1.0.22-BSD-Peslyak-scrypt.txt` |
| BLAKE2b (Samuel Neves) | CC0 (também OpenSSL ou Apache-2.0, à escolha) | `crypto_generichash/blake2b/ref/*` | `libsodium-1.0.22-dominio-publico-CC0.txt` |
| Argon2 (Dinu e Khovratovich) e codificação (Thomas Pornin) | CC0 | `crypto_pwhash/argon2/*` | idem |
| Salsa20, ChaCha20, HSalsa20 de referência (D. J. Bernstein) | domínio público | `crypto_stream/*/ref`, `crypto_core/hsalsa20/ref2` | idem |

Arquivos fora da biblioteca com avisos de terceiros (todos **não compilados**, não entram em binário): macros e
scripts do autotools (`aclocal.m4`, `configure`, `m4/*`, `build-aux/*`, `Makefile.in`: GNU, com exceção de
configuração/FSF permissiva), `contrib/Findsodium.cmake` (CC0 do autor), `packaging/nuget/package.gsl`
(MIT/X11 do iMatix), recursos do MSVC. O vcpkg usa o **tarball de distribuição**, em que `configure` e `Makefile.in`
são gerados e **não** entram no `.a`/`.dll`/`.so`.

## Compilado/incorporado no nosso uso
- **Entram na biblioteca** `libsodium` que o vcpkg compila: todos os `.c` de `src/libsodium` com o aviso
  acima (o port compila a biblioteca inteira). **O que entra no nosso executável** depende do tipo de ligação:
  em ligação estática só vão os objetos referenciados; em dinâmica, a DLL/.so inteira acompanha o produto.
  **Não verifiquei por tabela de símbolos** quais objetos o `sw-pack` arrasta; o que sei é que usamos
  `crypto_hash_sha256_*` (**Percival, BSD-2**), `crypto_sign_ed25519_*` (que usa SHA-512 de **Percival**) e `sodium_init`.
  Por isso **tratamos todos os avisos da tabela como aplicáveis** ao que distribuirmos junto com a libsodium.
- Obrigação prática (BSD-2 e ISC): binários distribuídos reproduzem o aviso e o texto. Estão em `NOTICE`
  e `third_party/licenses/`. CC0/domínio público não exigem texto, mas foram preservados.

## Tabela gerada (todos os achados)
| Arquivo | Escopo | Componente | Blob | Linhas (n de marcadores; 1a) |
| --- | --- | --- | --- | --- |
| .github/workflows/dotnet-core.yml | outro (nao compilado) | .github | 7422b57188 | 1; L265: run: cp AUTHORS ChangeLog LICENSE packaging/dotnet-core/libsodium.pkgproj .libsodium-pack/ |
| CITATION.cff | raiz do repositorio (documentacao/build, nao compilado) | CITATION.cff | 2090280747 | 1; L18: license: ISC |
| LICENSE | raiz do repositorio (documentacao/build, nao compilado) | LICENSE | 95c22fbadf | 3; L2: * ISC License |
| Makefile.am | build (gerado/script do autotools, nao compilado) | Makefile.am | 7fc179281c | 1; L8: LICENSE \ |
| Makefile.in | build (gerado/script do autotools, nao compilado) | Makefile.in | 5db821d2e2 | 2; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| README.markdown | raiz do repositorio (documentacao/build, nao compilado) | README.markdown | 5e7c352092 | 2; L78: ## License |
| aclocal.m4 | build (gerado/script do autotools, nao compilado) | aclocal.m4 | 16139f0802 | 28; L3: # Copyright (C) 1996-2025 Free Software Foundation, Inc. |
| build-aux/compile | build (gerado pelo autotools, nao compilado) | build-aux | 02ff093c33 | 6; L6: # Copyright (C) 1999-2025 Free Software Foundation, Inc. |
| build-aux/config.guess | build (gerado pelo autotools, nao compilado) | build-aux | 48a684601b | 40; L3: #   Copyright 1992-2024 Free Software Foundation, Inc. |
| build-aux/config.sub | build (gerado pelo autotools, nao compilado) | build-aux | 3d35cde174 | 24; L3: #   Copyright 1992-2025 Free Software Foundation, Inc. |
| build-aux/depcomp | build (gerado pelo autotools, nao compilado) | build-aux | 9f6725b9e0 | 6; L6: # Copyright (C) 1999-2025 Free Software Foundation, Inc. |
| build-aux/install-sh | build (gerado pelo autotools, nao compilado) | build-aux | 1d8d966964 | 8; L6: # This originates from X11R5 (mit/util/scripts/install.sh), which was |
| build-aux/ltmain.sh | build (gerado/script do autotools, nao compilado) | build-aux | 3e6a3db3a5 | 29; L7: # Written by Gordon Matzigkeit <gord@gnu.ai.mit.edu>, 1996 |
| build-aux/missing | build (gerado pelo autotools, nao compilado) | build-aux | 5e450bab3c | 6; L8: # Copyright (C) 1996-2025 Free Software Foundation, Inc. |
| build-aux/test-driver | build (gerado pelo autotools, nao compilado) | build-aux | d81fba79cd | 6; L6: # Copyright (C) 2011-2025 Free Software Foundation, Inc. |
| builds/Makefile.in | build (gerado/script do autotools, nao compilado) | builds | 2679d0a092 | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| builds/msvc/resource.rc | build/CI (nao compilado) | builds | ddd177c4ff | 1; L52: VALUE "LegalCopyright", "Copyright (c) 2013-2026 The libsodium authors." |
| configure | raiz do repositorio (documentacao/build, nao compilado) | configure | 34ed561c5d | 31; L8: # Copyright (C) 1992-1996, 1998-2017, 2020-2026 Free Software Foundation, |
| contrib/Findsodium.cmake | contrib (nao compilado) | contrib | 316c5e94a3 | 3; L3: # To the extent possible under law, the author(s) have dedicated all copyright |
| contrib/Makefile.in | build (gerado/script do autotools, nao compilado) | contrib | 9b98b076e3 | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| dist-build/Makefile.in | build (gerado/script do autotools, nao compilado) | dist-build | 422818143f | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| dist-build/android-aar.sh | build (gerado/script do autotools, nao compilado) | dist-build | 2984ebea2d | 1; L84: cp "LICENSE" "${DEST_PATH}/META-INF" |
| m4/ax_add_fortify_source.m4 | build (gerado/script do autotools, nao compilado) | m4 | 860c159853 | 4; L29: # LICENSE |
| m4/ax_check_compile_flag.m4 | build (gerado/script do autotools, nao compilado) | m4 | b197d06a8e | 4; L27: # LICENSE |
| m4/ax_check_define.m4 | build (gerado/script do autotools, nao compilado) | m4 | c10d1137ae | 3; L17: # LICENSE |
| m4/ax_check_gnu_make.m4 | build (gerado/script do autotools, nao compilado) | m4 | 785dc96daf | 4; L62: # LICENSE |
| m4/ax_check_link_flag.m4 | build (gerado/script do autotools, nao compilado) | m4 | 06af181b9d | 13; L27: # LICENSE |
| m4/ax_pthread.m4 | build (gerado/script do autotools, nao compilado) | m4 | 9f35d13914 | 15; L58: # LICENSE |
| m4/ax_tls.m4 | build (gerado/script do autotools, nao compilado) | m4 | 1af4f32f64 | 13; L16: # LICENSE |
| m4/ax_valgrind_check.m4 | build (gerado/script do autotools, nao compilado) | m4 | 876f887689 | 3; L61: # LICENSE |
| m4/ld-output-def.m4 | build (gerado/script do autotools, nao compilado) | m4 | 59895bc244 | 1; L2: dnl Copyright (C) 2008-2013 Free Software Foundation, Inc. |
| m4/libtool.m4 | build (gerado/script do autotools, nao compilado) | m4 | 8d323b3ee4 | 23; L3: #   Copyright (C) 1996-2001, 2003-2019, 2021-2024 Free Software |
| m4/ltoptions.m4 | build (gerado/script do autotools, nao compilado) | m4 | 25caa89029 | 1; L3: #   Copyright (C) 2004-2005, 2007-2009, 2011-2019, 2021-2024 Free |
| m4/ltsugar.m4 | build (gerado/script do autotools, nao compilado) | m4 | 48bc9344a4 | 1; L3: # Copyright (C) 2004-2005, 2007-2008, 2011-2015 Free Software |
| m4/ltversion.m4 | build (gerado/script do autotools, nao compilado) | m4 | 228df3f39b | 1; L3: #   Copyright (C) 2004, 2011-2019, 2021-2024 Free Software Foundation, |
| m4/lt~obsolete.m4 | build (gerado/script do autotools, nao compilado) | m4 | c6b26f88f6 | 1; L3: #   Copyright (C) 2004-2005, 2007, 2009, 2011-2015 Free Software |
| packaging/dotnet-core/libsodium.pkgproj | outro (nao compilado) | packaging | 1501527571 | 4; L15: <Copyright>&#169; $([System.DateTime]::UtcNow.ToString(yyyy)) Frank Denis</Copyright> |
| packaging/nuget/package.gsl | outro (nao compilado) | packaging | 5b255e5e0e | 4; L5: .#  is licensed under MIT/X11. |
| src/Makefile.in | build (gerado/script do autotools, nao compilado) | src | 187e1cdb13 | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| src/libsodium/Makefile.in | build (gerado/script do autotools, nao compilado) | src | a4678afbce | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| src/libsodium/crypto_core/hsalsa20/ref2/core_hsalsa20_ref2.c | biblioteca (compilada na libsodium) | crypto_core/hsalsa20 | 1d1220fee2 | 1; L4: Public domain. |
| src/libsodium/crypto_generichash/blake2b/ref/blake2.h | biblioteca (compilada na libsodium) | crypto_generichash/blake2b | edfc99a245 | 6; L6: To the extent possible under law, the author(s) have dedicated all copyright |
| src/libsodium/crypto_generichash/blake2b/ref/blake2b-load-sse2.h | biblioteca (compilada na libsodium) | crypto_generichash/blake2b | 8e67421aca | 3; L6: To the extent possible under law, the author(s) have dedicated all copyright |
| src/libsodium/crypto_generichash/blake2b/ref/blake2b-load-sse41.h | biblioteca (compilada na libsodium) | crypto_generichash/blake2b | 31745fc139 | 3; L6: To the extent possible under law, the author(s) have dedicated all copyright |
| src/libsodium/crypto_generichash/blake2b/ref/blake2b-ref.c | biblioteca (compilada na libsodium) | crypto_generichash/blake2b | 19008f54ac | 3; L6: To the extent possible under law, the author(s) have dedicated all copyright |
| src/libsodium/crypto_hash/sha256/cp/hash_sha256_cp.c | biblioteca (compilada na libsodium) | crypto_hash/sha256 | 394c391410 | 4; L3: * Copyright 2005,2007,2009 Colin Percival |
| src/libsodium/crypto_hash/sha512/cp/hash_sha512_cp.c | biblioteca (compilada na libsodium) | crypto_hash/sha512 | a36841b983 | 4; L3: * Copyright 2005,2007,2009 Colin Percival |
| src/libsodium/crypto_pwhash/argon2/argon2-core.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | d1274ba382 | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-core.h | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | d544f3ebe6 | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-encoding.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | 3c9b4927a1 | 5; L15: * should amount to Public Domain or its closest equivalent; the |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-avx2.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | 5c7956632b | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-avx512f.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | 8629fdf51b | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-neon.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | 3e3fec7aac | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-ref.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | d93daee19a | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-ssse3.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | 1930bc35f3 | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2-fill-block-wasm32.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | d8d65d8b43 | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2.c | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | ddc55c2c58 | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/argon2/argon2.h | biblioteca (compilada na libsodium) | crypto_pwhash/argon2 | a1014a470a | 2; L6: * This work is licensed under a Creative Commons CC0 1.0 License/Waiver. |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/crypto_scrypt-common.c | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 2abb5897e0 | 2; L2: * Copyright 2013 Alexander Peslyak |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/crypto_scrypt.h | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 1224c0f132 | 5; L2: * Copyright 2009 Colin Percival |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/nosse/pwhash_scryptsalsa208sha256_nosse.c | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 9ecaf52f58 | 5; L2: * Copyright 2009 Colin Percival |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/pbkdf2-sha256.c | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | bcf7da553a | 4; L2: * Copyright 2005,2007,2009 Colin Percival |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/pbkdf2-sha256.h | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 962b43311d | 4; L2: * Copyright 2005,2007,2009 Colin Percival |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/scrypt_platform.c | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 890517f797 | 2; L2: * Copyright 2013 Alexander Peslyak |
| src/libsodium/crypto_pwhash/scryptsalsa208sha256/sse/pwhash_scryptsalsa208sha256_sse.c | biblioteca (compilada na libsodium) | crypto_pwhash/scryptsalsa208sha256 | 89fe8aec87 | 5; L2: * Copyright 2009 Colin Percival |
| src/libsodium/crypto_stream/chacha20/ref/chacha20_ref.c | biblioteca (compilada na libsodium) | crypto_stream/chacha20 | 40cccbf8f8 | 1; L5: Public domain. |
| src/libsodium/crypto_stream/salsa20/ref/salsa20_ref.c | biblioteca (compilada na libsodium) | crypto_stream/salsa20 | f0854ebf7e | 1; L4: Public domain. |
| src/libsodium/crypto_stream/salsa2012/ref/stream_salsa2012_ref.c | biblioteca (compilada na libsodium) | crypto_stream/salsa2012 | bfdfeedba3 | 1; L4: Public domain. |
| src/libsodium/crypto_stream/salsa208/ref/stream_salsa208_ref.c | biblioteca (compilada na libsodium) | crypto_stream/salsa208 | 38b3ed5bfd | 1; L4: Public domain. |
| src/libsodium/include/Makefile.in | build (gerado/script do autotools, nao compilado) | include | c5651e8362 | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| test/Makefile.in | build (gerado/script do autotools, nao compilado) | test | 37b1bb5c3b | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
| test/default/Makefile.in | build (gerado/script do autotools, nao compilado) | test | b4ee2b426d | 1; L4: # Copyright (C) 1994-2025 Free Software Foundation, Inc. |
