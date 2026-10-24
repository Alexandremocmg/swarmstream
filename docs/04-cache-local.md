# Cache local e sistema de arquivos virtual

Responsável pela camada 2 da arquitetura. Objetivo: o engine **nunca espera por disco nem por limpeza de cache**, e o cache **nunca entrega dado errado**.

## Modos de instalação

| Modo | Conteúdo local | Cache | Papel no swarm |
| --- | --- | --- | --- |
| **Compacto (padrão)** | Grupo `core` (~10–15% do jogo) | Teto configurável (ex.: 20 GB), com remoção | Serve o que tiver em cache |
| **Completo / offline** | Tudo | Sem remoção | **Super-seeder** |
| **Personalizado** | `core` + grupos escolhidos (ex.: campanha, idioma) | Teto para o resto | Serve o que tiver |

## Estrutura em disco

```
<pasta_do_jogo>/swarm/
  manifest.bin        manifesto atual assinado
  store-000.bin ...   arquivos pré-alocados, divididos em slots de tamanho de peça (1 MiB)
  index.bin           índice binário mapeado: hash da peça → (arquivo, slot), estado, contadores
  journal.bin         registro só de append (WAL) das transações de metadados
```

### 1. Arquivos de dados (`store-XXX.bin`)
- Divididos em **slots de tamanho fixo de 1 MiB (1 048 576 bytes)** alinhados a 4 KiB (compatível com `FILE_FLAG_NO_BUFFERING` no Windows e `O_DIRECT` no Linux).
- Capacidade padrão por arquivo de store: 2 GiB (2 048 slots). Novos arquivos (`store-001.bin`, etc.) são criados sob demanda até atingir o teto de cache configurado.
- **Pré-alocação de verdade, não esparsa** (F1-10). Windows: `FileAllocationInfo` e depois `FileEndOfFileInfo`. Linux: `fallocate(2)`. O espaço livre é conferido **antes** de reservar, e um pedido que não cabe dá `DiskFull` sem reservar nada; se a reserva falhar, o que foi reservado volta ao disco e o arquivo recém-criado é apagado.
  - **Aviso: sistema de arquivos sem `fallocate`** (NFS antigo, alguns overlay e ZFS). O código cai na `posix_fallocate`, que a glibc **emula escrevendo zeros** no arquivo inteiro: 2 GiB por arquivo, com o tempo de gravar 2 GiB. Por causa da conferência prévia, isso não enche o disco, mas deixa a criação lenta. Se aparecer em campo, a saída é zerar o arquivo em segundo plano. O ramo tem teste (`test_store_files_posix_fallocate_fallback`); a emulação em si não tem, porque o CI não tem um sistema de arquivos sem `fallocate`.
- Estrutura de cada slot:
  - Offset `0`: bytes da peça compactada (tamanho `size ≤ 1 048 576`).
  - Sem fragmentação interna entre slots; peças ocupam 1 slot inteiro.

### 2. Formato do Journal WAL (`journal.bin`)
O journal é um log de escrita prévia (Write-Ahead Log) puramente append-only. Nenhuma alteração no índice é persistida antes da entrada correspondente estar fsyncada no journal.

#### Cabeçalho do Arquivo Journal (primeiros 64 bytes)
```text
offset  tam  campo          descrição
0       8    magic          = "SWJOURN\0" (0x004E52554F4A5753)
8       2    version        = 1
10      2    flags          = 0 (reservado)
12      4    page_size      = 4096 (alinhamento de bloco de disco)
16      8    checkpoint_lsn = menor LSN ativo necessário para recovery
24      8    last_lsn       = último LSN escrito e flushado
32      24   reserved       = 0x00
56      8    header_crc     = XXH3-64 dos bytes [0..56)
```

- **O cabeçalho nunca é regravado no lugar** (F1-11): um cabeçalho rasgado por uma queda perderia o journal inteiro. Ele é escrito uma vez, na criação, e `last_lsn` guarda o LSN daquele momento; o último LSN real sai da leitura dos registros. O checkpoint (§4) cria um journal novo e o troca por rename atômico.

