# Manifesto v0: tamanho, abertura e memória (F1-04)

Medição pedida pelo [ADR-0012](../adr/0012-serializacao-do-manifesto.md) (verificação) e por
[03 §7.3](../03-formato-de-conteudo.md#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo).

## Como foi medido
- **Máquina:** Intel Core i7-7700HQ 2,8 GHz (sem SHA-NI), 19,9 GB de RAM, Windows 11, **não estava parada**
  (navegador e outras sessões abertas; carga de CPU ≈ 30%). Os números são os de três execuções seguidas e estáveis;
  a primeira de cada série oscilou mais (de 690 ms a 2,3 s no manifesto grande) e foi descartada.
- **Data:** 2026-10-27. **Compilador:** g++ 16.2 (MinGW-w64 UCRT), `-DCMAKE_BUILD_TYPE=Release`.
- **Manifesto sintético**, não o mapa da F0-09 (os assets não são versionados). Imita a forma dele:
  blocos de 32 a 256 KiB (média ≈ 144 KiB), 60% do tamanho depois do Zstd, ~12 blocos por arquivo, peças de até 1 MiB,
  três grupos (`core` 10% dos blocos e duas regiões). Gerador: [`tests/manifest_fixture.hpp`](../../sw-pack/tests/manifest_fixture.hpp).
- Dois processos, para o pico de memória de quem gerou não contaminar quem abre:

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-rel --target sw-manifest-bench
B=build-rel/sw-pack/sw-manifest-bench
$B generate --blocks 24576   --out m3.bin     # "mapa de 3 GB"
$B open m3.bin --runs 7
$B generate --blocks 1200000 --out m150.bin   # "150 GB" (164,8 GiB crus)
$B open m150.bin --runs 9
```

> **Atenção (2026-10-27): números de tempo desatualizados.** As medições abaixo foram feitas quando o Ed25519 e o
> SHA-512 eram implementações próprias do `sw-pack`. Por decisão do dono o Ed25519 agora é do **libsodium**
> (ADR-0012). O tamanho do arquivo não muda; a linha da assinatura (245 ms em 150 GB) **tem de ser medida de novo**
> com o libsodium (SHA-512 dele é mais rápido), e o libsodium assina/verifica mensagem contígua, então a abertura
> ganha uma **cópia temporária do payload** (≈ 61 MiB de pico extra e alguns ms de `memcpy`). Esta máquina não tinha
> libsodium nem vcpkg (MinGW), então **a remedição ainda não foi feita**; o procedimento abaixo vale como está.
> O SHA-256 também passou a ser o do libsodium (`crypto_hash_sha256_*`); os 372 ms em 150 GB do `manifest_id`
> eram do SHA-256 próprio e também **precisam ser remedidos**.
> **Remedição dos 150 GB com libsodium: fica para depois do CI verde, numa máquina parada.** O pico de ≈ 61 MiB
> extra na verificação (cópia do payload) é pendência de otimização.

## Resultados
| | mapa ≈ 3 GB | ≈ 150 GB (1,2 M blocos) |
| --- | --- | --- |
| blocos / arquivos / peças | 24 576 / 2 064 / 2 183 | 1 200 000 / 99 935 / 106 644 |
| conteúdo cru | 3,38 GiB | 164,8 GiB |
| **tamanho do arquivo** | **1,26 MiB** (53,9 B por bloco) | **61,6 MiB** (53,8 B por bloco) |
| memória residente depois de abrir (arquivo lido inteiro) | 1,7 MiB acima da base | 63,1 MiB acima da base (pico do processo 81 MiB) |
| **abertura completa** (mediana) | ≈ 15 ms | ≈ 705 ms |
| ↳ assinatura Ed25519 (SHA-512 sobre o arquivo todo) | 7 ms | 245 ms |
| ↳ SHA-256 do `manifest_id` | 7 ms | 372 ms |
| ↳ verificador + validações do §8 | 1,5 ms | 85 ms |

"Abertura completa" = cabeçalho, chave, assinatura, sondagem do `min_reader`, `manifest_id`, verificador e todas as
validações do §8, com o arquivo já na memória (não conta ler o disco).

## O que os números dizem contra os critérios
1. **Tamanho (03 §7.3, meta < 50 MB em 150 GB): estoura.** 61,6 MiB, mais que os ≈ 54 MB estimados. A estimativa de 45 B por
   bloco só contava as colunas de bloco; pelo que foi gerado há também ≈ 4 B por bloco de peças (hash 32 B + 14 B por peça, uma peça
   a cada ≈ 11 blocos) e ≈ 5 B por bloco de arquivos (≈ 57 B por arquivo: caminho de ≈ 30 B mais colunas). Com a saída (b) de §7.3
   (hash de bloco cortado para 16 B) o arquivo cairia para ≈ 43 MiB (cálculo: 61,6 − 1,2 M × 16 B; não medido).
2. **Memória residente:** com o arquivo lido por inteiro ficam 63 MiB residentes, o que passa de 50 MB. O ADR-0012 prevê
   devolver as páginas depois da verificação (mapear + `madvise`/remapear) e medir a residência **depois de uma rota completa**;
   isso é do `sw-core` e **não foi medido aqui**.
3. **Custo de abrir (ADR-0012: reabrir a escolha do FlatBuffers se passar de 300 ms em 150 GB): ≈ 705 ms, acima do limite
   na letra.** Mas 617 ms disso são os dois hashes (SHA-256 do `manifest_id` e SHA-512 dentro do Ed25519), que
   **não dependem do formato**; o que é do formato (verificador e validações) custa 85 ms. Os hashes aqui são implementações
   simples e portáveis (≈ 176 MB/s e 255 MB/s); o OpenSSL 3.5 nesta mesma CPU faz 400 MB/s (SHA-256) e 590 MB/s
   (SHA-512), o que levaria o total a ≈ 355 ms, ainda acima de 300 ms. O que falta é decisão do dono (ver cartão):
   aceitar o número, ou baixar o custo (SHA-NI/hash em duas threads, ou não recalcular o `manifest_id` a cada início).

## Conferência independente do que foi gerado
`python -I sw-pack/testdata/verify_manifest_independent.py m.bin` (precisa de `flatbuffers` e `cryptography`): a assinatura
confere no OpenSSL e o payload é lido com o runtime oficial do FlatBuffers 25.12.19 (sem código gerado), com as
validações principais do §8 refeitas em Python. Rodado com um manifesto de 3 000 blocos: passou.
