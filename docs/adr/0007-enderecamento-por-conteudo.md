# ADR-0007: Endereçamento por conteúdo em vez de swarm por versão

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
A proposta original isolava clientes de versões antigas do swarm. Isso divide o swarm a cada patch e desperdiça os blocos iguais entre versões.

## Decisão
- Blocos e peças são identificados pelo **hash do conteúdo**.
- Corte de blocos por **FastCDC** (definido pelo conteúdo), para que mudanças pequenas afetem poucos blocos.
- O manifesto de cada versão lista os hashes válidos; o cliente só aceita o que o seu manifesto assinado lista.

## Consequências
- (+) Patches pequenos; o swarm não se divide a cada versão.
- (+) A segurança vem do hash + manifesto assinado, não do isolamento.
- (−) Agrupar blocos em peças fica mais complexo (peças podem mudar entre versões); estudar uma estratégia de "peças estáveis" na Fase 4.
