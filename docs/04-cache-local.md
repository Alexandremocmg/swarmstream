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

#### Registros de Transação (Record Entry)
Cada mutação no cache gera um registro append-only de tamanho fixo ou delimitado com soma de verificação:
```text
offset  tam  campo          descrição
0       4    record_magic   = 0x57414C52 ("WALR")
4       2    record_type    = COMMIT_PIECE (1) | FREE_SLOT (2) | CHECKPOINT (3)
6       2    flags          = flags adicionais (ex.: 0x01 = fixo/pinned)
8       8    lsn            = Log Sequence Number (estritamente monotônico crescente)
16      8    timestamp_utc  = época em microssegundos
24      32   piece_hash     = SHA-256 raiz Merkle da peça (zeros para CHECKPOINT)
56      4    store_file_id  = índice do arquivo store (ex.: 0 para store-000.bin)
60      4    slot_id        = índice do slot no arquivo store
64      4    piece_size     = tamanho gravado da peça (bytes)
68      4    reserved       = 0
72      8    record_crc     = XXH3-64 do registro [0..72)
Total: 80 bytes por registro (compacto e alinhável a 8 bytes).
```

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

- **Por que esta ordem (Data-First, then WAL Commit)?**
  - Se a máquina travar antes do Passo 5: o journal **não** contém o commit. Na inicialização, a transação não existe; o slot é considerado lixo e reutilizado. O engine **nunca** lerá dados parciais nem zeros.
  - Se travar após o Passo 5: o journal contém o registro válido e com checksum correto. A recuperação do Passo 3 já garante que os dados físicos foram persistidos no slot antes do journal. O registro é reexecutado com 100% de integridade.

### 4. Formato do Índice (`index.bin`) e Checkpoints
- O `index.bin` armazena uma tabela hash ou densa dos slots:
  - `piece_hash` (32 B) → `(store_file_id: u16, slot_id: u32, size: u32, state: u8, flags: u8, access_counter: u32, checksum_xxh3: u64)`.
- **Checkpointing periódico em background**:
  1. A thread de I/O descarrega o estado atual do índice para `index.bin.tmp`.
  2. Executa `fsync(index.bin.tmp)`.
  3. Realiza substituição atômica via rename (`rename` / `ReplaceFileW` no Windows).
  4. Grava registro `CHECKPOINT(lsn)` no `journal.bin` e trunca logs anteriores ao `checkpoint_lsn`.

### 5. Algoritmo de Recuperação de Falhas (Crash Recovery)
Ao inicializar o subsistema de cache:
1. Lê o cabeçalho de `journal.bin` e verifica integridade do checksum. Se corrompido, utiliza backup ou reconstrói verificando hashes nas stores.
2. Posiciona no último checkpoint válido.
3. Percorre sequencialmente os registros de transação:
   - Valida `record_magic` e `record_crc` (XXH3-64).
   - Ao encontrar o primeiro registro truncado (escrita incompleta durante queda / torn write): interrompe a leitura imediatamente, truncando o log naquele ponto.
   - Aplica os registros válidos: adiciona entradas de `COMMIT_PIECE` e remove entradas de `FREE_SLOT` da tabela do índice.
4. Identifica slots ocupados no store que não constam no índice reconstruído: marca-os como livres para reutilização imediata.
5. **Verificação de leitura subsequente**: todo bloco lido é conferido com checksum rápido XXH3-64 antes de ser entregue à memória do jogo; peças com checksum inválido são marcadas para re-download.

## Concorrência

```
 thread do jogo ─┐                      ┌─ threads de descompressão (pool)
 thread de stream┼─► fila de pedidos ─► │  THREAD DE I/O (dona do cache)
 outras threads ─┘   (lock-free MPSC)   └─ escreve/lê slots, índice, remoção
```

- **Uma única thread de I/O é dona** do índice e dos slots. As outras só mandam pedidos e recebem respostas.
- **Leases**: ler um bloco devolve um lease (contagem de referência). Peça com lease ativo **nunca** é removida nem sobrescrita.
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
