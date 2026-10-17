# Proposta original (registro)

> Texto da proposta como foi apresentada pelo dono da ideia, guardado como referência histórica. As decisões técnicas que corrigem ou detalham este texto estão em [02-arquitetura.md](02-arquitetura.md) e em [adr/](adr/README.md). Onde houver conflito, **vale a arquitetura e os ADRs**.

## Proposta de Arquitetura: Plataforma Híbrida de Distribuição e Streaming Dinâmico de Assets P2P para Jogos

### 1. Visão geral do problema e oportunidade

Jogos AAA modernos alcançaram tamanhos exorbitantes (80 GB a 300 GB), sobrecarregando o espaço de armazenamento dos SSDs dos usuários e gerando custos colossais de banda de saída (egress) via CDN para as distribuidoras (publishers).

A solução: uma infraestrutura descentralizada de P2P Asset Streaming em runtime (execução local do jogo com carregamento de assets sob demanda). O sistema combina armazenamento flexível (modo streaming mínimo vs. modo instalação completa offline) com uma rede swarm distribuída para otimizar o I/O local e a eficiência da banda de rede.

### 2. Arquitetura em 4 camadas

```
+---------------------------------------------------------------------------------+
|                          1. GAME ENGINE INTEGRATION                             |
|          (Prediction Engine, Extended Prefetching & Dynamic Loader)             |
+---------------------------------------------------------------------------------+
                                       |
                                       v
+---------------------------------------------------------------------------------+
|                       2. LOCAL CACHE & BUFFER (RAM/SSD)                         |
|   (Modo Mínimo: Buffer Circular LRU | Modo Completo: Armazenamento Total)       |
+---------------------------------------------------------------------------------+
                                       |
            +--------------------------+--------------------------+
            | (Prioridade 1: P2P Swarm)|                          | (Fallback)
            v                          v                          v
+-----------------------+  +-----------------------+  +---------------------------+
| 3.1 P2P SWARM NETWORK |  |  3.2 SUPER-SEEDERS    |  |     4. INFINITE SEED      |
|  (QUIC/WebRTC, NAT)   |  | (Usuários Download)   |  |      (Publisher CDN)      |
+-----------------------+  +-----------------------+  +---------------------------+
```

### 3. Componentes técnicos

**A. Fracionamento, integridade e versionamento**
- Chunking adaptativo: na build, arquivos de mídia são divididos em blocos de 1 MB a 4 MB.
- Integridade (SHA-256): cada bloco tem hash validado contra um manifesto assinado pela publisher; blocos adulterados são descartados.
- Manifesto versionado: clientes em versões antigas são isolados do swarm principal.

**B. Prefetching e memória (runtime)**
- O engine nunca lê diretamente da rede; lê só do buffer RAM/SSD.
- Prefetch preditivo por vetores de câmera e posição do jogador: janela de 5–10 s em redes rápidas, 30–60 s em redes lentas, com LOD dinâmico.
- Descompressão assíncrona (LZ4 ou Zstandard) em threads secundárias.

**C. Conectividade P2P e NAT**
- Download do mesmo bloco a partir de vários peers em paralelo.
- Travessia de CGNAT com STUN/ICE/QUIC (QUIC, WebRTC ou libp2p).
- Descoberta de peers priorizando mesma cidade, estado ou ISP.

**D. Instalação flexível**
- Modo compacto / streaming (padrão): core do jogo (~15 GB) + cache circular ajustável (ex.: 20 GB, LRU).
- Modo completo / offline: jogo 100% no SSD.
- Super-seeder: jogadores no modo completo servem os jogadores em streaming.

**E. CDN híbrida e cold-start**
- A CDN da publisher é o "seed infinito" de fallback.
- Lançamento: começa CDN-centric e migra para P2P-centric (60–80% do tráfego pelo swarm).

### 4. Incentivos e QoS
- QoS local automático: upload pausado ou limitado se a conexão da casa estiver lenta.
- Tit-for-tat adaptativo.
- Sistema de créditos "Fast Lane" para quem compartilha em background.

### 5. Go-to-market
- Fase 1: PC (Steam, Epic, launchers próprios).
- Fase 2: B2B com publishers (redução de CDN e retenção em SSDs cheios).
- Fase 3: SDKs para Unreal, Unity e Godot.

### 6. Perguntas para o especialista
1. Tamanho ideal de chunk (1, 2 ou 4 MB)?
2. STUN/TURN próprios vs. EOS / Steamworks P2P?
3. Como limpar o cache LRU sem conflito com a thread principal?
4. PoC com Godot/Unreal + libtorrent/WebTorrent?

As respostas estão em [02-arquitetura.md](02-arquitetura.md#respostas-à-pauta-técnica).
