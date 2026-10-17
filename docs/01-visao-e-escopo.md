# Visão e escopo

## O problema

- Jogos AAA ocupam de 80 GB a 300 GB. Jogadores com SSD cheio **desinstalam** jogos, e um jogo desinstalado raramente volta a ser jogado.
- Cada patch grande obriga milhões de jogadores a baixar dezenas de GB da CDN da publisher no mesmo dia.
- A maior parte dos assets de um jogo não é usada em uma sessão típica: o jogador está numa região do mapa, num capítulo, num modo.

## A proposta de valor

| Para quem | O que ganha |
| --- | --- |
| **Jogador** | Instala ~15 GB e começa a jogar; o resto chega sob demanda. Patches menores e mais rápidos. Pode escolher instalar tudo para jogar offline. |
| **Publisher** | Menos jogadores desinstalando (retenção), tempo até o primeiro jogo menor, menos pico de egress em lançamentos e patches. |
| **Estúdio** | Um SDK que esconde rede, cache e integridade atrás de uma API de "abrir arquivo" com dicas de prefetch. |

> **Nota para o pitch:** em contratos grandes, o custo de CDN por GB já é baixo. O argumento mais forte tende a ser **retenção** (o jogo cabe no SSD) e **tempo até jogar**, com a economia de banda como bônus. Isso precisa ser medido no piloto (ver [08-metricas-e-testes.md](08-metricas-e-testes.md)).

## Princípios do produto

1. **O jogo nunca pode ficar pior do que com a CDN sozinha.** O P2P é otimização, nunca dependência. Se os peers falham, a CDN atende.
2. **O engine só lê do cache local.** Rede é problema do cliente, não do jogo.
3. **Toda falta de bloco tem um plano.** O jogo sabe o que fazer quando um asset não chegou a tempo (LOD baixo, espera controlada, tela de loading). Travar o frame não é plano.
4. **Respeitar a conexão do jogador.** Upload limitado, pausado em conexões ruins ou com franquia, e desligável.
5. **Medir antes de afirmar.** Cada fase tem métricas de saída; nenhuma promessa ao mercado sem número da bancada de testes.

## Escopo por horizonte

### Dentro do escopo (até o MVP)
- Pipeline de build que converte os arquivos do jogo em chunks endereçados por conteúdo + manifesto assinado.
- Biblioteca cliente (núcleo) com: sistema de arquivos virtual, cache local, agendador de downloads por prazo, P2P e fallback CDN.
- SDK para **Godot 4** (primeira engine, usada na PoC).
- Launcher mínimo para PC (Windows primeiro, Linux em seguida) com modo compacto e modo completo.
- Serviços de backend mínimos: tracker/descoberta, distribuição de manifestos, STUN, telemetria.

### Fora do escopo (por enquanto)
- Consoles (sistemas de arquivo e políticas fechados).
- Mobile.
- Cloud gaming (renderizar no servidor) — é outro produto.
- DRM próprio. A plataforma pode **transportar** conteúdo cifrado, mas não substitui o DRM da publisher.
- Sistema de créditos "Fast Lane" e incentivos econômicos (avaliar depois do MVP).

## Referências de mercado (para estudar)

| Referência | Por que importa |
| --- | --- |
| Microsoft Flight Simulator (2020/2024) | Streaming massivo de dados do mundo pela nuvem: valida que jogadores aceitam assets sob demanda. |
| PS4/PS5 "jogar enquanto baixa", Xbox Intelligent Delivery | Instalação em partes e conteúdo opcional. |
| Unreal Engine 5 (IoStore, conteúdo sob demanda) | A engine mais usada em AAA já caminha para assets via CDN; potencial concorrente e potencial integração. |
| Windows Delivery Optimization | P2P de patches que deu certo em escala. |
| Blizzard Downloader / Battle.net (P2P abandonado ~2016) | Caso de P2P que foi descontinuado. Entender os motivos é obrigatório antes de vender a ideia. |
| BitTorrent v2 / libtorrent | Base técnica madura: Merkle SHA-256, prazos por peça, web seeds. |
| Microsoft DirectStorage, Oodle/Kraken | Descompressão em blocos pequenos e I/O assíncrono em PC. |

## Decisões do dono do produto (2026-10-17)

- **Nome:** Swarmstream (definitivo).
- **Arquitetura:** ADRs 0001–0009 aceitos ([índice](adr/README.md)).
- **Licença:** núcleo open source sob **Apache-2.0** ([ADR-0008](adr/0008-licenca-open-source.md)).
- **Modelo de negócio (formato B):** núcleo aberto, backend hospedado por nós e pago, launcher aberto ou gratuito, protocolos abertos e documentados ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)).
- **Primeiro cliente-piloto:** estúdio indie com jogo grande em Godot. Publishers e outras engines ficam para a Fase 6.

## Perguntas em aberto para o dono do produto

- Plano de preços do backend hospedado e limites de um plano gratuito para o piloto.
- Launcher: código aberto ou apenas gratuito (pode esperar a Fase 4).
