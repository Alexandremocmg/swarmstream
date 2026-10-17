# ADR-0002: Chunk em dois níveis

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
A proposta original sugeria chunks de 1–4 MB "alinhados ao MTU". O MTU (~1.500 bytes) não tem relação com essa escala. Blocos grandes aumentam a espera numa falta (4 MB a 20 Mbps ≈ 1,6 s) e a amplificação de leitura; blocos pequenos demais aumentam a sobrecarga de protocolo e de índice.

## Decisão
- **Bloco** (compressão/leitura): 128 KB padrão, configurável de 64 a 256 KB, descomprimível sozinho.
- **Peça** (troca P2P/CDN): ~1 MB.
- **Folha Merkle**: 16 KB (padrão do BitTorrent v2).

## Consequências
- (+) Faltas pequenas baixam pouco; peers trocam unidades eficientes.
- (+) Verificação de partes vindas de vários peers.
- (−) Índice maior do que com blocos de 4 MB; medir o tamanho do manifesto na Fase 0.
- Os valores finais saem das medições da Fase 1.
