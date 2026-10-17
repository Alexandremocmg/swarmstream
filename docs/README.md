# Documentação

| Doc | Conteúdo | Público |
| --- | --- | --- |
| [00 — Proposta original](00-proposta-original.md) | O texto inicial da ideia, guardado como registro | Todos |
| [01 — Visão e escopo](01-visao-e-escopo.md) | Problema, valor, princípios, escopo, referências de mercado | Produto, todos |
| [02 — Arquitetura](02-arquitetura.md) | Componentes, fluxo de leitura, contratos, respostas à pauta técnica | Todos os devs |
| [03 — Formato de conteúdo](03-formato-de-conteudo.md) | Blocos, peças, manifesto, compressão, versões | T1, T2 |
| [04 — Cache local](04-cache-local.md) | Modos de instalação, disco, concorrência, remoção | T2 |
| [05 — Rede e agendador](05-rede-e-agendador.md) | Prazos, libtorrent, NAT, tracker, QoS, cold-start, serviços | T4, T5 |
| [06 — Integração com engine](06-integracao-engine-sdk.md) | API C, política de falta, prefetch, SDK Godot | T3 |
| [07 — Segurança e privacidade](07-seguranca-e-privacidade.md) | Ameaças, LGPD/GDPR, software do jogador | T8, todos |
| [08 — Métricas e testes](08-metricas-e-testes.md) | Métricas, telemetria, bancada `sw-netlab`, tipos de teste | T6, todos |
| [09 — Roadmap por fases](09-roadmap-fases.md) | Fases F0–F6, atividades por trilha e nível, portões | Todos |
| [10 — Equipe e níveis](10-equipe-e-niveis.md) | O que cada nível assume, composição, rituais, entrada | Todos |
| [11 — Backlog inicial](11-backlog-inicial.md) | Tarefas de F0–F2 com nível, tamanho e critério de aceite | Todos os devs |
| [12 — Riscos](12-riscos.md) | Riscos, mitigação e quando se verificam | Produto, arquiteto |
| [13 — Skills do Claude](13-skills-claude.md) | Skills instaladas no projeto, por nível e por trilha | Todos os devs |
| [14 — Briefing da Fase 0](14-briefing-fase-0.md) | O que pedir a cada pessoa na Fase 0, com exemplos de pedido | Todos, dono do produto |
| [15 — Portão da Fase 0 e plano para 3 recursos](15-portao-fase-0-e-plano-3-recursos.md) | Parecer sobre a passagem para a Fase 1, lista de fechamento e novo plano para 3 pessoas | Dono do produto, líder técnico |
| [Spike F0 — FileAccess no Godot](spikes/F0-spike-fileaccess-godot.md) | Onde dá para interceptar a leitura de arquivos via GDExtension, riscos e plano de 2 dias | T2, T3 |
| [Glossário](glossario.md) | Termos do projeto | Todos |
| [ADRs](adr/README.md) | Decisões de arquitetura | Todos os devs |
| [Modelos](templates/) | Modelo de ADR e de tarefa | Todos |

Material de estudo na raiz do projeto: `Exemplo_Engine_Navegador.md` (protocolo de I/O com hints e cache persistente de um porte de engine para navegador — útil como referência das camadas 1 e 2).

## Regras da documentação
- Português, frases curtas, um assunto por arquivo.
- Mudou uma decisão? Escreva um ADR novo e atualize o doc afetado no mesmo PR.
- Números de desempenho só com a medição que os gerou (rodada da bancada, data, perfil de rede).
