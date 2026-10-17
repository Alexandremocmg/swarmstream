# Equipe, níveis e divisão do trabalho

Como organizar vários desenvolvedores de níveis diferentes no projeto, o que cada nível pode assumir e como evitar que o trabalho de um bloqueie o outro.

## Níveis e o que se espera de cada um

| Nível | Autonomia | Pode ser dono de | Precisa de revisão de |
| --- | --- | --- | --- |
| **Júnior (J)** | Tarefas bem definidas, com critério de aceite claro | Testes, ferramentas, scripts da bancada, telas, overlay de debug, documentação, conteúdo do mapa de teste | Um pleno ou sênior em todo PR |
| **Pleno (P)** | Funcionalidades inteiras dentro de um módulo, a partir de uma especificação | Componentes como remoção de cache, fonte HTTP, classificador do `sw-pack`, launcher, serviços simples | Um sênior em código de núcleo; outro pleno no resto |
| **Sênior (S)** | Desenha e implementa partes críticas; quebra trabalho para plenos e juniores | Cache em disco, agendador, integração com libtorrent, GDExtension, assinatura, prefetch espacial | Outro sênior ou o arquiteto em código crítico |
| **Arquiteto / Tech lead (A)** | Define contratos e decisões; responde pelos portões de fase | ADRs, contratos entre módulos, revisão de segurança, relatórios técnicos | Dono do produto (decisões de escopo) |

**Código crítico** (exige revisão de sênior ou arquiteto): verificação de hash e assinatura, parsing de dados de rede, journal e índice do cache, agendador, API pública `swarmstream.h`.

## Matriz trilha × nível

O que cada nível costuma fazer em cada trilha (detalhe por tarefa em [11-backlog-inicial.md](11-backlog-inicial.md)):

| Trilha | Júnior | Pleno | Sênior | Arquiteto |
| --- | --- | --- | --- | --- |
| **T1 Conteúdo/build** | Testes do chunker; CLI e mensagens; relatórios de tamanho | Classificação core/grupos; compressão; geração de torrent | FastCDC, ordenação de peças, Merkle, patches | Formato do manifesto |
| **T2 Cache/VFS** | Testes de unidade; ferramenta de inspeção do cache | Remoção CLOCK; cache em RAM; VFS | Slots, journal, leases, recuperação | `ICacheStore` |
| **T3 Engine** | Mapa de teste; rota de câmera; overlay; exemplos | Prefetch por áreas; mips/LODs; plugin de editor | GDExtension; política de falta; índice espacial | API do SDK |
| **T4 Rede** | Telemetria de rede; scripts de teste | Fonte HTTP; QoS; integração do libtorrent no build | Fonte P2P; storage do libtorrent; agendador; NAT | `IContentSource`, estratégia de fontes |
| **T5 Serviços** | Painéis; scripts de deploy | Tracker, manifest service, coletor | Arquitetura de deploy e escala | Escolha de tecnologia |
| **T6 Bancada/QA** | Rodar rodadas; relatórios; servidor de rede simulada | Peers em containers; testes de queda; CI | Simulação em escala; metodologia de medição | Métricas de portão |
| **T7 Launcher** | Telas e textos | Fluxos de instalação e configurações | Instalador, atualização do próprio launcher | — |
| **T8 Segurança** | Casos de teste de peer malicioso | Fuzzing; penalização de peers | Assinatura, cifra, revisão de parsing | Modelo de ameaças |

## Composição da equipe

> **Situação atual (2026-10-17): 3 recursos, plano autorizado.** O cronograma, a distribuição de tarefas e a regra de revisão para essa equipe estão em [15-portao-fase-0-e-plano-3-recursos.md](15-portao-fase-0-e-plano-3-recursos.md). As tabelas abaixo descrevem a equipe de 7 pessoas originalmente planejada.

| Formato | Pessoas | Para quê |
| --- | --- | --- |
| **Mínimo** | 1 A/S + 1 P + 1 J | F0–F1 em ~4–5 meses em vez de ~3. Válido para validar o Portão 1 com pouco orçamento. |
| **Base (recomendado)** | 1 A + 2 S + 2 P + 2 J | Cronograma de [09-roadmap-fases.md](09-roadmap-fases.md). |
| **Ampliado** | Base + 1 S de backend/operação **já na F2** (formato B, [ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)) + 1 P de launcher e 1 QA a partir da F4 | Serviço hospedado pago, MVP e piloto. |

Perfil desejado dos sêniores: um com experiência em **engines / streaming de assets / C++ de baixa latência**, outro em **redes / P2P / sistemas distribuídos**.

## Donos por módulo (CODEOWNERS)

Cada pasta do repositório tem um dono (um sênior ou o arquiteto) que aprova os PRs dela. Sugestão inicial:

```
/docs/adr/           @arquiteto
/sw-core/cache/      @senior-engine
/sw-core/sched/      @senior-redes
/sw-core/sources/    @senior-redes
/sw-core/include/    @arquiteto
/sw-pack/            @senior-engine
/sw-godot/           @senior-engine
/sw-netlab/          @pleno-qa
/sw-services/        @senior-redes
```

## Como trabalhar em paralelo sem bloquear

1. **Contrato primeiro.** Antes de implementar um módulo, o header/interface é revisado e congelado para a fase.
2. **Mocks para tudo.** Cada interface tem uma implementação falsa (fonte em memória, cache em memória, `sw-core` falso para a engine). Ninguém espera a parte do outro ficar pronta.
3. **Tarefas pequenas.** Nenhuma tarefa maior que ~5 dias; se for maior, o sênior quebra.
4. **Build da semana.** Toda semana, a rota completa roda na bancada com o que está em `main`, e o relatório vai para todos.
5. **PRs pequenos e revisados.** Revisão em até 1 dia útil.

## Rituais

| Ritual | Frequência | Duração |
| --- | --- | --- |
| Sincronização rápida | Diária ou 3×/semana (time remoto) | 15 min |
| Planejamento | A cada 2 semanas | 1 h |
| Revisão da build da semana (métricas) | Semanal | 30 min |
| Revisão de ADR | Quando houver ADR novo | 30 min |
| Retrospectiva + revisão da fase | Fim de cada fase | 1–2 h |

## Trilha de entrada por nível

| Nível | Primeira semana |
| --- | --- |
| Júnior | Ler [01](01-visao-e-escopo.md), [02](02-arquitetura.md), [glossário](glossario.md); compilar e rodar o mapa de teste; pegar uma tarefa marcada **boa-primeira-tarefa**. |
| Pleno | O anterior + ler os docs da trilha (03–06) e os ADRs; parear com o sênior da trilha numa tarefa média. |
| Todos | Ver quais skills do Claude usar no seu nível em [13-skills-claude.md](13-skills-claude.md). |
| Sênior | Tudo + revisar os contratos da sua trilha e propor ajustes por ADR; assumir uma tarefa crítica da fase. |

## Contribuidores externos (núcleo open source, ver ADR-0008)

Como o núcleo é open source ([ADR-0008](adr/0008-licenca-open-source.md)), as tarefas **boa-primeira-tarefa** e de bancada são ideais para contribuidores externos. Código crítico continua exigindo revisão de um dono do módulo.