#### Registros de Transação (Record Entry)
Cada mutação no cache gera um registro append-only com soma de verificação. O `COMMIT_PIECE` carrega o checksum XXH3-64 de **cada bloco** da peça ([ADR-0013](adr/0013-hash-dos-blocos.md)); os outros tipos têm tamanho fixo:
```text
offset    tam  campo          descrição
0         4    record_magic   = 0x57414C52 ("WALR")
4         2    record_type    = COMMIT_PIECE (1) | FREE_SLOT (2) | CHECKPOINT (3)
6         2    flags          = flags adicionais (ex.: 0x01 = fixo/pinned)
8         8    lsn            = Log Sequence Number (estritamente monotônico crescente)
16        8    timestamp_utc  = época em microssegundos
24        32   piece_hash     = SHA-256 raiz Merkle da peça (zeros para CHECKPOINT)
56        4    store_file_id  = índice do arquivo store (ex.: 0 para store-000.bin)
60        4    slot_id        = índice do slot no arquivo store
64        4    piece_size     = tamanho gravado da peça (bytes)
68        4    block_count    = n, blocos da peça (COMMIT_PIECE: ≥ 1; FREE_SLOT e CHECKPOINT: 0)
72        8·n  block_xxh3[n]  = XXH3-64 dos bytes GRAVADOS de cada bloco, na ordem da peça
72+8·n    8    record_crc     = XXH3-64 do registro [0..72+8·n)
Total: 80 + 8·n bytes (FREE_SLOT e CHECKPOINT: 80). Sempre múltiplo de 8.
```
- **De onde vêm os checksums:** calculados pelo cliente sobre os bytes da peça **em memória, depois de a peça passar na verificação Merkle** e antes do passo 2 da escrita (abaixo), usando os limites de bloco do manifesto (`offset_in_piece`, `packed_size`, 03 §5). Nunca vêm da rede e nunca são calculados relendo o disco (isso abençoaria uma escrita corrompida).
- `n` tem de ser igual ao `block_count` da peça no manifesto em uso; a spec não limita `n` além de `n ≤ piece_size` (cada bloco tem pelo menos 1 byte), então o registro tem tamanho variável.
- **Tamanho do registro:** o limite é `n ≤ piece_size ≤ 1 048 576` (cada bloco tem pelo menos 1 byte; peça de no máximo 1 MiB, 03 §3), ou seja, até 80 + 8 · 1 048 576 ≈ **8 MiB** por registro. Na prática fica bem abaixo, mas não é pequeno em conteúdo muito comprimível ou com muitos arquivos pequenos: blocos de 256 KiB de zeros comprimem para ≈ 26 B (Zstd nível 3; 25 B nos níveis 15–19, medido com o Python 3.14 em 2026-10-19) ⇒ até ≈ 40 mil blocos por peça ⇒ registro de ≈ 322 KB; arquivos de 100 B, um bloco cada ⇒ ≈ 10 mil blocos ⇒ ≈ 84 KB. Por isso o append do journal é uma **escrita de tamanho variável**, com buffer dimensionado por `n` (reservado fora do caminho de leitura), e não uma estrutura fixa de 80 B.

### 3. Protocolo de Escrita ACID (Durabilidade e Atomicidade)
Para prevenir dados corrompidos ou leituras de zeros em crashes repentinos (falha de energia ou kernel panic), a escrita segue ordem rígida com barreiras de E/S (`fsync` / `FlushFileBuffers`):

```text
[1. Alocação]      thread dona reserva slot livre no mapa de bits em memória
[2. Escrita Dados] grava dados da peça no slot pré-alocado do store-XXX.bin
[3. Barreira 1]    fdatasync(store_fd)  --> dados estão fisicamente na mídia
[4. Escrita WAL]   append do registro COMMIT_PIECE em journal.bin
[5. Barreira 2]    fdatasync(journal_fd) --> ponto de commit da transação
[6. Publicação]    atualiza entrada no índice em memória e acorda leitores
```

- O registro do passo 4 já leva os checksums por bloco; o passo 6 copia a lista para o índice em memória.
- **Por que esta ordem (Data-First, then WAL Commit)?**
  - Se a máquina travar antes do Passo 5: o journal **não** contém o commit. Na inicialização, a transação não existe; o slot é considerado lixo e reutilizado. O engine **nunca** lerá dados parciais nem zeros.
  - Se travar após o Passo 5: o journal contém o registro válido e com checksum correto. A recuperação do Passo 3 já garante que os dados físicos foram persistidos no slot antes do journal. O registro é reexecutado com 100% de integridade.

