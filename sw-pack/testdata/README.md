# sw-pack/testdata — vetores de teste do formato (spec 03, §12)

`vectors-v0.json` guarda os valores que qualquer implementação (C++ no `sw-pack`/`sw-core`, Go nos serviços) precisa reproduzir. Valores em hexadecimal; `stream(label, n)` = `SHA-256("swarmstream-vec-v0|" ‖ label ‖ u32le(contador))` concatenado e cortado em `n` bytes.

**Como ler os campos** (esclarecimentos do F1-51):
- `stream(label, n)`: o contador começa em **0** e cresce de 1 em 1; cada bloco de 32 bytes é `SHA-256("swarmstream-vec-v0|" ‖ label ‖ u32le(contador))`, e o resultado é cortado em `n` bytes.
- `chunker.gear_first4_hex`: cada entrada é o **valor** de 64 bits em **hexadecimal** (16 dígitos, big-endian como número), **não** os bytes little-endian. Ex.: `gear[0]` aparece como o número `gear[0]`, não como a sequência de bytes da tabela.
- `chunker.gear_table_sha256`: SHA-256 da serialização das **256 entradas**, cada uma como **8 bytes little-endian** (2048 bytes no total, na ordem de `i` de 0 a 255).
- `chunker.cases[*].chunks`: `len` é o tamanho do bloco em bytes; `sha256` é o SHA-256 dos bytes do bloco. Os blocos cobrem a entrada sem lacuna e sem sobreposição.

| Seção | O que cobre |
| --- | --- |
| `chunker` | tabela gear e algoritmo `swarmstream-fastcdc-v0` (§11.1) com os parâmetros do §11.1 (32 KiB / 128 KiB / 256 KiB); vazio, 1 byte, nos limites min/max, 1 MiB, 4 MiB e zeros; dois casos com parâmetros pequenos: `small_params_64k` (256/1024/4096) e **`edges_small_params`** (64/256/1024 sobre 16 KiB de `stream("chunker/edges")`, F1-51, adicionado depois sem mudar nenhum valor anterior; pega a máscara pequena em `i == avg` e o hash que começa antes de `min`) |
| `codecs` | frames Zstd e LZ4 (block format) de 6 entradas, com a regra "não reduziu ⇒ `NONE`"; Zstd com `Frame_Content_Size` presente e `Dictionary_ID = 0` |
| `envelope` | envelope do §7.1 assinado com **chave de teste** (seed `00..1f`), `MSG`, assinatura Ed25519, `manifest_id` e 7 casos que o leitor DEVE recusar (3 exigidos pela spec + cabeçalho/chave); mais 2 casos de `format_version` desconhecido, com assinatura válida (recusado por igualdade, `unsupported`; um deles também com `reserved ≠ 0`, para fixar que a versão é conferida antes) |
| `envelope.payload_probe` | sondagem do `min_reader` (§7.1, passo 4): 7 manifestos assinados com payload FlatBuffers mínimo (`min_reader` 0, 1, ausente, identificador errado, raiz fora do buffer, raiz dentro do cabeçalho, campo por cima do `soffset`), com o resultado esperado (`ok`, `unsupported`, `invalid`) para um leitor da revisão 0 |

## Comandos
```bash
python verify_vectors.py            # confere o JSON com implementação independente (~2 min; Python 3.14+ ou `pip install zstandard`)
python verify_vectors.py --skip-zstd  # sem decodificador Zstd (Zstd fica só com o cabeçalho conferido)
python test_properties.py -v        # propriedades e comparação entre as duas implementações (~30 s)
python gen_vectors.py               # regenera o JSON (Python 3.14+ ou `zstandard`)
```
Precisa de `openssl` 3.x no PATH (no Windows, o Git for Windows traz em `usr/bin`).

