# sw-pack/testdata — vetores de teste do formato (spec 03, §12)

`vectors-v0.json` guarda os valores que qualquer implementação (C++ no `sw-pack`/`sw-core`, Go nos serviços) precisa reproduzir. Valores em hexadecimal; `stream(label, n)` = `SHA-256("swarmstream-vec-v0|" ‖ label ‖ u32le(contador))` concatenado e cortado em `n` bytes.

| Seção | O que cobre |
| --- | --- |
| `chunker` | tabela gear, FastCDC 2020 com os parâmetros do §11.1 (32 KiB / 128 KiB / 256 KiB) e um caso com parâmetros pequenos; vazio, 1 byte, nos limites min/max, 1 MiB, 4 MiB e zeros |
| `codecs` | frames Zstd e LZ4 (block format) de 6 entradas, com a regra "não reduziu ⇒ `NONE`"; Zstd com `Frame_Content_Size` presente e `Dictionary_ID = 0` |
| `envelope` | envelope do §7.1 assinado com **chave de teste** (seed `00..1f`), `MSG`, assinatura Ed25519, `manifest_id` e 7 casos que o leitor DEVE recusar (3 exigidos pela spec + cabeçalho/chave) |

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
- **Chunker:** duas implementações escritas separadamente (`gen_vectors.py` e `verify_vectors.py`), comparadas em 150 entradas aleatórias (`test_properties.py`).
- **Limites:** as duas implementações em Python foram escritas pela mesma pessoa a partir da spec. Independência de código, não de autoria; a revisão do sênior e a futura implementação em C++ são a terceira conferência.
- **Payload do envelope é sintético** (bytes opacos, não FlatBuffers): falta `flatc` / `sw-pack`. O envelope (§7.1) está completo; o payload real será um vetor novo.

## Definição do chunker usada nos vetores (proposta para o §11.1)
```
mask_s = (bits_altos = log2(avg)+2) ; mask_l = (bits_altos = log2(avg)-2)      # para avg=128 KiB: 19 e 15 bits
cut(início): n = restante; se n <= min -> n
             limite = min(n, max); normal = min(avg, limite)
             fp = 0; para i de min até limite-1:   # o hash começa em min (salto de posições)
                 fp = ((fp << 1) + gear[byte[i]]) mod 2^64
                 se (fp & (mask_s se i < normal senão mask_l)) == 0: retorne i+1
             retorne limite
```
Esta definição **fecha o que o §11.1 deixava em aberto** e precisa de aprovação do sênior (spec marcada `critico`) antes de virar parte do formato.
