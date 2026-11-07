# Análise do R1: o que o caminho mínimo do F1-67 usa de F1-69 a F1-74 (opção B)

**Status: ANÁLISE SÓ DE LEITURA, entregue ao dono em 2026-11-07. Não é decisão.** Responde ao [pedido](pedido-r1-analise-caminho-minimo.md) autorizado pelo dono em 2026-11-06 (opção B da [proposta do Portão 1](portao-1-caminho-minimo.md)). Não muda cartão, ordem, critério nem dependência. O que a seção 5 sugere é **proposta**, e só o dono decide.

**Autor:** sessão do R1 (Claude Opus 5.5), que não implementou nenhum dos cartões analisados.
**Base:** `main` em `a0178fc`, o branch do #37 (F1-69) no head `6a3556871dda932eeaeaddfbfbcd1b1dff2679b7` e o run `42044485012`.
**Regras seguidas:** nada foi compilado nem executado, e a varredura de falta de memória não rodou. O #37 só foi lido, sem alteração. Não há previsão de prazo.

**Legenda da evidência:**
- **[Lido]**: li o código ou o texto no arquivo e na linha citados.
- **[CI]**: li o resultado no GitHub (`gh pr view 37`, `gh run view 42044485012`).
- **[Suposição]**: é inferência minha e não foi verificada.

## 0. Resposta curta
1. **Para ler, medir e ser medido, o F1-67 não precisa de F1-71, F1-72, F1-73 nem F1-74.** Nenhum dos quatro muda o resultado de uma leitura num processo com memória disponível. No jogo em execução, as falhas de memória do cache já são capturadas e quebram o cache sem terminar o processo (`cache_owner.cpp:540-552` e `:635-647`) [Lido].
2. **Quem precisa deles são os critérios de aceite do próprio F1-67**, e não a função:
   - a varredura de falta de memória no `sw_init` e no `sw_shutdown`, sem excluir pontos e incluindo o MSVC Debug;
   - o código `SW_E_OUT_OF_MEMORY`, que não pode virar `SW_E_IO`;
   - o resultado do `sw_shutdown` pela opção A do ADR-0018.

   Sem os quatro cartões, esses critérios **não podem passar como estão escritos** (seção 2). Adiar os cartões exige, portanto, mudar ou dividir critérios do F1-67, e isso é decisão do dono.
3. **O F1-69 (lock e piso) está em outra categoria.** O `sw_init` precisa dele para cumprir o header, que lista `SW_E_CACHE_LOCKED` e `SW_E_MANIFEST_ROLLBACK`. Uma leitura medida num processo só não usa nenhum dos dois. O código de produto do #37 passa em 6 de 7 checks. O único teste que falha é `sw-core-floor-oom`, no passo Debug do Windows. O mesmo job passou nos passos `[Release]`, inclusive `[Release] Test` [CI]. Pelo diagnóstico do cartão F1-73, a falha vem do `sw-format`, não do piso [Sessão do F1-69, conferido em parte pelo Planejador].
4. **Três dos quatro defeitos de memória só existem no MSVC Debug.** São os seis sítios do F1-73 e os construtores padrão citados no F1-74 e no F1-72. Os outros dois valem em qualquer configuração:
   - `CacheOwner::stop()` aloca dentro de função `noexcept` (F1-71);
   - `replace_file` aloca no POSIX depois do `rename` (F1-74).

   Os dois só disparam com falta de memória real ou injetada.

## 1. O que o caminho mínimo do F1-67 chama
É a ordem C1 a C8 do [F1-60 §1.1](../../tarefas/cartoes/F1-60.md). Para uma leitura funcional e medida (F1-67 mais F1-68), estes são os pontos que tocam os cartões analisados:

| Passo | O que chama | Arquivo e linha | Cartão envolvido |
|---|---|---|---|
| C2 | `CacheDirLock::acquire` | `cache_dir_lock.hpp:99` (#37) [Lido] | F1-69 |
| C3 | `ManifestView::open` do manifesto novo e `read_installed_floor`, que também chama `ManifestView::open` | `manifest_view.cpp:423,460,727,879`, `message_parts.hpp:26`; `installed_floor.hpp:161` (#37) [Lido] | F1-73 (sítios), F1-69 |
| C5 | `CacheOwner::start` → `StoreFiles::open` (fora do `try`), `recover()` → `checkpoint()` | `cache_owner.cpp:877`, `:880-898`, `:410` [Lido] | F1-72, F1-74 (via `checkpoint`), F1-70 (feito) |
| C8 | `commit_installed_floor` → `replace_file` (no #37 por um contorno local) | `installed_floor.hpp:184` (#37); `platform_file.hpp:297-307` [Lido] | F1-69, F1-74 |
| desfazer de falha depois de C5, e S4 no `sw_shutdown` | `~CacheOwner` → `stop()` → `checkpoint()` → `write_index_file`/`create_fresh` → `replace_file` | `cache_owner.cpp:910`, `:938-965`, `:416-452`; `cache_journal.cpp:138,340` [Lido] | F1-71, F1-74 |
| S6 | `~TelemetryLog` (resumo, `fflush`, `fclose`, sem devolver o resultado) | `telemetry.cpp:308-328` [Lido] | F1-71 (item 4) |

Na entrada de cada função `sw_*` há uma fronteira de exceções: `bad_alloc` vira `SW_E_OUT_OF_MEMORY` e qualquer outra exceção vira `SW_E_INTERNAL` ([F1-60 §1.2](../../tarefas/cartoes/F1-60.md)). Por isso uma exceção que **sobe** de `start()` não atravessa a API C. O que a fronteira **não** impede é um `std::terminate` interno (F1-60 §5, ADR-0018) [Lido].

## 2. Tabela por cartão
| Cartão | O que o F1-67 chama | Fronteira ou contrato | O que quebra no F1-67 se faltar | O que é só garantia de falta de memória | Retrabalho previsível se vier depois |
|---|---|---|---|---|---|
| **F1-69** (lock e piso, #37) | `CacheDirLock::acquire` em C2; `read_installed_floor` em C3; `commit_installed_floor` em C8 | Header: `SW_E_CACHE_LOCKED`, `SW_E_MANIFEST_ROLLBACK`; ADR-0011; F1-60 §6.1 e §6.2 | **Funcional, contra o header:** sem ele o `sw_init` não devolve `CACHE_LOCKED` nem `MANIFEST_ROLLBACK`, e os critérios "um teste por código" e "contexto existente intacto" (2º `sw_init` na mesma pasta) não passam. **Uma leitura medida num processo só não usa o lock nem o piso.** | O `sw-core-floor-oom` no MSVC Debug é a parte que falha no #37 [CI] | Se o `sw_init` vier antes, C2, C3 e C8 entram depois. Como são passos novos na pilha de construção, refazem-se a varredura do `sw_init` e os testes de falha de cada etapa [Suposição: custo pequeno no código, mas com nova revisão crítica do `sw_init`] |
| **F1-70** (`block_count`) | Vetor esperado em `CacheOwnerConfig` (C5) | docs/04 §5 passo 3 | Nada: **FEITA** (`b5f94fd`) | — | — |
| **F1-73** (`sw-format` no MSVC Debug) | `ManifestView::open` em C3, duas vezes: o manifesto novo e a cópia do piso | Construtores padrão `noexcept` da STL que alocam com `_ITERATOR_DEBUG_LEVEL != 0` (padrão do F1-63) | **Nada funcional.** Quebra o critério "OOM em `sw_init`... sem `terminate` (**incluindo MSVC Debug**)" e "varredura de `operator new` em cada etapa do `sw_init`, **sem excluir pontos**". A varredura entra no `ManifestReader::open` e acha os mesmos sítios [Suposição apoiada no diagnóstico do #37: 108 pontos com `terminate`, todos no `sw-format`] | Tudo | Sem mudança de API (o F1-73 preserva as assinaturas). Rodar de novo a varredura do `sw_init` no MSVC Debug e tirar a exclusão, se o dono tiver aceitado uma |
| **F1-74** (`replace_file` e `CacheErrorCode::OutOfMemory`/`Internal`) | `replace_file` em C8 (piso) e dentro de `checkpoint()` (C5, desfazer, S4). O enum serve ao mapeamento dos códigos do cache para `SW_E_*` | `platform_file.hpp:306-307`: no POSIX, `to.parent_path()` aloca dentro de função `noexcept` **depois** do `rename`. No Windows (`:301`) não há alocação no helper [Lido] | **Nada funcional.** No Linux (ASan/TSan/Ubuntu) a varredura do `sw_shutdown` passa por `checkpoint` → `replace_file` e pode terminar o processo **em qualquer configuração** [Suposição: depende de o ponto *k* cair na alocação do `parent_path`]. Sem o enum, o mapeamento `OutOfMemory → SW_E_OUT_OF_MEMORY` do ADR-0018 não tem de onde vir | Tudo, mais a distinção memória × disco (alternativa B do dono) | O mapeamento do `sw_init`/`sw_shutdown` é escrito sem `OutOfMemory`/`Internal` e ganha dois casos depois. O contorno local do piso no #37 (`RealFloorFileOps::replace`) já existe e continua até o F1-74 |
| **F1-71** (`stop()` e auditoria da parada) | `stop()` em S4 e no desfazer de falha depois de C5; `close()` da telemetria em S6 | `cache_owner.cpp:938-965`: `stop()` `noexcept` chama `checkpoint()`, que aloca (`:419-434`). Um `bad_alloc` ali é `std::terminate` **em qualquer compilador** [Lido]. ADR-0018 requisitos 1 e 2; decisão do dono sobre `close()` explícito | **Nada funcional**: com memória, `stop()` faz checkpoint e fecha journal e store. Quebra a varredura do `sw_shutdown` (opção A: "nenhum `terminate`", resultado em {OK, IO, DISK_FULL, OOM, INTERNAL}) e a regra de precedência da telemetria (sem `close()`, a falha do resumo não chega ao retorno) | Tudo, menos o `close()` da telemetria, que é **contrato de resultado** (decisão do dono de 2026-11-02) e não só memória | S4 já chama `stop()` e mapeia o código; o retrabalho é acrescentar os casos novos. Em S6, trocar a destruição da telemetria por `close()` e incorporar o resultado. Os testes do `sw_shutdown` são refeitos com o `stop()` novo, com nova revisão crítica [Suposição] |
| **F1-72** (`start()` e `recover()`) | `CacheOwner::start` em C5 | `cache_owner.cpp:877`: `StoreFiles::open` fora do `try`; `:896-898`: `bad_alloc` vira `IoError`; `:902`: só `system_error` no `std::thread`. Outras exceções escapam [Lido] | **Nada funcional.** Quebra "OOM em cada etapa do `sw_init` → `SW_E_OUT_OF_MEMORY`": sem o F1-72, falta de memória no C5 sai como `SW_E_IO`. Uma exceção que escapa de `start()` é capturada na fronteira do `sw_*` e vira `SW_E_INTERNAL`, sem vazar à API (§1) [Lido]. No MSVC Debug pode haver `terminate` interno ainda não medido [Suposição: o F1-74 lista os riscos de `create_fresh` e `tmp_of`, chamados por `recover()`] | Tudo | O `sw_init` mapeia o código de `start()`. Quando o F1-72 chegar, o caso C5 da varredura passa de `SW_E_IO` aceito como limitação para `SW_E_OUT_OF_MEMORY`. É só teste e mapeamento, sem mudança de estrutura [Suposição] |

## 3. Contratos que cada opção de adiamento afetaria
| Contrato | A (ordem atual) | C (F1-67 antes de F1-71 a F1-74, com F1-69 incluído) | C' (F1-67 também antes do F1-69) |
|---|---|---|---|
| `swarmstream.h` v0 | não muda | não muda | não muda (os códigos `CACHE_LOCKED`/`MANIFEST_ROLLBACK` ficariam **sem implementação**, o que contraria o comentário do `sw_init`) |
| ADR-0017 | não muda | não muda | não muda |
| ADR-0018 (Proposta) | precisa do aceite antes da regra do `sw_shutdown` | **igual**: o F1-71 não está no caminho do aceite. Sem o F1-71, a opção A só se cumpre com memória disponível | igual |
| `CacheErrorCode` | o F1-74 cria `OutOfMemory`/`Internal` antes | mapeamento escrito sem eles e completado depois | igual a C |
| [F1-60 §1.1](../../tarefas/cartoes/F1-60.md) (ordem C1 a C8 e S0 a S8) | intacta | intacta | **muda**: C2, C3 (piso) e C8 inseridos depois |
| [F1-60 §1.2](../../tarefas/cartoes/F1-60.md) (fronteira de exceções) | intacta | intacta; é ela que segura as exceções que escapam de `start()` sem o F1-72 | intacta |
| [F1-60 §5](../../tarefas/cartoes/F1-60.md) / ADR-0018, requisito "nenhum `terminate` na parada" | cumprido pelo F1-71 | **não cumprido** até o F1-71. Exige limitação declarada pelo dono | idem |
| Critérios do F1-67: "OOM em cada etapa do `sw_init`, sem excluir pontos, incluindo MSVC Debug" e "varredura do `sw_shutdown` inteiro" | cumpridos | **precisam ser divididos**: entram depois ou ficam como limitação declarada. Isso contraria a regra do F1-63 ("sem excluir pontos") | idem, mais os testes de lock e piso |
| Regra do dono de 2026-11-02 (F1-71 e F1-72 pré-requisitos do F1-67) | mantida | **revista pelo dono** | revista |

## 4. O que não foi possível concluir por leitura
1. **Configuração de build das rodadas do Portão 1.** O [docs/08](../08-metricas-e-testes.md) e o [baseline](../medicoes/baseline.md) não a fixam. O portão do F1-20 usou MinGW-w64 GCC em Debug ([f1-20-portao.md:19](../medicoes/f1-20-portao.md)). Se a medição usar GCC ou MSVC Release, os sítios do F1-73 não existem no binário medido. Se usar MSVC Debug, existem, mas só disparam com falta de memória [Suposição sobre o mecanismo, apoiada no [CI]: Windows Release e Linux verdes no #37].
2. **Se a varredura do `sw_shutdown` atinge de fato o `parent_path` do `replace_file`** no Linux. Depende de o injetor falhar só a k-ésima alocação ou daí em diante, e da ordem das alocações no `checkpoint`. Não executei.
3. **Fronteiras `noexcept` do MSVC Debug em `start()`/`recover()`** (`create_fresh`, `tmp_of`, `File`, `remove_file`). Estão listadas no F1-74 como risco, sem medição. Não medi.
4. **As 108 pilhas do #37**, uma a uma. Li o resumo do cartão F1-73 e o resultado do job. Não reli o log por pilha.
5. **O custo de cada opção.** Sem prazo, como o pedido exige. Do diff do #37 tirei só este dado: cerca de 1,7 mil linhas de produto e cerca de 4,9 mil de teste, das quais cerca de 2,7 mil nos arquivos `*_oom*`, `oom_support.hpp` e `alloc_survey.hpp` (`git diff --stat origin/main...6a35568`) [Lido].

## 5. Proposta ao dono (não aceita; nada muda sem decisão)
Os fatos acima separam o trabalho em duas partes.

**Parte funcional**, necessária para medir e para o header:
- F1-69 (lock e piso);
- F1-67, com os critérios funcionais;
- F1-68.

**Robustez contra falta de memória**, que não muda uma leitura medida:
- F1-73, F1-74, F1-71 e F1-72;
- as varreduras de OOM do F1-67.

Duas formas de seguir, para o dono escolher, além de manter a A:
- **C1, dividir os critérios do F1-67.** O F1-67 entra com tudo menos as varreduras de OOM do `sw_init` e do `sw_shutdown` e o resultado do `sw_shutdown` sob OOM. Essas três ficam num cartão de acompanhamento depois do F1-72, com o F1-67 declarando a limitação (OOM em C5 → `SW_E_IO`; `terminate` possível no `stop()` sem memória). Exige rever a regra de 2026-11-02 e aceitar, por prazo limitado, uma exceção à regra "sem excluir pontos".
- **C2, fechar o F1-69 primeiro com o que já está verde.** Aceitar o #37 com o `sw-core-floor-oom` do **MSVC Debug** declarado pendente do F1-73 (Release, Linux, ASan e TSan verdes [CI]), e então seguir com C1. Exige a mesma exceção, só nesse teste e nessa configuração.

Em ambas: o ADR-0018 continua precisando de aceite antes da regra do `sw_shutdown`, e nenhum critério do Portão 1 muda. **Não recomendo C' (F1-67 sem lock e piso).** Ela deixa dois códigos do header sem implementação e muda a ordem C1 a C8 do F1-60.

## 6. Achado lateral (não pedido; registrar e não corrigir aqui)
- `CacheOwner::stop()` volta cedo quando `thread.join()` lança (`cache_owner.cpp:950-954`), **sem** fechar journal e store. É o caso "chamado da própria dona", uso incorreto. É compatível com o comentário, mas a auditoria do F1-71 deve listá-lo [Lido].
