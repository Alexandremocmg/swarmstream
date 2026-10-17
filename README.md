# Swarmstream

Plataforma híbrida de distribuição e **streaming de assets de jogos** sob demanda: o jogador instala só o essencial e o resto chega enquanto joga, vindo de outros jogadores (P2P) e da CDN da publisher, com um cache local que o jogo lê como se fosse o disco.

> **Estado:** planejamento. ADRs 0001–0008 aceitos; núcleo open source sob Apache-2.0 ([ADR-0008](docs/adr/0008-licenca-open-source.md)). Ainda não há código; a [Fase 0](docs/09-roadmap-fases.md#f0--fundação--3-semanas) cria o repositório e a base.

## Comece por aqui

| Se você é... | Leia |
| --- | --- |
| Dono do produto / investidor | [Visão e escopo](docs/01-visao-e-escopo.md) · [Roadmap](docs/09-roadmap-fases.md) · [Riscos](docs/12-riscos.md) |
| Desenvolvedor chegando no projeto | [Arquitetura](docs/02-arquitetura.md) · [Glossário](docs/glossario.md) · [Como contribuir](CONTRIBUTING.md) · [Equipe e níveis](docs/10-equipe-e-niveis.md) |
| Quem vai pegar uma tarefa | [Backlog inicial](docs/11-backlog-inicial.md) (procure `boa-primeira-tarefa`) |

Índice completo: [docs/README.md](docs/README.md).

## A ideia em 4 frases

1. O build do jogo é dividido em blocos pequenos, comprimidos e identificados pelo hash do conteúdo, com um manifesto assinado pela publisher.
2. O jogo lê de um sistema de arquivos virtual que só serve dados do cache local.
3. Um agendador enche o cache: pede aos peers o que tem tempo de esperar e à CDN o que é urgente.
4. Quem instalou o jogo completo vira fonte rápida para quem joga em modo compacto.