## O que "independente" significa aqui (e o que não)
- **Ed25519:** a assinatura é determinística; o gerador usa uma implementação em Python puro do RFC 8032 (conferida com o teste 1 do RFC) e o verificador exige que o **OpenSSL produza os mesmos 64 bytes** e aceite/recuse as assinaturas.
- **LZ4:** o gerador tem um compressor próprio; o verificador tem um decodificador próprio. Conformidade = o frame decodifica para os bytes originais (outro compressor pode gerar outros bytes válidos).
- **Zstd:** o cabeçalho do frame é conferido por um parser escrito do RFC 8878; a **descompressão usa libzstd** (a mesma do gerador). Uma segunda implementação de Zstd só entra quando o `sw-pack` em C++ existir.
- **Chunker:** duas implementações escritas separadamente (`gen_vectors.py` e `verify_vectors.py`), comparadas em 150 entradas aleatórias (`test_properties.py`). Uma terceira, de **outro autor** (R1), escrita só a partir do pseudocódigo abaixo e da spec, sem ler as outras duas: `verify_chunker_r1.py`. Reproduz os 10 casos do chunker e a tabela gear.
- **Limites:** `gen_vectors.py` e `verify_vectors.py` foram escritas pela mesma pessoa a partir da spec. Para o chunker, `verify_chunker_r1.py` dá independência de autoria. Envelope, LZ4 e cabeçalho Zstd ainda têm só independência de código, até existirem o `sw-pack` e o `sw-core` em C++ (ver "Aprovação").
- **Payload do envelope é sintético** (bytes opacos, não FlatBuffers): falta `flatc` / `sw-pack`. O envelope (§7.1) está completo; o payload real será um vetor novo.
- **Sondagem do `min_reader`:** o gerador monta os payloads mínimos à mão e o verificador tem a sua própria leitura com limites. Os dois foram conferidos contra o runtime oficial do FlatBuffers para Python (25.12.19): ele lê os payloads mínimos com os mesmos valores, e a sondagem lê buffers feitos pelo `Builder` oficial. O `Verifier` do FlatBuffers não foi rodado (não há `flatc` aqui); os payloads mínimos só cobrem o passo 4, não o 5. **Teste de conformidade:** todo leitor do manifesto, inclusive o serviço em Go ([ADR-0010](../../docs/adr/0010-linguagem-dos-servicos-de-backend.md)), precisa reproduzir os casos de `payload_probe`.

## Definição do chunker usada nos vetores (§11.1, aprovada)
```
mask_s = (bits_altos = log2(avg)+2) ; mask_l = (bits_altos = log2(avg)-2)      # para avg=128 KiB: 19 e 15 bits
cut(início): n = restante; se n <= min -> n
             limite = min(n, max); normal = min(avg, limite)
             fp = 0; para i de min até limite-1:   # o hash começa em min (salto de posições)
                 fp = ((fp << 1) + gear[byte[i]]) mod 2^64
                 se (fp & (mask_s se i < normal senão mask_l)) == 0: retorne i+1
             retorne limite
```
`mask` com `k` bits = os `k` bits **mais significativos** de 64 em 1 (`((2^64 − 1) << (64 − k)) mod 2^64`). O bloco inclui o byte `i` que disparou o corte. Identificador: `swarmstream-fastcdc-v0`. Não é compatível bit a bit com bibliotecas públicas de FastCDC.

## Aprovação (R1, 2026-10-18)
**Aprovada com ajustes.** Esta definição fecha o §11.1 e passa a fazer parte do formato; só muda por ADR.
- Conferida por implementação de outro autor (`verify_chunker_r1.py`): 10 de 10 casos e a tabela gear batem.
- Medido em 64 MiB pseudoaleatórios: média ≈ 148 KiB, 1,1% dos blocos no máximo; inserir 1 byte muda 1 bloco (critério da F1-01: ≤ 3).
- Ajustes feitos: identificador `fastcdc2020-sha256gear-v0` → `swarmstream-fastcdc-v0` (não é o FastCDC 2020 de referência, que avança dois bytes por passo com máscaras espalhadas); máscara e corte escritos sem ambiguidade aqui e no §11.1; média real registrada no §11.1.
- **Condição para a F1-01 (chunker em C++):** escrito só a partir do §11.1 e deste README, sem ler o código Python, por quem não escreveu `gen_vectors.py`. Ele precisa reproduzir os vetores antes de entrar no `main`. Para envelope e codecs, a mesma regra vale para o `sw-pack`/`sw-core` em C++.
