# Rede e agendador por prazo

Responsável pela camada 3 (swarm, super-seeders) e 4 (CDN) da arquitetura, e pelos serviços de backend ligados à rede.

## Ideia central: cada pedido tem um prazo

Toda peça pedida carrega um **prazo** (quando o jogo vai precisar dela):

| Origem do pedido | Prazo típico | Fonte preferida |
| --- | --- | --- |
| Falta de cache (o jogo está esperando) | 50–300 ms | **CDN** direto, peers em paralelo se houver um rápido conhecido |
| Prefetch de curto prazo (próximos segundos) | 1–10 s | Peers rápidos; CDN se o prazo se aproximar |
| Prefetch de longo prazo (próxima região) | 30–120 s | Peers |
| Instalação, patch, modo completo em background | Sem prazo | **Só peers** quando houver; CDN com limite de taxa |

O agendador reavalia a fila continuamente. Se uma peça pedida a um peer não vai chegar antes do prazo (pela taxa observada), ela é **pedida também à CDN** ("hedging"), e o que chegar primeiro vence. Ver [ADR-0003](adr/0003-agendador-por-prazo.md).

## Implementação inicial: libtorrent-rasterbar

| Necessidade | Recurso do libtorrent |
| --- | --- |
| Prazo por peça | `torrent_handle::set_piece_deadline()` |
| CDN como seed infinito | Web seeds HTTP (BEP 19) |
| Integridade por partes | Torrent v2 (BEP 52), Merkle SHA-256 |
| NAT | UPnP, NAT-PMP, uTP; hole punching via extensão |
| Limite de upload | Classes de peer e limites de taxa |
| Descoberta | Tracker, DHT, Local Service Discovery (LAN) |

O `sw-core` usa o libtorrent **atrás da interface `IContentSource`**. Se no futuro trocarmos por QUIC próprio (ex.: iroh/libp2p), o resto do sistema não muda.

> Risco conhecido: o libtorrent assume que ele mesmo grava os arquivos. Precisamos de um **storage customizado** (interface `disk_interface`) que grave nos slots do nosso cache. Esta é uma tarefa sênior da Fase 2.

## NAT e conectividade

- **STUN** próprio (coturn em modo só STUN) para descobrir endereço público.
- **Hole punching** UDP (uTP/QUIC). **UPnP/NAT-PMP** quando o roteador aceitar.
- **Sem TURN/relay para assets** ([ADR-0004](adr/0004-sem-turn-cdn-como-reserva.md)): quem não conecta direto usa a CDN.
- Meta de medição: % de pares que conectam direto por tipo de NAT, na Fase 3.

## Descoberta e localidade

- **Tracker** próprio (HTTP/UDP) que responde peers priorizando: mesma LAN → mesmo ASN/ISP → mesma região → resto.
- O tracker usa uma base de IP → ASN/região (ex.: dados públicos de roteamento). Não guarda o histórico de IP dos jogadores além do necessário ([07](07-seguranca-e-privacidade.md)).
- **DHT** fica desligada no início (controle e privacidade); reavaliar depois.
- **LAN**: descoberta local ligada por padrão (casas e lan houses com vários PCs ganham muito).

## Swarm por conteúdo, não por versão

Os swarms são por **pacote/peça endereçada por hash**. Um cliente na versão 1.2 e outro na 1.3 trocam todas as peças que não mudaram. O manifesto assinado de cada cliente diz quais hashes ele aceita. Ver [ADR-0007](adr/0007-enderecamento-por-conteudo.md).

## QoS do jogador

- Medir latência da conexão (RTT até a CDN/STUN) continuamente. Se o RTT sobe com o upload ligado, **reduzir o upload** (estilo LEDBAT, que o uTP já implementa).
- Upload **desligado** durante partidas online competitivas (o SDK informa o estado do jogo).
- Detectar conexão com franquia (Windows informa "metered connection") e não fazer upload nela.
- Limites configuráveis no launcher: upload máximo, horários, "só quando o PC estiver ocioso".

## Cold-start de lançamento

- Dia 1: quase tudo vem da CDN. Peers ajudam entre si à medida que completam pacotes.
- **Pre-load**: distribuir os pacotes criptografados dias antes do lançamento (a chave chega no dia). É o momento em que o P2P tem mais tempo e mais valor.
- Meta a medir no piloto: curva de % de bytes vindos de peers por dia após o lançamento.

## Serviços de backend

| Serviço | Função | Estado mínimo para a PoC |
| --- | --- | --- |
| CDN / object storage | Guarda peças e manifestos; responde HTTP Range | Servidor HTTP local na `sw-netlab` |
| Manifest service | Publica o manifesto atual por jogo e canal (estável, beta) | Arquivo estático |
| Tracker | Lista peers com localidade | Tracker open source na Fase 2, próprio na Fase 4 |
| STUN | Endereço público | coturn |
| Telemetria | Recebe eventos do cliente | Endpoint simples que grava em arquivo (como o `/log` do porte de referência) |

## Incentivos (pós-MVP)
Tit-for-tat padrão do BitTorrent já basta no início. O sistema de créditos "Fast Lane" fica para depois do MVP, com estudo anti-fraude (contas falsas inflando créditos).
