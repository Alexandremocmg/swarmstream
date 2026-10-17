# sw-services

Backend e infraestrutura do Swarmstream — trilha **T5 (Serviços)**.

> **Estado:** Fase 0. Pasta criada pelo [F0-01](../docs/11-backlog-inicial.md);
> a linguagem do backend ainda **não foi decidida** (ADR pendente). Não escreva
> código aqui antes do ADR de backend ser aceito.

## O que vai morar aqui

- Manifest service e tracker próprio (F4)
- Contas de estúdio, painel do estúdio, monitoramento e cobrança (F4, ADR-0009)
- Coletor de telemetria (F4)
- coturn na bancada de testes (F2-09)

## Enquanto isso

A bancada de rede simulada (`sw-netlab`) é Python e já existe; ela não é
`sw-services`. Serviços só entram aqui após o ADR.

## Referências

- [docs/02-arquitetura.md](../docs/02-arquitetura.md) — contratos entre módulos
- [docs/09-roadmap-fases.md](../docs/09-roadmap-fases.md) — trilha T5