### 4. Formato do Índice (`index.bin`) e Checkpoints
- O `index.bin` tem três partes: cabeçalho, **tabela de entradas** (uma por slot ocupado) e **região de checksums** (`u64`, os XXH3-64 por bloco de todas as peças, contíguos por peça).
- Entrada, 56 bytes, alinhada a 8:
  ```text
  offset  tam  campo
  0       32   piece_hash      raiz Merkle da peça
  32      4    slot_id
  36      4    size            bytes gravados da peça
  40      4    access_counter
  44      4    cks_first       índice, em u64, do 1º checksum da peça na região de checksums
  48      4    block_count     n; os checksums são cks[cks_first .. cks_first + n)
  52      2    store_file_id
  54      1    state
  55      1    flags
  ```
  Antes havia um `checksum_xxh3: u64` por peça; ele saiu. A entrada continua com 56 bytes, porque `cks_first` e `block_count` ocupam o lugar do checksum único e o espaço de alinhamento.
- O cabeçalho guarda o número de entradas, o tamanho da região de checksums, o `lsn` do checkpoint e um XXH3-64 de todo o arquivo. No checkpoint, a região é regravada compacta (sem buracos de peças removidas). Layout (F1-11), 64 bytes, little-endian:
  ```text
  offset  tam  campo
  0       8    magic            = "SWINDEX\0"
  8       2    version          = 1
  10      2    flags            = 0
  12      4    entry_count
  16      8    checksum_count   u64 na região de checksums
  24      8    checkpoint_lsn   o journal continua a partir daqui
  32      4    slot_size        layout do store: um índice de outro layout é descartado
  36      4    slots_per_file
  40      16   reserved         = 0
  56      8    file_xxh3        XXH3-64 do arquivo inteiro, calculado com este campo zerado
  ```
  O leitor confere magic, versão, layout, `tamanho do arquivo = 64 + 56·entry_count + 8·checksum_count` (antes de alocar) e o XXH3. Depois confere cada entrada (slot dentro do arquivo, `1 ≤ size ≤ slot_size`, `1 ≤ block_count ≤ size`, checksums dentro da região, sem slot nem peça repetidos). Qualquer defeito descarta o índice **inteiro**: um índice incoerente não é usado em parte.
