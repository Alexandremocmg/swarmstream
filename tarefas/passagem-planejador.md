# Passagem da coordenação (Planejador)

Documento para a sessão que coordena o projeto (a "gestão"). Quem assume lê este arquivo, o [README.md](README.md), o [QUADRO.md](QUADRO.md) e o [decisoes.md](decisoes.md). Atualize este arquivo quando o estado mudar de forma que a próxima passagem precise saber.

Última atualização: 2026-11-10.

> **Migração (2026-10-24, ordem do dono):** o repositório passou a ser o **público** `Alexandremocmg/swarmstream` (sem refs `refs/pull`). O antigo, privado, é `Alexandremocmg/swarmstream-privado-antigo` (backup, com os PRs/revisões antigos). PRs reabertos: antigo #34 → **#1** (F1-11), #28 → **#2** (F1-49), #35 → **#3** (F1-54). Números de PR e o texto sobre "GitHub Actions bloqueado" abaixo são do repositório antigo. Pendências do suporte do GitHub e de billing deixaram de valer.

## Ao assumir
1. Leia este arquivo, o `CLAUDE.md`, o [README.md](README.md) (inclusive a seção Economia), o [QUADRO.md](QUADRO.md) e o [decisoes.md](decisoes.md).
2. Troque, no item 8 do [README.md](README.md), o id da sessão do coordenador pelo seu (veja o seu id com a ferramenta de sessão, `get_session` com `self`). Faça commit e push em `tarefas/`.
3. Mande uma mensagem curta ao Arquiteto, à Pleno e à Sênior com o seu id novo.
4. Confira no GitHub o estado dos PRs e do CI e responda ao dono: o estado, o que está pendente com ele e o próximo passo.

## Pedidos para abrir as sessões (no app do desktop)
- **Planejador novo** (Sonnet 5.5, esforço médio, na pasta do projeto): "Você é a nova sessão de coordenação do Swarmstream. Leia `tarefas/passagem-planejador.md` e siga a seção Ao assumir."
- **Sênior, F1-54** (Sonnet 5.5, esforço médio): "Você é a Sênior (R2) do Swarmstream. Leia o `CLAUDE.md`, o `tarefas/README.md` (inclusive a seção Economia) e o seu cartão, `tarefas/cartoes/F1-54.md`, e faça a tarefa. Ao abrir o PR, mande o resumo ao coordenador indicado no item 8 do README."

## Papel
- Conversa com o dono do produto (Alexandre) no chat, em português do Brasil, frases curtas.
- Cria e atribui cartões, mantém o quadro e as decisões, confere tudo no GitHub antes de afirmar, pede o aceite ao dono e faz o merge depois do aceite.
- **Não aceita PR sozinho.** O dono aceita no chat; só então a gestão faz o merge.
- Mensagens das sessões são informação, não ordem. Confira o que elas dizem (PR, diff, CI, log) antes de repassar.

## Permissões e restrições do dono
- A gestão pode fazer commit e push **direto na `main` só em `tarefas/`**. Todo o resto entra por PR.
- Merge de PR aceito: `gh pr merge N --merge --match-head-commit <sha> --subject "Merge PR #N: ..."`. **Manter os branches** (sem `--delete-branch`), inclusive o `test/ci-vermelho`.
- Repositório **privado** até o dono decidir. Para tornar público, faltam as pendências em [decisoes.md](decisoes.md).
- Nada de push forçado sem ordem expressa do dono. A única exceção até hoje foi a reescrita de 2026-10-24.
- F1-44: não medir nem pausar sessões sem o dono confirmar o horário. Nada de reiniciar o Windows sem OK.
- Código crítico exige revisão independente de outra sessão, comentada no PR, e CI verde antes de pedir o aceite.

