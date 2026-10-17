# ADR-0004: Sem TURN para assets; CDN é a reserva

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
Pergunta da pauta: STUN/TURN próprios ou EOS/Steamworks P2P? Relay (TURN) de GBs de assets custa tanto quanto servir pela CDN. Steam Datagram Relay e EOS P2P foram feitos para tráfego de partida e prendem a um ecossistema.

## Decisão
- STUN próprio (coturn só STUN), hole punching UDP, UPnP/NAT-PMP.
- **Nenhum relay para assets.** Quem não conecta direto usa a CDN.
- Não depender de EOS/Steamworks para transporte; podem servir para descoberta ou identidade no futuro, se útil.

## Consequências
- (+) Custo de infraestrutura baixo; independência de plataforma.
- (−) Jogadores atrás de CGNAT simétrico não participam do P2P. Medir a proporção na Fase 3.
