# Formato de conteúdo: blocos, peças, manifesto e versões

Especificação do que o `sw-pack` produz e o `sw-core` consome. **Status: v0 (F0-08)** — escrita para que quem implementa o `sw-pack` e o leitor do `sw-core` consiga trabalhar só com este documento. Não vira "congelada" até o ADR de hash e serialização (F0-05) ser aceito; ver [§13](#13-decisões-provisórias-que-dependem-de-adr).

Palavras **DEVE / NÃO DEVE / PODE** têm o sentido de RFC 2119. Todo inteiro é **little-endian**. `KiB = 1024 bytes`.

Referências: [ADR-0002](adr/0002-chunk-em-dois-niveis.md) (dois níveis), [ADR-0007](adr/0007-enderecamento-por-conteudo.md) (endereçamento por conteúdo), [glossário](glossario.md). Vetores de teste: [§12](#12-vetores-de-teste) e [exemplos/piece_ref.py](exemplos/piece_ref.py).

---

## 1. Visão geral e unidades

| Unidade | Tamanho | Função |
| --- | --- | --- |
| **Bloco** | `raw_size` de 1 byte a 256 KiB (alvo médio 128 KiB) | Unidade de compressão e de leitura. Descomprimível sozinho. |
| **Peça** | **no máximo 1 MiB (1 048 576 B)**, comprimida, alvo ≥ 90% cheia | Unidade de troca P2P, de download da CDN e **de slot do cache**. Agrupa blocos inteiros. |
| **Folha Merkle** | 16 KiB | Unidade de verificação dentro da peça. |
| **Grupo** | variável | Conjunto de peças baixado junto (`core`, `regiao_norte`, `idioma_pt`...). |
| **Pacote** | variável | Um grupo materializado como **um torrent v2** (ver [§10](#10-mapeamento-para-torrent-v2-informativo)). 1 grupo = 1 pacote. |

Relações obrigatórias:

- Um bloco **pertence a exatamente uma peça** e nunca atravessa o limite entre duas peças. (Ler um bloco exige no máximo 1 peça.)
- Uma peça pertence a **exatamente um grupo**.
- Um arquivo é uma sequência ordenada de blocos; arquivo vazio = 0 blocos.
- O hash de um bloco é calculado sobre os **bytes originais (descomprimidos)**; o hash de uma peça (raiz Merkle) é calculado sobre os **bytes da peça como armazenada** (comprimidos e, no futuro, cifrados). Assim o hash do bloco independe do codec, e o peer valida a peça sem descomprimir.

```
arquivo ──► [bloco][bloco][bloco] ... (ordem do arquivo; hash sobre os bytes crus)
                │
        sw-pack reordena por localidade e empacota
                ▼
peça 0: [frame bloco a][frame bloco b][frame bloco c]   ≤ 1 MiB, sem padding
peça 1: [frame bloco d] ...
        └─ frame = saída do codec, SEM cabeçalho próprio (tamanhos vêm do manifesto)
```

## 2. Primitivas criptográficas (v0)

| Uso | Algoritmo | Observação |
| --- | --- | --- |
| Hash de bloco, folha, nó Merkle, `manifest_id` | **SHA-256** | Uma só primitiva em tudo. Também é o que o BitTorrent v2 usa. |
| Assinatura do manifesto | **Ed25519** (puro, RFC 8032) | Chave pública da publisher embutida no jogo/launcher. |
| Checksum rápido na leitura do cache | XXH3-64 | **Não faz parte deste formato**; é detalhe do cache ([04](04-cache-local.md)). |

O envelope declara o `hash_alg` (1 = SHA-256). Valores desconhecidos **DEVEM** fazer o leitor recusar o manifesto. BLAKE3 fica reservado (2) caso o ADR de hash mude a decisão; ver [§13](#13-decisões-provisórias-que-dependem-de-adr).

## 3. Árvore de Merkle da peça

Todas as peças usam uma árvore **binária completa de 64 folhas** (1 MiB / 16 KiB), independentemente do tamanho real da peça. Isso é compatível com a camada de peças do BEP 52.

```
folha[i]  = SHA-256( bytes_da_peça[i*16384 .. min((i+1)*16384, size)] )   // última folha pode ser curta; NÃO preencher com zeros antes do hash
folha[i]  = 32 bytes 0x00                                                 // para i ≥ ceil(size/16384): folha ausente (NÃO é o hash de vazio)
pai       = SHA-256( filho_esquerdo ‖ filho_direito )
piece_hash = raiz da árvore de 64 folhas
```

Regras:
- `size` da peça é 1..1 048 576. Peça de 0 bytes **NÃO DEVE** existir.
- O cliente verifica uma **folha** isolada com o caminho de 6 irmãos (irmãos só dependem de `piece_hash`; o `sw-core` pode pedir ao peer os hashes irmãos, ou recalculá-los se tiver a peça inteira).
- Peça inteira recebida: recalcula a raiz e compara com `pieces[i].piece_hash`. Divergência = descarta a peça e penaliza o peer.

## 4. Frame de bloco (dentro da peça)

O frame é só a saída do codec; o manifesto diz onde começa e quanto mede.

| `codec` | Valor | Formato do frame | Descompressão |
| --- | --- | --- | --- |
| NONE | 0 | bytes crus; `packed_size == raw_size` | cópia |
| ZSTD | 1 | **frame Zstandard padrão** (RFC 8878) com `Frame_Content_Size` presente, sem dicionário (`Dictionary_ID = 0`) | `ZSTD_decompress`; saída DEVE ter exatamente `raw_size` bytes |
| LZ4 | 2 | **LZ4 block format** (não o frame format) | `LZ4_decompress_safe` com capacidade `raw_size`; saída DEVE ter exatamente `raw_size` bytes |

Regras:
- `raw_size` do bloco: 1..262 144 (256 KiB). `packed_size`: 1..1 048 576.
- Se a compressão não reduzir o tamanho, o `sw-pack` **DEVE** usar `codec = NONE`.
- Depois de descomprimir, o leitor **DEVE** conferir `SHA-256(saída) == block_hash` antes de entregar o dado ao jogo na primeira vez que o bloco entra no cache de RAM (o cache depois usa XXH3 por leitura, [04](04-cache-local.md)).
- Dicionários Zstd por tipo de asset: **fora do v0** (`dict_id` reservado no schema, sempre 0).

## 5. Conteúdo de uma peça e posição dos blocos

Uma peça é a concatenação dos frames dos seus blocos, **sem cabeçalho, sem rodapé, sem padding**. O manifesto lista os blocos **na ordem física** (a mesma ordem das peças). Por isso:

```
peça p cobre blocks[ p.first_block .. p.first_block + p.block_count )
offset_in_piece(b) = soma de packed_size dos blocos anteriores na mesma peça
p.size = soma de packed_size de todos os blocos da peça      // o leitor DEVE validar
```

O leitor calcula `offset_in_piece` uma vez no carregamento (RAM: 4 B por bloco) em vez de gravá-lo no manifesto.

**Empacotamento (algoritmo do `sw-pack`):** percorra os blocos na ordem de localidade; feche a peça atual quando o próximo bloco faria `size > 1 MiB`; meta de enchimento ≥ 90% exceto a última peça de cada grupo. A ordem de localidade (mesma região/cena juntas) vem dos metadados da engine e depois de traces ([06](06-integracao-engine-sdk.md#prefetch)). A ordem não afeta a correção, só o desempenho.

**Deduplicação:** blocos com o mesmo `block_hash` DEVEM ser armazenados uma vez por build, com as regras:
1. Dedup dentro do mesmo grupo: permitido.
2. Dedup entre grupos: permitido **somente quando o bloco único está no grupo `core`** (que sempre está local). Qualquer outro caso: duplicar o bloco.
3. Assim, um arquivo do grupo G só depende de peças dos grupos `core` e G (fechamento simples de dependências).

## 6. Arquivos, caminhos e grupos

Caminho (`path`):
- UTF-8, normalizado em **NFC**, separador `/`, **relativo à raiz do jogo** (`res://a/b.ctex` ⇒ `a/b.ctex`).
- Sem `/` inicial ou final, sem segmentos vazios, `.` ou `..`, sem bytes `\0` ou `\`. Máximo 1024 bytes.
- Comparação **sensível a maiúsculas**, mas o `sw-pack` **DEVE** recusar um build com dois caminhos que colidam ignorando maiúsculas (quebraria no Windows).
- A lista `files` é ordenada por **ordem bytewise do caminho UTF-8** (permite busca binária).

Arquivo:
- `size` = soma dos `raw_size` dos seus blocos (o leitor DEVE validar).
- `class`: `CORE`(0) | `GROUP`(1) | `OPTIONAL`(2). `CORE` ⇒ `group = 0` (o grupo `core`). `GROUP`/`OPTIONAL` ⇒ `group ≥ 1`.
- Todos os blocos de um arquivo estão em peças do grupo do arquivo **ou** do grupo `core` (regra de dedup acima).
- Política de falta ([06](06-integracao-engine-sdk.md#política-de-falta)) por tipo de asset: **mips baixos, LODs baixos, colisão e lógica devem ser classificados `CORE` pelo `sw-pack`**; o manifesto só registra o resultado.

Grupo: `id` (índice; **0 = `core`**, sempre presente), `name` (`[a-z0-9_]{1,48}`, único), `priority` (u8, 0 = mais alta; usada pelo agendador), `piece_first`/`piece_count` (as peças de um grupo são **contíguas** na tabela de peças).

## 7. Manifesto

### 7.1 Envelope (cabeçalho fixo + payload)

O manifesto é um arquivo `manifest.bin`: um **envelope fixo** seguido do **payload** serializado. A assinatura cobre os bytes exatos, então **não existe etapa de canonicalização**.

```text
offset  tam  campo
0       8    magic            = "SWMANIF\0"  (53 57 4D 41 4E 49 46 00)
8       2    format_version   = 0   (versão do ENVELOPE, conferida por igualdade; ver abaixo)
10      1    hash_alg         = 1   (SHA-256)
11      1    sig_alg          = 1   (Ed25519)
12      4    reserved         = 0   (leitor v0 DEVE recusar se ≠ 0)
16      8    payload_len      (bytes)
24      8    key_id           = primeiros 8 bytes de SHA-256(chave_pública_Ed25519)
32      8    reserved2        = 0
40      64   signature        = Ed25519(chave_privada, MSG)
104     ...  payload (payload_len bytes)

MSG = "SWARMSTREAM-MANIFEST-V0\0" (24 bytes ASCII) ‖ envelope[0..40) ‖ payload
manifest_id = SHA-256( envelope inteiro [0..104) ‖ payload )     // identifica a versão; usado por serviços e no cache
```

**Versões, em dois níveis** ([ADR-0012](adr/0012-serializacao-do-manifesto.md), item 5):
- `format_version` versiona **só o envelope** (a tabela acima, o `MSG` e o `manifest_id`). O leitor o confere por **igualdade**: v0 aceita só `0`. Ele só muda se o layout do envelope mudar, por ADR. Mudança no payload **não** mexe nele.
- A compatibilidade do **payload** é dada pelo campo `min_reader` do payload (§7.2): a menor **revisão de payload** que um leitor precisa suportar para entender aquele conteúdo. Um leitor v0 suporta a revisão `0`.

Ordem obrigatória no leitor (o código de erro entre parênteses é o de [`swarmstream.h`](../sw-core/include/swarmstream.h)):
1. Conferir o cabeçalho **nesta ordem**: (a) `magic` (e tamanho mínimo de 104 bytes); errado ⇒ `SW_E_MANIFEST_INVALID`. (b) `format_version`; diferente de `0` ⇒ `SW_E_MANIFEST_UNSUPPORTED` (o envelope é de um formato que este leitor não conhece), **sem olhar o resto**, porque com versão desconhecida o layout dos campos seguintes nem é confiável. (c) `hash_alg`, `sig_alg`, reservados e `payload_len` (arquivo total = 104 + `payload_len`; `payload_len` ≤ 256 MiB); qualquer falha ⇒ `SW_E_MANIFEST_INVALID`.
   - Este `UNSUPPORTED` vem **antes** da assinatura, então **não é autenticado**: qualquer um pode forjá-lo trocando dois bytes. O efeito é só uma mensagem "atualize o cliente" num arquivo que seria recusado de qualquer forma; o cliente não deve tomar nenhuma ação automática (como baixar uma atualização) só por causa dele. O `UNSUPPORTED` do passo 4, ao contrário, é autenticado.
2. Localizar a chave pública pelo `key_id` na lista embutida; desconhecida ⇒ recusar (`SW_E_SIGNATURE_INVALID`).
3. **Verificar a assinatura antes de ler o payload.** Falha ⇒ recusar (`SW_E_SIGNATURE_INVALID`). Nada do payload é usado antes.
4. **Sondar o payload**, sem o verificador completo: conferir o `file_identifier` (`"SWM0"`) e ler só o campo `min_reader` da tabela raiz, conferindo cada limite (algoritmo abaixo). Identificador errado ou limite violado ⇒ recusar (`SW_E_MANIFEST_INVALID`). `min_reader` maior que a revisão de payload suportada ⇒ recusar (`SW_E_MANIFEST_UNSUPPORTED`, "atualize o cliente").
5. Rodar o verificador do serializador (FlatBuffers `Verifier`) e as validações da [§8](#8-validações-do-leitor) (`SW_E_MANIFEST_INVALID`; anti-rollback ⇒ `SW_E_MANIFEST_ROLLBACK`).

O passo 4 vem **antes** do verificador de propósito: um payload de revisão nova pode ter mudado de forma incompatível (por exemplo, um `struct` alterado), e o verificador com o esquema antigo o recusaria como inválido. Sondar antes dá o erro certo (`UNSUPPORTED`, não `INVALID`). A sondagem só confia em bytes já autenticados pela assinatura, mas confere cada limite mesmo assim, porque a chave da publisher pode ter sido usada por um build com defeito.

Sondagem do `min_reader` (todos os inteiros little-endian; `P` = payload, `n = len(P)`):
```text
n ≥ 8 e P[4..8) == "SWM0"                          senão INVALID
r = u32(P, 0);   r ≥ 8, r % 4 == 0 e r + 4 ≤ n     senão INVALID   // tabela raiz, depois do uoffset e do identificador
v = r − i32(P, r);  v ≥ 0, v % 2 == 0, v + 4 ≤ n   senão INVALID   // vtable
vs = u16(P, v); ts = u16(P, v+2)
vs ≥ 4, vs % 2 == 0, v + vs ≤ n, ts ≥ 4, r + ts ≤ n  senão INVALID
slot = 4 + 2·4                                      // min_reader é o campo de id 4
fo = (slot + 2 ≤ vs) ? u16(P, v + slot) : 0
min_reader = (fo == 0) ? 0 : (fo ≥ 4 e fo + 2 ≤ ts ? u16(P, r + fo) : INVALID)   // fo ≥ 4: o campo não sobrepõe o soffset
```
Os dois limites `r ≥ 8` e `fo ≥ 4` importam no caminho `UNSUPPORTED`: nele o `Verifier` não roda depois, e sem eles bytes assinados malformados poderiam dar um "atualize o cliente" falso. Todo leitor do manifesto (o `sw-core` e o serviço de manifestos em Go, [ADR-0010](adr/0010-linguagem-dos-servicos-de-backend.md)) usa os casos `payload_probe` de [`sw-pack/testdata`](../sw-pack/testdata/README.md) como teste de conformidade desta sondagem.

### 7.2 Payload (schema FlatBuffers v0)

Serialização **provisória**: FlatBuffers (leitura sem cópia; manifesto grande fica mapeado em memória). Ver [§13](#13-decisões-provisórias-que-dependem-de-adr). Arquivo `manifest.fbs`:

```fbs
namespace swarm.v0;

enum Codec : ubyte { None = 0, Zstd = 1, Lz4 = 2 }
enum FileClass : ubyte { Core = 0, Group = 1, Optional = 2 }

struct Hash32 { b: [ubyte:32]; }

// Tabelas "colunares": um vetor por campo, mesmo índice. Evita overhead por objeto
// e mantém ~41 bytes por bloco. Todos os vetores de uma tabela têm o mesmo comprimento.
table Blocks {                 // ORDEM FÍSICA (a das peças)
  hash:        [Hash32];       // SHA-256 dos bytes crus
  raw_size:    [uint32];       // 1..262144
  packed_size: [uint32];       // 1..1048576 (== raw_size se codec None)
  codec:       [Codec];
}

table Pieces {
  piece_hash:  [Hash32];       // raiz Merkle (§3)
  size:        [uint32];       // 1..1048576 = soma de packed_size
  first_block: [uint32];       // índice em Blocks; peças cobrem Blocks sem lacuna, em ordem
  block_count: [uint32];
  group:       [uint16];
}

table Files {                  // ordenados por path (bytewise UTF-8)
  path:        [string];
  size:        [uint64];
  class:       [FileClass];
  group:       [uint16];
  first_ref:   [uint32];       // início em block_refs
  ref_count:   [uint32];
}

table Group {
  name:        string;         // [a-z0-9_]{1,48}; índice 0 = "core"
  priority:    ubyte;
  piece_first: uint32;
  piece_count: uint32;
  deps:        [uint16];       // além de "core" (v0: sempre vazio; reservado)
}

table Sources {
  cdn_base_urls:    [string];  // https://, sem barra final; URL da peça = base + "/p/" + hex(piece_hash)
  trackers:         [string];
  torrent_infohashes: [Hash32]; // infohash v2 (32 bytes) do pacote de cada grupo; índice = group id
}

table Chunker {                // INFORMATIVO: o leitor não depende disto
  min_size: uint32; avg_size: uint32; max_size: uint32;
  algorithm: string;           // v0: "swarmstream-fastcdc-v0" (§11.1)
}

table ManifestPayload {
  game_id:     string;         // [a-z0-9.-]{1,64}
  build_id:    string;         // ex.: "1.3.0+4521"; texto livre para humanos
  build_seq:   uint64;         // inteiro estritamente crescente por game_id (anti-rollback)
  created_at:  int64;          // segundos Unix UTC
  min_reader:  uint16;         // menor revisão de payload que o leitor precisa suportar (v0: 0). Campo de id 4, uint16: id e tipo FIXOS PARA SEMPRE (sondagem do §7.1, passo 4)
  chunker:     Chunker;
  blocks:      Blocks;
  pieces:      Pieces;
  block_refs:  [uint32];       // índices em Blocks, na ORDEM DO ARQUIVO, concatenados por arquivo
  files:       Files;
  groups:      [Group];
  sources:     Sources;
  cipher_id:   ubyte = 0;      // 0 = sem cifra (v0). Reservado: ver §9
}

root_type ManifestPayload;
file_identifier "SWM0";
```

Evolução do payload (o envelope e o `format_version` não mudam por causa dela):
- Campo novo **no fim** de uma tabela: compatível; **não** muda `min_reader`. Leitores antigos ignoram o campo.
- Mudança de `struct`, de tipo, remoção ou reordenação de campo: incompatível; o escritor **sobe** `min_reader` para a nova revisão, e a mudança exige ADR. Leitores antigos recusam no passo 4 com `SW_E_MANIFEST_UNSUPPORTED`, em vez de lerem errado.
- Ficam fixos para sempre, porque a sondagem depende deles: `root_type ManifestPayload`, `file_identifier "SWM0"` e o campo `min_reader` (id 4, `uint16`).

### 7.3 Orçamento de tamanho (meta: < 50 MB para 150 GB de jogo)

Por bloco: hash 32 + raw 4 + packed 4 + codec 1 + ref de arquivo 4 = **45 B**; mais ~4 B de `offset_in_piece` só em RAM.

| Jogo (bytes crus) | Bloco médio | Blocos | Manifesto aprox. |
| --- | --- | --- | --- |
| 3 GB (mapa F0-09) | 128 KiB | ~24 mil | ~1,1 MB |
| 150 GB | 128 KiB | ~1,2 M | **~54 MB (estoura a meta)** |
| 150 GB | 192 KiB | ~0,8 M | ~36 MB |
| 150 GB | 256 KiB | ~0,6 M | ~27 MB |

A meta de 50 MB **não** é cumprida em 150 GB com o bloco médio de 128 KiB. Medir no mapa da F0-09 e, se confirmado, usar uma destas saídas na F1 (decidir por ADR): (a) média de 192 KiB para grupos de assets grandes; (b) cortar `block_hash` para 16 bytes no manifesto (colisão em 2^64, aceitável porque a assinatura já impede adulteração e a integridade de transporte vem da Merkle da peça); (c) dividir o manifesto em raiz + tabela de blocos por grupo, carregada sob demanda. O formato acima **não impede** (c): é só mover `Blocks` para segmentos por grupo num payload de revisão 1 (`min_reader = 1`); o envelope não muda.

## 8. Validações do leitor

Estas validações são o passo 5 do [§7.1](#71-envelope-cabeçalho-fixo--payload): rodam depois da assinatura, da sondagem do `min_reader` (passo 4; `SW_E_MANIFEST_UNSUPPORTED` se for maior que a revisão suportada) e do verificador do serializador. O `sw-core` DEVE recusar o manifesto se qualquer item falhar (nada é "corrigido" silenciosamente); o erro é `SW_E_MANIFEST_INVALID`, exceto o item 10 (`SW_E_MANIFEST_ROLLBACK`):

1. `groups[0].name == "core"`; nomes de grupo únicos e válidos; `group` de arquivo e peça `< len(groups)`.
2. Vetores de cada tabela colunar têm o mesmo comprimento.
3. Peças cobrem `blocks` em ordem, sem lacuna nem sobreposição: `pieces[0].first_block == 0`, `pieces[i+1].first_block == pieces[i].first_block + pieces[i].block_count`, soma final `== len(blocks)`.
4. Para cada peça: `1 ≤ size ≤ 1 048 576` e `size == Σ packed_size` dos seus blocos.
5. Para cada bloco: `1 ≤ raw_size ≤ 262 144`; `1 ≤ packed_size ≤ 1 048 576`; `codec == None ⇒ packed_size == raw_size`.
6. Peças de cada grupo são contíguas e batem com `piece_first`/`piece_count`.
7. Arquivos: caminhos válidos, estritamente crescentes (bytewise), sem colisão ignorando maiúsculas; `first_ref`/`ref_count` formam partição de `block_refs` em ordem; `size == Σ raw_size` dos blocos referenciados; todo índice em `block_refs < len(blocks)`.
8. Fechamento de dependências: o grupo de cada bloco referenciado por um arquivo é `core` ou o grupo do arquivo. Arquivo `CORE` só referencia blocos do grupo 0.
9. Limites de sanidade (anti-DoS): ≤ 16 M blocos, ≤ 4 M arquivos, ≤ 4096 grupos, strings ≤ 1024 bytes.
10. Anti-rollback: `build_seq` ≥ o `build_seq` do manifesto instalado para o mesmo `game_id` (o launcher pode permitir downgrade explícito do usuário; o `sw-core` não decide isso).

O `min_reader` não é mais um item desta lista: ele é conferido no passo 4 do §7.1, antes do verificador, porque depois dele já seria tarde para dar o erro certo.

## 9. Sigilo (reservado, fora do v0)

`cipher_id = 0` no v0. Reserva de projeto, já válida para quem implementa a Merkle: com cifra, os **bytes da peça** passam a ser o texto cifrado, a raiz Merkle é calculada sobre ele (peers validam sem a chave), e o `block_hash` continua sobre o texto claro. A chave por pacote chega pelo serviço de licença ([07](07-seguranca-e-privacidade.md)). Nonce/AEAD (AES-256-GCM ou ChaCha20-Poly1305) serão definidos em ADR próprio **antes** de ligar o recurso; um leitor v0 DEVE recusar `cipher_id ≠ 0`.

## 10. Mapeamento para torrent v2 (informativo)

Para o P2P ([ADR-0001](adr/0001-poc-godot-libtorrent.md)), cada grupo vira um torrent v2 de **um arquivo virtual** formado pelas suas peças em ordem, cada uma ocupando uma janela de 1 MiB (a parte após `size` é **padding de zeros que nunca é transmitido nem armazenado**, como "pad file" do BEP 47/52). Assim a camada de peças do torrent (raiz Merkle de 64 folhas por janela) coincide com `piece_hash`. Web seed (BEP 19) aponta para a CDN. **Risco:** suporte do libtorrent a esse arranjo e a peças menores que a janela precisa ser provado no spike de F2 ([R7 em 12](12-riscos.md)); se não servir, o plano B é uma camada de mensagens própria sobre o mesmo `piece_hash`, sem mudar o formato do manifesto.

## 11. Pipeline do `sw-pack` e fluxo de patch

```
arquivos do jogo
 → classificação (core / grupo / opcional) a partir de regras do estúdio + metadados da engine
 → chunking FastCDC por arquivo → blocos            (§11.1)
 → hash SHA-256 do bloco cru; dedup (§5)
 → compressão por bloco (codec por tipo de asset, §4)
 → empacotamento em peças por localidade (§5)
 → Merkle por peça (§3) → payload FlatBuffers (§7.2) → envelope + assinatura (§7.1)
 → pacote/torrent v2 por grupo (§10) → upload à CDN: objetos "/p/<hex(piece_hash)>" e o manifesto
```

Patch: o estúdio publica um manifesto novo (`build_seq` maior). O cliente compara `block_hash` do manifesto instalado com o novo e baixa só as **peças** que contêm blocos novos. Como a ordem de localidade e a dedup mudam o conteúdo das peças, uma peça nova pode conter blocos já locais; o cliente pode **reutilizar blocos locais** só por reempacotamento local (F4) — no v0, a unidade de download de patch é a peça. Estratégia de "peças estáveis" fica na Fase 4 ([ADR-0007](adr/0007-enderecamento-por-conteudo.md)).

### 11.1 Chunker (determinismo)

O leitor não depende do chunker, mas **dois builds só compartilham blocos se o chunker for idêntico**. Parâmetros fixados para o v0 (qualquer mudança invalida dedup entre builds, não a leitura):

- FastCDC com normalização nível 2, **um byte por passo**, `min = 32 KiB`, `avg = 128 KiB`, `max = 256 KiB`; trecho restante com até `min` bytes vira um único bloco. Identificador no manifesto: `Chunker.algorithm = "swarmstream-fastcdc-v0"`.
- **Não é compatível bit a bit** com as implementações públicas de FastCDC (que usam outra tabela gear, máscaras espalhadas e, no FastCDC 2020, dois bytes por passo). Não use uma biblioteca externa como referência: a referência são os vetores de [`sw-pack/testdata/`](../sw-pack/testdata/README.md).
- Tabela gear de 256 entradas de 64 bits: `gear[i] = LE64(SHA-256("swarmstream-gear-v0" ‖ byte(i))[0..8])`.
- Máscaras: `mask_s` = os `log2(avg)+2` bits **mais significativos** de 64 em 1 (19 bits para 128 KiB); `mask_l` = os `log2(avg)−2` mais significativos (15 bits). `mask_s` vale para posições `i < normal`, `mask_l` a partir daí; corte quando `(fp & mask) == 0`.
- O hash começa em `fp = 0` na posição `min` (as primeiras `min` posições não entram no hash), e o bloco **inclui** o byte que disparou o corte (`retorne i+1`). Sem corte até `min(restante, max)`, o bloco termina ali.
- Algoritmo exato (pseudocódigo) e vetores: [`sw-pack/testdata/README.md`](../sw-pack/testdata/README.md). **Aprovado pelo R1 em 2026-10-18**; faz parte do formato e só muda por ADR.
- Tamanho real, medido em 64 MiB pseudoaleatórios: **média ≈ 148 KiB** (mediana 148 KiB; 1,1% dos blocos no máximo; 17% até 128 KiB). `avg` é o ponto de normalização, não a média. Inserir 1 byte no meio mudou 1 bloco. O orçamento de [§7.3](#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo) deve usar a média medida nos assets reais (F1-04), não 128 KiB.

Divergência assumida em relação ao rascunho anterior ("bloco 64–256 KB"): o limite rígido é `raw_size ≤ 256 KiB`; não há mínimo para o leitor (blocos de cauda e arquivos pequenos são menores), e o `avg` é configurável.

## 12. Vetores de teste

Gerados por [exemplos/piece_ref.py](exemplos/piece_ref.py) (`python -I piece_ref.py`). Bloco `n` bytes com seed `s`: `byte[i] = (i*7 + s) mod 256`. Peça de exemplo com 3 blocos, `codec = NONE` (frames = bytes crus), na ordem do bloco 0 ao 2:

| bloco | raw = packed | offset_in_piece | SHA-256 (`block_hash`) |
| --- | --- | --- | --- |
| 0 (seed 1) | 1000 | 0 | `095ecb62e30793ab4b954cd6a0586d0cc91f7ea5b1332694d8da780e98676d78` |
| 1 (seed 2) | 20000 | 1000 | `dc6ef9efc23943d51c3314af5ed321e6b36dac25053b6050fcf1bf1f27286db7` |
| 2 (seed 3) | 5000 | 21000 | `34398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afb` |

Peça: `size = 26000` ⇒ 2 folhas (16 384 + 9 616 bytes), 62 folhas ausentes (zeros).

| item | valor |
| --- | --- |
| folha 0 | `fd3ce7c92ea7c5a157b3849d6993b1b7900c63d48105daffdaa51e7cbbf9dc19` |
| folha 1 | `307c053c240dbdcf982082b6e67c172fb2b5eb0c1041ce4a5eb8a319fbb1e6b0` |
| **`piece_hash`** | `cc5aced08754bb61cc287108a94d664a1d6b45cd7e0a1f607c8ea359fd76e1c2` |
| `piece_hash` de peça de 1 byte `0x00` | `00ae8802da7a65746a6910e91fa8b0557ef1dc2e1be91e502985c019da71f051` |

Vetores de **envelope assinado, Zstd/LZ4 e chunker** estão em [`sw-pack/testdata/vectors-v0.json`](../sw-pack/testdata/README.md), gerados por `gen_vectors.py` e conferidos por implementação independente (`verify_vectors.py`; OpenSSL reproduz a assinatura byte a byte). Pendentes: payload FlatBuffers real no envelope (precisa de `flatc`/`sw-pack`) e aprovação do sênior para a definição do chunker. Critério de conformidade mínimo do `sw-pack` v0: reproduzir os valores acima e os do JSON e passar nas validações da [§8](#8-validações-do-leitor) com um manifesto gerado e depois adulterado (um byte do payload, um byte do hash, assinatura trocada) — os três casos DEVEM ser recusados.

## 13. Decisões provisórias que dependem de ADR

O backlog F0-08 depende do F0-05; como os ADRs ainda não existem, o v0 fixa defaults. Se o ADR decidir outra coisa, só o que está abaixo muda.

| Decisão | Default do v0 | Por quê | O que muda se o ADR discordar |
| --- | --- | --- | --- |
| Hash dos blocos | **SHA-256** (`hash_alg = 1`) | Uma primitiva só; aceleração em hardware (SHA-NI); é o hash do BEP 52, então o `piece_hash` e o do torrent coincidem | BLAKE3 é mais rápido no `sw-pack` e na verificação de bloco, mas **não** pode substituir SHA-256 na Merkle da peça (BEP 52); usaria `hash_alg = 2` só para `block_hash`. Mudança local ao §2/§4 |
| Serialização | **FlatBuffers** + envelope fixo | Leitura sem cópia, manifesto de dezenas de MB, vetores colunares; assinatura sobre bytes exatos dispensa canonicalização | CBOR (mais simples) obrigaria definir CBOR determinístico (RFC 8949 §4.2) ou manter o envelope e trocar só o payload. O envelope e os §3–§6 não mudam |
| Tamanho do hash no manifesto | 32 bytes | Simplicidade | Ver §7.3(b) |

## Questões em aberto
- Medir o manifesto real no mapa da F0-09 e confirmar a escolha do §7.3.
- Tamanho ótimo de bloco por tipo de asset (F1).
- Rotação de chaves da publisher (lista de `key_id` confiáveis no launcher) — fora desta spec, vai para o ADR do launcher.
- Aceitar ou não torrent v2 com janelas de 1 MiB parcialmente vazias no libtorrent (§10).
