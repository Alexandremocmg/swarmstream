# Decisões e permissões do dono do produto

Registro mantido pela gestão. Cada linha é uma decisão tomada pelo dono do produto, com a data. Para o histórico de ADRs, veja [docs/adr/](../docs/adr/README.md).

## Permissões permanentes
| Data | Permissão | Escopo e limites |
| --- | --- | --- |
| 2026-10-18 | A gestão **commita direto na `main`, sem PR**, os arquivos de `tarefas/` (cartões, quadro, decisões) | **Somente a pasta `tarefas/`**, e somente arquivos de texto/documentação. Esta decisão e o `gerar_quadro.py` entraram por PR. Código, ADRs, specs, `CLAUDE.md` e qualquer outra documentação continuam por PR com aceite do dono |
| 2026-10-18 | A gestão **envia mensagens às outras sessões do projeto** | Só às sessões de trabalho deste repositório (hoje: Arquiteto, Sênior e Pleno). A mensagem avisa e aponta para o cartão; não pede merge, push, convite, gasto nem nenhuma ação que o dono não tenha autorizado. Não se aplica a sessões de outros projetos |

Continuam valendo, sem exceção: nenhuma sessão aceita o próprio PR; só o dono aceita PRs; tornar o repositório público, convidar colaboradores e alterar `LICENSE`, `NOTICE`, `SECURITY.md` ou `CODEOWNERS` exigem o dono.

## Decisões do projeto
| Data | Decisão |
| --- | --- |
| 2026-10-17 | Nome do projeto: **Swarmstream**. Núcleo open source sob **Apache-2.0** (ADR-0008) |
| 2026-10-17 | Modelo de negócio: **formato B** (núcleo aberto, backend hospedado por nós e pago, launcher aberto ou gratuito, protocolos abertos). ADR-0009 |
| 2026-10-17 | ADRs 0001 a 0009 aceitos |
| 2026-10-17 | Primeiro cliente-piloto: **estúdio indie com jogo grande em Godot** |
| 2026-10-17 | Plano para **3 recursos**; backend hospedado adiado para depois do Portão 2; regra de revisão com 3 pessoas |
| 2026-10-17 | Repositório **privado** até o fechamento da Fase 0; recursos sem contas no GitHub (todos trabalham pela conta do dono) |
| 2026-10-17 | Reinício do Windows para a carga fria **dispensado** nesta fase |
| 2026-10-18 | ADRs 0010 a 0016 e congelamento do `swarmstream.h` v0 aprovados |
| 2026-10-18 | Definição do chunker `swarmstream-fastcdc-v0` e limites do baseline aprovados (com ajustes do R1) |
| 2026-10-18 | **Fase 0 encerrada; Fase 1a aberta.** Critério do Portão 1 fixado em [docs/16](../docs/16-briefing-fase-1a.md) e não se ajusta depois de ver o resultado |
| 2026-10-18 | Trabalho com sessões do Claude Code; comunicação por documentos nesta pasta |

## Pendências do dono do produto
| Pendência | Desde |
| --- | --- |
| Escolher a pessoa do teste de 1 hora no Windows (Windows Sandbox, quem não montou o ambiente) | 2026-10-18 |
| Decidir quando o repositório deixa de ser privado (recomendado: depois do Portão 1 e de consulta jurídica rápida) | 2026-10-17 |
| Plano de preços do backend hospedado (depois do Portão 2) | 2026-10-17 |
| Mover o Godot 4.7.2 para uma pasta fixa (hoje está em uma pasta temporária do Windows) | 2026-10-18 |
| Destino da alteração solta em `docs/medicoes/guia-1h.md` na pasta compartilhada: um pré-teste do guia feito por um agente Hermes em contêiner Docker (Ubuntu 22.04). Não é da sessão Pleno; autor não identificado pela gestão. A própria nota diz que **não vale** como resultado do critério. Sugestão: revisar e, se útil, entrar por PR; senão descartar | 2026-10-18 |
| Dizer se a sessão "Hermez Agent utilização" faz parte do projeto (hoje só Arquiteto, Sênior e Pleno recebem avisos da gestão) | 2026-10-18 |
| Incluir os cartões F1-37 a F1-40 no backlog de [docs/11](../docs/11-backlog-inicial.md) (exige PR) | 2026-10-18 |