- **Custo** (estimativa; os valores reais saem da F1-11):

  | Item | Antes (1 checksum por peça) | Agora (1 por bloco) |
  | --- | --- | --- |
  | RAM e disco do índice, por slot | 56 B | 56 B + 8 B × blocos da peça |
  | Cache de 20 GB (20 480 slots), blocos gravados de ~74 KiB em média* | 1,1 MB | 1,1 MB + 2,3 MB (≈ 283 mil blocos) = **≈ 3,4 MB** |
  | Registro `COMMIT_PIECE` de uma peça de 1 MiB com 14 blocos | 80 B | 192 B |

  \* Média do chunker ≈ 148 KiB crus ([03 §11.1](03-formato-de-conteudo.md#111-chunker-determinismo)) com compressão de ~2:1, a confirmar na F1-03. O custo cresce com o número de blocos, não com o tamanho: conteúdo muito comprimível (blocos de poucos bytes) aumenta a região de checksums, até 8 B por bloco.
- **Decisão da F1-11:** em RAM, cada entrada tem o próprio vetor de checksums, reservado na escrita (thread dona). Isso dá O(1) para o bloco *b* e nenhuma alocação na leitura, sem arena e, portanto, sem buracos para compactar. O custo é um vetor por peça (24 B + o cabeçalho do bloco de memória), aceitável perto dos 56 B da entrada; se a medição da F1-15 pedir, troca-se por uma arena sem mudar o formato em disco.
- Em RAM, o arranjo exato (tabela de entradas + arena de checksums) é decisão da F1-11, com duas regras: achar o checksum do bloco *b* de uma peça em O(1) e nenhuma alocação no caminho de leitura (a lista é reservada no passo 6 da escrita, na thread dona). A arena **terá buracos** quando peças saírem (remoção CLOCK); a F1-11 decide como reaproveitá-los ou compactá-los em RAM. No disco a compactação já acontece no checkpoint (acima).
- **Checkpointing periódico em background**:
  1. A thread de I/O descarrega o estado atual do índice para `index.bin.tmp`.
  2. Executa `fsync(index.bin.tmp)`.
  3. Realiza substituição atômica via rename (`rename` + `fsync` da pasta no POSIX; `MoveFileExW` com `MOVEFILE_WRITE_THROUGH` no Windows, que só volta com o rename no disco).
  4. Grava registro `CHECKPOINT(lsn)` no `journal.bin` e trunca logs anteriores ao `checkpoint_lsn`. **Como a F1-11 faz:** escreve um journal novo em `journal.bin.tmp` (cabeçalho com `checkpoint_lsn` e o registro `CHECKPOINT`), sincroniza e troca por rename, do mesmo jeito que o índice. Uma queda entre o rename do índice e o do journal deixa o índice novo com o journal antigo, cujos registros têm LSN ≤ `checkpoint_lsn` e são pulados.
- A F1-11 grava um checkpoint ao abrir (depois da recuperação), ao parar sem erro e quando o journal passa de `checkpoint_journal_bytes` (64 MiB por padrão).

### 5. Algoritmo de Recuperação de Falhas (Crash Recovery)
Ao inicializar o subsistema de cache:
1. Lê o cabeçalho de `journal.bin` e verifica integridade do checksum. Se corrompido, utiliza backup ou reconstrói verificando hashes nas stores. O mesmo vale para o `index.bin` (XXH3-64 do arquivo no cabeçalho): se não bater, o índice é descartado e reconstruído re-verificando a raiz Merkle (SHA-256) de cada slot ocupado contra o manifesto e recalculando os checksums por bloco dos slots aprovados. É lento (lê o cache inteiro), mas não confia em nada não verificado.
2. Posiciona no último checkpoint válido.
3. Percorre sequencialmente os registros de transação:
   - Valida `record_magic` e `record_type`; para `COMMIT_PIECE`, confere, **nesta ordem e antes de ler os checksums**: `1 ≤ piece_size ≤ 1 048 576`, `1 ≤ block_count ≤ piece_size` e que o fim do registro (`72 + 8·block_count + 8`) cabe no arquivo. O teto de `piece_size` importa porque `block_count` e `piece_size` vêm do mesmo registro, que pode estar corrompido: sem ele, o único limite seria "cabe no arquivo", e o leitor poderia ler e hashear o journal inteiro antes de descartar o registro; com ele, o pior caso é ≈ 8 MiB. Para os outros tipos, `block_count == 0`. Depois valida `record_crc` (XXH3-64 do registro inteiro, incluindo os checksums).
   - Ao encontrar o primeiro registro truncado ou inválido (escrita incompleta durante queda / torn write): interrompe a leitura imediatamente, truncando o log naquele ponto.
   - **Sem ressincronização, e isso é aceito.** Com registros de tamanho variável não há como achar o próximo registro depois de um inválido, então uma corrupção no **meio** do log (e não só na cauda rasgada) descarta tudo o que veio depois. A política "trunca no primeiro inválido" já era esta; o custo é que as peças commitadas depois daquele ponto viram slots órfãos (passo 4) e são baixadas ou re-verificadas de novo. Nunca se perde dado verificado: só se perde a indexação dele.
   - Aplica os registros válidos: `COMMIT_PIECE` adiciona a entrada **com a sua lista de checksums por bloco**; `FREE_SLOT` remove a entrada e a lista.
   - Depois de aplicar, toda entrada cujo `block_count` difere do `block_count` da peça no manifesto em uso é descartada e o slot fica livre (a peça é de um manifesto com outros limites de bloco).
   - `block_count` igual **não prova** o mesmo layout: dois manifestos podem partir os mesmos bytes da peça em blocos diferentes com o mesmo número de blocos (possível com `codec = NONE`). Não há risco de dado errado: o checksum do bloco não bate na primeira leitura, a peça é invalidada e baixada de novo (passo 5). O custo é só um download a mais, raro (exige trocar de manifesto mantendo a peça com outra partição), e não justifica guardar um resumo do layout no v0.
4. Identifica slots ocupados no store que não constam no índice reconstruído: marca-os como livres para reutilização imediata.
   - **O que a F1-11 também descarta aqui:** entradas além do teto (o teto diminuiu entre aberturas; os `store-XXX.bin` além do teto são apagados e o último é encurtado) e entradas num `store-XXX.bin` que não existe (no Windows não há `fsync` de pasta: um arquivo criado logo antes de uma queda pode sumir). As peças saem do índice; nada vira erro fatal.
   - **Ainda não feito (falta o manifesto no `sw-core`):** a conferência de `block_count` contra o manifesto em uso (passo 3) e a reconstrução por re-verificação Merkle quando o `index.bin` e o journal estão perdidos (passo 1). Até lá, cabeçalho do journal inválido descarta os registros e vale só o índice; índice inválido descarta o índice e vale só o journal. Nos dois casos se perde indexação, nunca se aceita dado não verificado.
5. **Verificação de leitura subsequente**: todo bloco lido é conferido com **o XXH3-64 do próprio bloco** (`cks[cks_first + i]` para o *i*-ésimo bloco da peça), calculado sobre os bytes gravados desse bloco (até 256 KiB), antes de ser entregue à memória do jogo; não é preciso ler a peça inteira (até 1 MiB). Bloco com checksum inválido invalida a **peça inteira** (o slot é liberado e a peça é pedida de novo), porque o defeito de disco pode atingir outros blocos do mesmo slot.

## Concorrência

```
 thread do jogo ─┐                      ┌─ threads de descompressão (pool)
 thread de stream┼─► fila de pedidos ─► │  THREAD DE I/O (dona do cache)
 outras threads ─┘   (lock-free MPSC)   └─ escreve/lê slots, índice, remoção
```

- **Uma única thread de I/O é dona** do índice e dos slots. As outras só mandam pedidos e recebem respostas.
- **Leases**: ler um bloco devolve um lease (contagem de referência). Peça com lease ativo **nunca** é removida nem sobrescrita.
  - **Como a F1-13 faz:** o lease é da peça inteira, pedido à dona (`CacheOp::Acquire`) e devolvido num `PieceLease` (RAII, só se move). A contagem é um atômico por slot: só a dona soma; qualquer thread solta, sem fila, sem trava e sem alocar (`release`), e a dona lê com `acquire` antes de liberar um slot. O único ponto que libera slot de peça (`invalidate`) recusa com `SlotBusy` se houver lease: `Remove` de peça presa devolve `SlotBusy` sem gravar `FREE_SLOT`. Um bloco que não confere numa peça presa a marca como estragada: ela nunca mais é servida (`CorruptedChecksum`), mas o slot só sai no primeiro pedido que a tocar depois do último lease. Leases não sobrevivem a fechar e abrir o cache. A remoção CLOCK (F1-14) tem de pular slots presos.
- **Leitura de acerto** pode ser servida sem passar pela thread de I/O (leitura direta do arquivo mapeado ou com `pread`) se o lease já estiver pego — o caminho quente não depende de fila.
- A thread principal do jogo **nunca** bloqueia em remoção, compactação ou gravação.

## Política de remoção

- **Conjunto fixo** (nunca sai): grupo `core`, UI, boot, e o que o jogo marcar como fixo.
- **Algoritmo**: começar com **CLOCK** (simples, barato). Avaliar **W-TinyLFU** ou **Segmented LRU** na Fase 1 com traces reais. LRU puro é descartado: um fast travel expulsaria o que o jogador usa sempre.
- **Marcas d'água**: remoção começa em 95% do teto e para em 85%. Roda em background, com baixa prioridade de I/O.
- **Prefetch não pode expulsar dado quente**: peças trazidas por prefetch entram como "frias" até serem lidas uma vez.

## Memória (RAM)

- Cache de blocos **descomprimidos** em RAM, pequeno (ex.: 256–1024 MB), também com leases.
- Buffers reaproveitados em pool. Nada de alocação por leitura no caminho quente.

## Métricas que o cache publica
Taxa de acerto (RAM e disco), faltas por minuto, tempo de espera p50/p95/p99 por falta, bytes removidos, peças inválidas, uso do teto. Ver [08-metricas-e-testes.md](08-metricas-e-testes.md).

## Questões em aberto
- `mmap` ou `pread` + buffers próprios? Medir no Windows (onde `mmap` grande tem custo de page fault diferente).
- Integração com DirectStorage no Windows: Fase 4 ou depois.
