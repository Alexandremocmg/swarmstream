# Arquitetura

Este documento é a referência técnica do sistema. Ele **corrige e detalha** a [proposta original](00-proposta-original.md). Cada decisão importante tem um ADR em [adr/](adr/README.md).

## Visão em uma frase

O jogo lê arquivos de um **sistema de arquivos virtual (VFS)** que só serve dados do **cache local**; um **agendador por prazo** enche esse cache buscando **chunks endereçados por conteúdo** em **peers** quando há tempo e na **CDN** quando o prazo aperta, validando cada pedaço contra um **manifesto assinado**.

## Diagrama de componentes

```
 BUILD (estúdio)                                CLIENTE (PC do jogador)
 ───────────────                                ─────────────────────────────────────────────
 arquivos do jogo                               ┌───────────────────────────────────────────┐
      │                                         │  JOGO (Godot / Unreal / Unity)            │
      ▼                                         │   └─ SDK: open/read + hints + miss policy │
 ┌──────────┐   manifesto assinado              └──────────────┬────────────────────────────┘
 │ sw-pack  │──────────────┐                                   │ C ABI (leitura síncrona/assíncrona)
 │ chunker  │              │                    ┌──────────────▼────────────────────────────┐
 │ compress │   chunks     │                    │  sw-core                                   │
 │ sign     │──────┐       │                    │   ├─ VFS ......... caminho → chunks        │
 └──────────┘      │       │                    │   ├─ Cache ....... slots, leases, eviction │
                   ▼       ▼                    │   ├─ Prefetcher .. hints + traces          │
 BACKEND      ┌─────────┐ ┌──────────────┐      │   ├─ Scheduler ... fila por prazo          │
 ───────      │   CDN   │ │ manifest svc │◄─────┤   ├─ Verifier .... Merkle SHA-256          │
              │(objetos)│ └──────────────┘      │   ├─ Fontes ...... P2P (libtorrent) | CDN  │
              └────▲────┘ ┌──────────────┐      │   ├─ QoS ......... limites de upload       │
                   │      │ tracker/DHT  │◄─────┤   └─ Telemetria                            │
                   │      └──────────────┘      └───────┬───────────────────────┬────────────┘
                   │      ┌──────────────┐              │ peças urgentes        │ peças com folga
                   └──────┤  STUN        │              ▼                       ▼
                          └──────────────┘          CDN (web seed)       outros peers
                                                                          (seeders completos
                                                                           e jogadores)
```

## Módulos e responsáveis

Os módulos se comunicam só por **interfaces definidas** (seção [Contratos](#contratos-entre-módulos)). Isso permite que equipes diferentes trabalhem em paralelo usando *mocks* das outras partes.

| Módulo | O que faz | Linguagem | Doc detalhado |
| --- | --- | --- | --- |
| `sw-pack` | Ferramenta de build: divide, comprime, calcula hashes, gera e assina o manifesto, gera o torrent v2. | C++20 (núcleo) + CLI | [03-formato-de-conteudo.md](03-formato-de-conteudo.md) |
| `sw-core` | Biblioteca do cliente: VFS, cache, prefetch, agendador, fontes, verificação, QoS, telemetria. | C++20, API em C | [04-cache-local.md](04-cache-local.md), [05-rede-e-agendador.md](05-rede-e-agendador.md) |
| `sw-godot` | GDExtension que liga o Godot ao `sw-core`. | C++ | [06-integracao-engine-sdk.md](06-integracao-engine-sdk.md) |
| `sw-launcher` | App do jogador: instalação, modos compacto/completo, configurações de upload. Aberto ou gratuito ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)). | Tauri 2: Rust + UI web ([ADR-0011](adr/0011-tecnologia-do-launcher.md)) | — |
| `sw-services` | Manifest service, tracker, STUN, coletor de telemetria. Hospedado por nós, multi-cliente, com protocolos abertos ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)). | Go ([ADR-0010](adr/0010-linguagem-dos-servicos-de-backend.md)) | [05-rede-e-agendador.md](05-rede-e-agendador.md) |
| `sw-netlab` | Bancada de testes: servidor CDN simulado, peers em containers, rede degradada. | Python + Docker | [08-metricas-e-testes.md](08-metricas-e-testes.md) |

Decisão de linguagem do núcleo: [ADR-0006](adr/0006-cpp20-nucleo-com-api-c.md).

## Fluxo de uma leitura

