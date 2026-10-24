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
| 2026-10-19 | **Regra do Portão 1** (perfil `cabo_50`): taxa de falta **abaixo de 1% em cada rodada**, com **zero faltas em lógica e colisão**; espera **p99 abaixo de 300 ms calculada sobre os eventos individuais reunidos de pelo menos três rodadas completas e comparáveis** (não usar média dos p99). O relatório registra, por rodada, leituras, faltas e p99, além do resultado agregado e do número de amostras. **Ausência de faltas não vira p99 = 0.** O método do percentil e o mínimo de amostras são proposta técnica do R1 (cartão F1-48) antes das medições finais. Stutter e carga seguem os critérios já aprovados. **Métricas automáticas aprovadas não aprovam sozinhas o Portão 1:** é preciso a avaliação visual registrada de pop-in |
| 2026-10-19 | F1-33: o **esqueleto** foi aceito, sem ser a versão completa nem evidência de aprovação do Portão 1 |
| 2026-10-19 | Atribuições: **R1** (Arquiteto) prioriza o F1-20, depois o F1-10 (com revisão do R2, após a F1-46) e o F1-48; **R2** (Sênior) faz a F1-46 e depois o F1-01; **R3** (Pleno) faz o F1-47 e depois continua o F1-33 |
| 2026-10-19 | F1-01 parte **apenas da especificação**; a sessão não recebe os scripts Python de referência nem o conteúdo deles. Se for preciso, sessão nova sem esses scripts no contexto |
| 2026-10-19 | F1-44: preparar o procedimento (sessões a pausar, onde gravar os resultados). **Não medir nem pausar sessões** antes de o dono confirmar o horário |
| 2026-10-19 | **Manter todos os branches**, inclusive `test/ci-vermelho` |
| 2026-10-19 | F1-01: independência confirmada pela sessão Sênior e conferida pela gestão no histórico das sessões; o F1-01 segue com a Sênior, depois da F1-46, com checkout esparso sem os scripts Python |
| 2026-10-19 | **F1-27** (fonte HTTP) fica com a **Sênior (R2), depois do F1-01** |
| 2026-10-19 | PR #20 aceito: regra do Portão 1 registrada nos docs 16 e 08 |
| 2026-10-20 | **F1-48 aceito (PR #29). Opção A:** o critério do Portão 1 é o p99 pontual (posto mais próximo) < 300 ms com **N >= 300 faltas concluídas por perfil**; a cota superior de 95% é **só informativa**. Rodadas comparáveis com os campos exatos de ambiente do doc 08; K declarado antes de medir (3 a 10, com uma extensão); sessões diferentes não se reúnem. Ruído da validação da bancada mantido em 10% |
| 2026-10-20 | **Rodada de faltas forçadas aceita**: se, com K = 10, o perfil não chegar a 300 faltas, faz-se uma rodada com cache frio e prefetch desligado, só para medir a espera; ela não entra na taxa de falta |
| 2026-10-20 | PRs #25 (F1-01), #26 (portão do F1-20), #27 (F1-51) e #29 (F1-48) aceitos |
| 2026-10-22 | PRs #30 (decisões do F1-48 no doc 08) e #32 (F1-27, fonte HTTP Range) aceitos |
| 2026-10-22 | Atribuições: **F1-12 e depois F1-11 ao Arquiteto**, com revisão independente da Sênior; **F1-53 à Pleno**, depois do PR #28 |
| 2026-10-24 | PR #31 (F1-10, crítico, revisado pela Sênior) aceito; Arquiteto liberado para o F1-12 |
| 2026-10-24 | PR #33 (F1-12, crítico, revisado pela Sênior) aceito, com o job **TSan** novo no CI (o CI passa a 14 checks); Arquiteto liberado para o F1-11 |
| 2026-10-24 | **Preparação para tornar o repositório público** (a cota grátis do GitHub Actions para repositório privado acabou). Por ordem do dono, o histórico foi **reescrito** para remover de todos os commits o `Exemplo_Engine_Navegador.md` (estudo de um porte de engine de terceiros), com **push forçado autorizado pelo dono**. Todos os SHAs mudaram (a main passou de `51cca9e` para `388da32`); o conteúdo, os autores e as mensagens são os mesmos. O backup e o mapa de commits ficam só com o dono, fora do repositório. SHAs citados antes desta data nos cartões e docs referem-se ao histórico antigo |
| 2026-10-24 | **Economia de Claude Code e de CI:** uma sessão nova por cartão; modelo por tipo de trabalho; uma mensagem por etapa; conectores de nuvem sem uso desligados nas sessões; coordenação passa para um Planejador novo, em Sonnet. Cartão **F1-54** (reduzir o custo do CI) atribuído à Sênior, numa sessão nova. Regras na seção Economia de [README.md](README.md) |
| 2026-10-24 | **Repositório público novo** `Alexandremocmg/swarmstream` (só `main` e branches, sem `refs/pull`) para ter Actions sem limite; o antigo foi renomeado `swarmstream-privado-antigo` e segue privado. PRs abertos reabertos como #1 (F1-11), #2 (F1-49) e #3 (F1-54). Números de PR citados antes desta data referem-se ao repositório antigo |
| 2026-10-19 | Ao concluir cada tarefa, a sessão envia ao **coordenador** (Planejador (sessão `local_2e2d3843-77d4-4cf1-8782-427f70498d9c`)) o resumo, o PR, os testes executados e as pendências; o quadro é atualizado com base nessa evidência |

## Pendências do dono do produto
| Pendência | Desde |
| --- | --- |
| Escolher a pessoa do teste de 1 hora no Windows (Windows Sandbox, quem não montou o ambiente) | 2026-10-18 |
| Tornar o repositório público. Antes: (1) **pedido ao suporte do GitHub** para apagar as referências dos PRs fechados e as visualizações em cache dos commits antigos (32 refs `refs/pull/*` ainda contêm o arquivo removido); (2) conferir em Billing que não há pagamento falho (se houver, o Actions pode seguir bloqueado mesmo em repositório público); (3) PR que tira de `docs/README.md` a menção ao arquivo removido. Depois: ligar o relato privado de vulnerabilidades (o `SECURITY.md` depende dele), o secret scanning e a proteção da main | 2026-10-24 |
| Plano de preços do backend hospedado (depois do Portão 2) | 2026-10-17 |
| Mover o Godot 4.7.2 para uma pasta fixa (hoje está em uma pasta temporária do Windows) | 2026-10-18 |
| Destino da alteração solta em `docs/medicoes/guia-1h.md` na pasta compartilhada: um pré-teste do guia feito por um agente Hermes em contêiner Docker (Ubuntu 22.04). Não é da sessão Pleno; autor não identificado pela gestão. A própria nota diz que **não vale** como resultado do critério. Sugestão: revisar e, se útil, entrar por PR; senão descartar. **Em 2026-10-22, guardada no branch local `local/pre-teste-hermes` (só na pasta compartilhada, não enviado ao GitHub)** | 2026-10-18 |
| Dizer se a sessão "Hermez Agent utilização" faz parte do projeto (hoje só Arquiteto, Sênior e Pleno recebem avisos da gestão) | 2026-10-18 |
| Incluir os cartões F1-37 a F1-47 no backlog de [docs/11](../docs/11-backlog-inicial.md) (exige PR) | 2026-10-18 |
| Apagar os branches já incorporados no GitHub, se quiser (ex.: `baseline/limites-aprovados-r1` e `feat/F0-08-vetores-de-teste`; o `test/ci-vermelho` convém manter, é a evidência do CI vermelho). Ninguém apagou sem a sua ordem | 2026-10-18 |
