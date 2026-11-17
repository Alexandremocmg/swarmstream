# Passagem da coordenação (Planejador)

Documento para a sessão que coordena o projeto (a "gestão"). Quem assume lê este arquivo, o [README.md](README.md), o [QUADRO.md](QUADRO.md) e o [decisoes.md](decisoes.md). Atualize este arquivo quando o estado mudar de forma que a próxima passagem precise saber.

Última atualização: 2026-11-16.

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
| Papel | Sessão | Situação em 2026-11-16 |
| --- | --- | --- |
| Coordenação (gestão) | `local_39716e51-ecfc-4e78-9383-22b567fe3a07` ("Planejador") | Assumiu em 2026-11-10. A anterior, `local_2e2d3843-77d4-4cf1-8782-427f70498d9c` ("Planejador novo"), foi avisada para não editar `tarefas/`, fazer merge, push nem mandar mensagens às sessões de trabalho |
| R1, F1-69 | `local_b0eb953b-04a5-47fd-a347-1b13c6e12542` ("F1-69 posse e piso de cache") | #37 mesclado; ociosa. Pela regra de economia, não reabra trabalho nela |
| Implementadora F1-73 (teste de capacidade) | `local_033169a3-45c7-456a-a655-b63a5772377f` ("Fable_5.2") | #51 mesclado; ociosa |
| Implementadora F1-74 | sessão nova do cartão (id no cartão e no #53) | #53 mesclado; ociosa |
| Revisões independentes | `local_cd64fdea-4cdd-4c60-bd1f-583969008786` (#37), `local_ae9ec816-c8bf-471f-95d6-e524a59349e7` (#51) e a sessão revisora do #53 | Terminaram (pareceres publicados) |
| Arquiteto, Pleno e Sênior | `local_730e5b24…`, `local_9c4d9116…`, `local_1718d5d4…` | Sem trabalho desde 2026-10-24 (Sênior: 2026-10-28); receberam o id do coordenador novo. F1-20, F1-33 e F1-47 voltaram a `ABERTA` |

Pela regra de economia, cartão novo vai para **sessão nova**.

## Estado em 2026-11-16
- **Main:** `95194b3` (merge do #53, F1-74), sobre `7112e48`. O `ci`, o `lint` e o `extension` da `main` em `95194b3` passaram. PRs que só tocam `docs/` ou `tarefas/` não disparam CI.
- **F1-69 (#37), F1-73 (#51) e F1-74 (#53) FEITOS.** C2 encerrada. O helper `replace_file` não aloca depois do `rename`; `CacheErrorCode::OutOfMemory`=11 e `Internal`=12 existem; o piso trata `OutOfMemory` como "nada trocado".
- **Lint:** `clang-format` fixo em 18.1.8 (pacote do PyPI, #49). O `clang-tidy` continua pelo `apt`, sem versão fixa.
- **ADR-0018:** Aceita. Ordem **C2 → C1**; a C2 acabou, falta a C1 (F1-71, F1-72, F1-67).
- **F1-57 (teste instável do TSan)** falhou duas vezes na tarde de 2026-11-12 e uma em 2026-11-02; ver o registro do cartão.
- **Push direto na `main`:** a decisão de 2026-10-18 permite à gestão em `tarefas/`, mas a checagem de permissões da sessão negou o push em 2026-11-10. Enquanto o dono não liberar, mudanças em `tarefas/` vão por PR.

## Pendente com o dono (2026-11-16)
1. **Achado do FIFO do #37** (`installed_floor.cpp:95`, `open` sem `O_NONBLOCK`; severidade baixa): o F1-74 saiu sem tratá-lo. Vai para o F1-71/F1-72, para cartão próprio ou fica como limitação?
2. **Configuração MSVC Release para as medições do Portão 1**, com conferência de leitura do baseline e da máquina, sem rodar benchmark.
3. **Ressalvas do #51** (perguntas A, B e C do cartão F1-73): `to_model()`; falta de memória em `ed25519_verify` virando `SignatureInvalid` (sugestão: condição do F1-67 e do F1-72); run extra para os dois mutantes só do MSVC.
4. **Autorização do próximo cartão de código** (F1-71, depois F1-72; F1-67 e F1-68): sessão nova e palavra do dono para cada um.
5. Prioridade do F1-57 e as pendências antigas de [decisoes.md](decisoes.md). Baixa gravidade, sem dono: `to.parent_path()` vazio no helper.

## Próximos passos
1. PR do comentário de `sw_shutdown` no `swarmstream.h`, conforme o texto aceito do ADR-0018, com revisão do R1 e do R2.
2. F1-71 e depois F1-72 (o F1-67 numa sessão nova), cada um só com início autorizado pelo dono.

## Como conferir (armadilhas)
- Git Bash: use `MSYS_NO_PATHCONV=1` em `git show origin/main:caminho`.
- Para esperar o CI sem gastar turnos: um único `gh pr checks N --watch` em segundo plano. Não peça às sessões que consultem o CI em loop.
- Edite cartões com script Python (UTF-8, `newline="\n"`) e rode `python tarefas/gerar_quadro.py` e depois `--check`.
- Commits com `-s` e a linha `Co-Authored-By`.