1. O jogo chama `sw_read(arquivo, offset, tamanho)` (ou a versão assíncrona).
2. O **VFS** traduz o trecho do arquivo para uma lista de **blocos** (unidade de compressão).
3. Para cada bloco, o **Cache** responde:
   - **acerto** → entrega um *lease* e os bytes já descomprimidos (ou descomprime numa thread de trabalho);
   - **falta** → cria um pedido com **prazo = agora + orçamento da falta** e chama a **política de falta** do jogo (ver [06](06-integracao-engine-sdk.md#política-de-falta)).
4. O **Scheduler** coloca a **peça** que contém o bloco na fila por prazo. Se a folga for grande, pede aos peers; se for curta, ou se os peers não entregarem a tempo, pede à CDN.
5. Cada pedaço recebido passa pelo **Verifier** (hash Merkle). Pedaço inválido é descartado e o peer é penalizado.
6. O Cache grava a peça (dados primeiro, índice depois) e acorda quem esperava.

Em paralelo, o **Prefetcher** recebe dicas do jogo e traces gravados e enfileira peças com prazos longos, para que a maioria das leituras seja acerto.

## Contratos entre módulos

Estes contratos devem existir **antes** do código das partes. Versão inicial na Fase 0 ([09-roadmap-fases.md](09-roadmap-fases.md)).

| Contrato | Entre | Forma |
| --- | --- | --- |
| Formato do manifesto | `sw-pack` → `sw-core`, `sw-services` | FlatBuffers em envelope assinado com Ed25519 ([ADR-0012](adr/0012-serializacao-do-manifesto.md)) |
| Layout de chunk | `sw-pack` → `sw-core` | Especificação binária em [03](03-formato-de-conteudo.md) |
| API pública do núcleo | `sw-core` → SDKs | Header C [`swarmstream.h`](../sw-core/include/swarmstream.h) v0 ([ADR-0014](adr/0014-contrato-swarmstream-h-v0.md)) |
| `IContentSource` | Scheduler → fontes | `request(peça, prazo) → future<bytes>`, `cancel()`, estatísticas |
| `ICacheStore` | Cache → disco | `alloc_slot`, `write`, `commit`, `read(lease)`, `free` |
| Eventos de telemetria | todos → coletor | Lista de eventos e campos em [08](08-metricas-e-testes.md) |

## Respostas à pauta técnica

### 1. Tamanho de chunk
Dois níveis, não um ([ADR-0002](adr/0002-chunk-em-dois-niveis.md)):
- **Bloco de compressão: 64 KB a 256 KB** (padrão inicial 128 KB), descomprimível sozinho (Zstd ou LZ4). Evita baixar 4 MB para ler uma textura de 300 KB.
- **Peça P2P: 1 MB**, unidade de troca entre peers.
- **Árvore de Merkle com folhas de 16 KB**, para validar partes da mesma peça vindas de peers diferentes.
- O stutter não vem do tamanho do chunk se a descompressão for assíncrona; vem da **espera de rede numa falta**. A Fase 1 mede isso.

### 2. NAT traversal
Sem TURN para assets e sem depender de EOS/Steamworks ([ADR-0004](adr/0004-sem-turn-cdn-como-reserva.md)):
- STUN próprio (barato), hole punching, UPnP/NAT-PMP.
- Quem não conecta direto usa a CDN; relay custaria o mesmo que a CDN.
- Steam Datagram Relay e EOS P2P foram feitos para tráfego de partida e prendem a um ecossistema.

### 3. Cache sem conflito com a thread principal
Thread de I/O dona do cache, *leases* com contagem de referência, slots pré-alocados, dados gravados antes do índice, remoção em background com CLOCK/W-TinyLFU. Detalhes em [04-cache-local.md](04-cache-local.md) e [ADR-0005](adr/0005-cache-em-slots-com-leases.md).

### 4. PoC
Sim: **Godot 4 + libtorrent-rasterbar** (torrent v2, `set_piece_deadline`, web seeds), começando **sem P2P** contra uma rede simulada ([ADR-0001](adr/0001-poc-godot-libtorrent.md), [ADR-0003](adr/0003-agendador-por-prazo.md)).

## Correções em relação à proposta original

| Proposta | Decisão | Por quê |
| --- | --- | --- |
| "Prioridade 1: P2P, CDN de fallback" | Agendador **por prazo** escolhe a fonte | P2P é ótimo em volume, ruim em urgência; o jogo não pode esperar o swarm. |
| Chunks de 1–4 MB "alinhados ao MTU" | Blocos de 128 KB + peças de 1 MB | MTU é ~1.500 bytes e não tem relação; blocos grandes aumentam a espera numa falta. |
| SHA-256 como "DRM" | Integridade (hash), autenticidade (assinatura), sigilo (cifra opcional) | São três problemas diferentes; ver [07](07-seguranca-e-privacidade.md). |
| Isolar clientes antigos do swarm | **Endereçamento por conteúdo** | Versões compartilham os chunks iguais; o hash já impede dado incompatível. |
| Prever só por vetor de câmera | Dicas do jogo + **traces gravados** + índice espacial | Generaliza melhor entre jogos. |
| LRU puro | CLOCK / W-TinyLFU + conjunto fixo | Fast travel não pode expulsar o que se usa sempre. |
| Tit-for-tat + créditos "Fast Lane" | Adiado para depois do MVP | A maioria do volume vem de seeders completos; créditos são vulneráveis a contas falsas. |
