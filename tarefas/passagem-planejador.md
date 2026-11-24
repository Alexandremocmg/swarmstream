# Passagem da coordenação (Planejador)

Documento para a sessão que coordena o projeto (a "gestão"). Quem assume lê este arquivo, o [README.md](README.md), o [QUADRO.md](QUADRO.md) e o [decisoes.md](decisoes.md). Atualize este arquivo quando o estado mudar de forma que a próxima passagem precise saber.

Última atualização: 2026-11-21.

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
| Papel | Sessão | Situação em 2026-11-21 |
| --- | --- | --- |
| Coordenação (gestão) | `local_39716e51-ecfc-4e78-9383-22b567fe3a07` ("Planejador") | Assumiu em 2026-11-10. A anterior, `local_2e2d3843-77d4-4cf1-8782-427f70498d9c` ("Planejador novo"), foi avisada para não editar `tarefas/`, fazer merge, push nem mandar mensagens às sessões de trabalho |
| R1, F1-69 | `local_b0eb953b-04a5-47fd-a347-1b13c6e12542` ("F1-69 posse e piso de cache") | #37 mesclado; ociosa. Pela regra de economia, não reabra trabalho nela |
| Implementadora F1-73 (teste de capacidade) | `local_033169a3-45c7-456a-a655-b63a5772377f` ("Fable_5.2") | #51 mesclado; ociosa |
| Implementadora F1-74 | `local_8382f78b-7137-4576-bc91-d1d50b34e8f8` | #53 mesclado; ociosa |
| Implementadora F1-71 | `local_d93addac-d1e5-4191-a24a-e8c263f5672f` | #55 mesclado; ociosa |
| Implementadora F1-72 (Opus) | `local_98902cf6-9638-489d-bdfd-f58dc3f6958f` | #57 mesclado; ociosa. Uma sessão Sonnet, `local_8cc323af-983a-402a-a7d8-559d67e9d5c8`, foi iniciada antes e **parada** sem nada commitado (ignore-a) |
| Revisões independentes | `local_cd64fdea-4cdd-4c60-bd1f-583969008786` (#37), `local_ae9ec816-c8bf-471f-95d6-e524a59349e7` (#51) `local_1231925f-ba4d-4905-8fe5-076315d74a39` (#53) `local_d0a402ce-3a0b-4a77-910c-9f446d4a9cb5` (#55) e `local_afaa9b71-7bcf-4730-9d53-0c034c2b5f9a` (#57) | Terminaram (pareceres publicados) |
| Arquiteto, Pleno e Sênior | `local_730e5b24…`, `local_9c4d9116…`, `local_1718d5d4…` | Sem trabalho desde 2026-10-24 (Sênior: 2026-10-28); receberam o id do coordenador novo. F1-20, F1-33 e F1-47 voltaram a `ABERTA` |

Pela regra de economia, cartão novo vai para **sessão nova**.

## Estado em 2026-11-21
- **Main:** `b59fb1c` (merge do #57, F1-72), sobre `880ab74`. O `ci` e o `lint` da `main` em `b59fb1c` passaram; o `extension` ficou **vermelho só no job Windows** (de novo, mesma causa do `0be4c4d`: configuração do `zstd` no CMake do GDExtension, imagem nova do runner; determinístico). PRs que só tocam `docs/` ou `tarefas/` não disparam CI.
- **F1-69 (#37), F1-73 (#51), F1-74 (#53), F1-71 (#55) e F1-72 (#57) FEITOS.** `start()`/`recover()` e `stop()` não terminam o processo por falta de memória ou exceção; o índice em memória do cache é `std::map` (no MSVC com `_ITERATOR_DEBUG_LEVEL != 0`, qualquer `unordered_*` pode terminar o processo sem memória). A C2 está encerrada; falta a C1 completa (F1-67 e F1-76).
- **2026-11-24 (depois do #62):** `main` em `f3e9796` (merge do #62). `ci` e `lint` da `main` passaram; o `extension` falhou só no job Windows (pendência conhecida). **F1-67: parte B FEITA** (#62), **parte A com a sessão Opus 3** `local_9e723089-16f2-4cbf-bf23-5e3010d3b0f4` (briefing enviado; sem PR), partes C e D bloqueadas (C depende só da A). Revisora do #62: `local_86fe0f17-5a25-46b6-855f-b5e18d99d4e8` (terminou); autora: Opus2 `local_b92625b1-bb1f-47df-8ad5-f69c0f408a91`.
- **2026-11-24:** `main` em `2f7e7d6` (merge do #59 sobre o #61, `7037b27`). **F1-67 em andamento em quatro partes:** parte B = PR #62 (Opus2 `local_b92625b1-bb1f-47df-8ad5-f69c0f408a91`; crítico; revisão independente `local_86fe0f17-5a25-46b6-855f-b5e18d99d4e8` publicou APROVO COM RESSALVAS sobre os heads `1e9ca77` e `0905caaa`; lacuna de teste do ramo cíclico de `table_erase_locked` e M32 probabilístico aguardam decisão do dono; aceite do dono pendente); parte A (`close()` da telemetria) sem sessão; C e D bloqueadas. Revisoras do #59: `local_8ef3d8af-e641-449a-982c-b3a9ed0d034c` (R1) e `local_613efe62-f8c7-43bb-9176-6c20b2322091` (R2), terminadas.
- **Lint:** `clang-format` fixo em 18.1.8 (pacote do PyPI, #49). O `swarmstream.h` está excluído do passo `C++ format` (#61). O `clang-tidy` continua pelo `apt`, sem versão fixa.
- **ADR-0018:** Aceita. Ordem **C2 → C1**.
- **CI:** o `ci.yml` exige os resumos `[F1-71]` e `[F1-72]` (pontos de falha por cenário) nos 5 passos de resumo; Windows Debug: `[F1-71]` 21/31/31/31, `[F1-72]` 72/119/109/158. O `sw-core-start-oom` leva 79 s no Windows Debug do CI.
- **F1-57 (teste instável do TSan)** falhou duas vezes na tarde de 2026-11-12 e uma em 2026-11-02; ver o registro do cartão.
- **Push direto na `main`:** a decisão de 2026-10-18 permite à gestão em `tarefas/`, mas a checagem de permissões da sessão negou o push em 2026-11-10. Enquanto o dono não liberar, mudanças em `tarefas/` vão por PR.

## Pendente com o dono (2026-11-21)
1. **Achado do FIFO do #37** (`installed_floor.cpp:95`, `open` sem `O_NONBLOCK`; severidade baixa): cartão próprio ou limitação? (F1-74, F1-71 e F1-72 saíram sem tratá-lo.)
2. **Configuração MSVC Release para as medições do Portão 1**, com conferência de leitura do baseline e da máquina, sem rodar benchmark.
3. **Ressalvas do #51** (perguntas A, B e C do cartão F1-73): `to_model()`; falta de memória em `ed25519_verify` virando `SignatureInvalid`; run extra para os dois mutantes só do MSVC.
4. **Ressalvas do #55:** R1' (valor real de `File::close()` sem teste), N3 só detectável no MSVC Debug e não rodado, R4 (`stop()` da thread dona; condição do F1-67).
5. **Ressalvas do #57:** R1 a R4 (lacunas de teste) e **R5: `put()` por falta de memória termina o processo no MSVC Debug, sem cartão dono**; a recuperação copia os checksums do `index.bin` (pico de memória, não medido).
6. **`extension` da `main` vermelho no job Windows** (determinístico; runs `44768643933` e `44896614283`): abrir cartão para corrigir o ASM do `zstd` ou fixar a imagem.
7. **Autorização do próximo cartão de código:** **F1-67** e **F1-68** (sessão nova e palavra do dono para cada um); depois o F1-76.
8. Prioridade do F1-57 e as pendências antigas de [decisoes.md](decisoes.md).
9. **Achados relatados pela Opus2 em 2026-11-24, não reproduzidos pela coordenação:** `sw-core-start-oom` (F1-72) instável no Windows local com MinGW Release ("THREAD VIVA": conta todas as threads do processo, inclusive as do pool do sistema; falhou em 2 de 4 rodadas na `main` limpa, e em mais 1 de 3); `sw-core-scheduler` falhou uma vez no Debug local (`test_deadline_scheduler.cpp:962`, espera de 5 s por tempo; 5 de 5 passes depois). Sem cartão dono; no CI passaram.
10. **Polimento do comentário de `sw_shutdown`** (R2-2 a R2-6 do #59) e a nota desatualizada do ADR-0018 ("ainda não feito"): exigem ADR e R1 + R2; sem autorização.
11. ~~Ressalvas do parecer do #62~~ **resolvidas em 2026-11-24**: os achados 1 a 4 entraram no próprio #62 por decisão do dono; o achado 5 (regra do `dst` no cancelamento) é condição do F1-68 ([cartão](cartoes/F1-68.md)).

## Próximos passos
1. ~~PR do comentário de `sw_shutdown` no `swarmstream.h`~~ **FEITO em 2026-11-24** (#59, mesclado em `2f7e7d6`, com R1 e R2; #61 tirou o header do clang-format). As ressalvas viraram condições da parte C do F1-67 ([registro](cartoes/F1-67.md)).
2. F1-67 e F1-68 em sessões novas, cada um só com início autorizado pelo dono; o F1-67 deve tratar o `stop()` da thread dona (R4 do #55) e o `unordered_*` no MSVC Debug.

## Como conferir (armadilhas)
- Git Bash: use `MSYS_NO_PATHCONV=1` em `git show origin/main:caminho`.
- Para esperar o CI sem gastar turnos: um único `gh pr checks N --watch` em segundo plano. Não peça às sessões que consultem o CI em loop.
- Edite cartões com script Python (UTF-8, `newline="\n"`) e rode `python tarefas/gerar_quadro.py` e depois `--check`.
- Commits com `-s` e a linha `Co-Authored-By`.
