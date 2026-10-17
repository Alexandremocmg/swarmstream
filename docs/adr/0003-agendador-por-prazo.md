# ADR-0003: Agendador por prazo

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
A proposta original punha o P2P como prioridade 1 e a CDN como reserva. Upload residencial é baixo e variável; um jogo esperando um peer lento trava.

## Decisão
Todo pedido tem um **prazo**. O agendador escolhe a fonte pela folga: folga longa → peers; folga curta ou peer atrasado → CDN (com pedido duplicado, *hedging*, quando o atraso é provável). Instalação e patches sem prazo preferem peers.

## Consequências
- (+) O jogo nunca fica pior do que com a CDN sozinha.
- (+) O P2P absorve o volume onde é bom (background, prefetch longo).
- (−) Pedidos duplicados gastam um pouco de CDN; medir e limitar.
- (−) A % de bytes vindos de peers pode ficar abaixo dos 60–80% da proposta; a Fase 3 mede o valor real.