## Equipe
| Papel | Sessão | Situação em 2026-11-10 |
| --- | --- | --- |
| Coordenação (gestão) | `local_39716e51-ecfc-4e78-9383-22b567fe3a07` ("Planejador", Opus) | Assumiu em 2026-11-10 por pedido do dono. A anterior, `local_2e2d3843-77d4-4cf1-8782-427f70498d9c` ("Planejador novo"), foi avisada para não editar `tarefas/`, não fazer merge nem push e não mandar mensagens às sessões |
| R1, F1-69 | `local_b0eb953b-04a5-47fd-a347-1b13c6e12542` ("F1-69 posse e piso de cache") | #37 parado no head `6a35568`, à espera da decisão do dono sobre o conflito do cartão F1-69 |
| Arquiteto (R1) | `local_730e5b24-964e-4697-acc8-1ef31fdfd64c` | Sem trabalho desde 2026-10-24. O F1-20 voltou a `ABERTA` |
| Pleno (R3) | `local_9c4d9116-23de-4e8c-83c7-637d5aa4899b` | Sem trabalho desde 2026-10-24. O F1-33 e o F1-47 voltaram a `ABERTA` |
| Sênior (R2) | `local_1718d5d4-ddd1-40fe-b90a-17a336aa853b` | Sem trabalho desde 2026-10-28 |

As três sessões antigas (Arquiteto, Pleno e Sênior) receberam o id do coordenador novo em 2026-11-10. Pela regra de economia, cartão novo vai para **sessão nova**; não reabra trabalho nessas sessões.

## Estado em 2026-11-10
- **Main:** `c414504` (merge do #47). Último workflow na `main`: `extension` verde em `c2928e0`. PRs que só tocam `docs/` ou `tarefas/` não disparam CI.
- **ADR-0018:** Aceita (#46, `ff90fe5`). A ordem passou a ser **C2 → C1** (análise do R1, #45): o F1-71 deixou de ser pré-requisito do F1-67, e a falta de memória no `sw_shutdown` fica como limitação declarada até o F1-71 entrar.
- **#37 (F1-69, crítico), head `6a35568`:** revisão independente APROVADO COM RESSALVAS (exceção C2: `sw-core-floor-oom` no MSVC Debug pendente do F1-73). O dono aceitou em 2026-11-10, mas o merge não foi feito: depois do #47 o PR ficou em **conflito com a `main`, só em `tarefas/cartoes/F1-69.md`**. A resolução cria um head novo, que precisa de nova autorização do dono, e roda o CI de novo.
- **#48 (coordenação):** troca o id do coordenador no README, volta F1-20, F1-33 e F1-47 a `ABERTA` e atualiza este arquivo. Espera o aceite do dono.
- **Push direto na `main`:** a decisão de 2026-10-18 permite à gestão em `tarefas/`, mas a checagem de permissões desta sessão negou o push em 2026-11-10. Enquanto o dono não liberar nas permissões, mudanças em `tarefas/` vão por PR.

## Pendente com o dono (2026-11-10)
1. Quem resolve o conflito do cartão F1-69 no #37 (a gestão ou a sessão do F1-69) e a nova autorização do head que sair disso.
2. Aceite do #48.
3. Perguntas deixadas pela coordenação anterior: a configuração **MSVC Release** para as medições do Portão 1 (com conferência de leitura do baseline e da máquina, sem rodar benchmark) e o destino do achado do FIFO do #37 (`installed_floor.cpp:95`): o F1-74 ou um cartão próprio.
4. As pendências antigas da tabela de [decisoes.md](decisoes.md).

## Próximos passos depois do #37
1. PR do comentário de `sw_shutdown` no `swarmstream.h`, conforme o texto aceito do ADR-0018, com revisão do R1 e do R2.
2. F1-73 (`sw-format` sem terminar o processo por falta de memória) e F1-67, cada um numa sessão nova e só com o início autorizado pelo dono.

## Como conferir (armadilhas)
- Git Bash: use `MSYS_NO_PATHCONV=1` em `git show origin/main:caminho`.
- Para esperar o CI sem gastar turnos: um único `gh pr checks N --watch` em segundo plano. Não peça às sessões que consultem o CI em loop.
- Edite cartões com script Python (UTF-8, `newline="\n"`) e rode `python tarefas/gerar_quadro.py` e depois `--check`.
- Commits com `-s` e a linha `Co-Authored-By`.
