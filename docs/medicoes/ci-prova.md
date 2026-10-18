# Prova do CI: um PR vermelho e um PR verde (item 3 do fechamento da Fase 0)

Repositório: <https://github.com/Alexandremocmg/swarmstream> · Data: 2026-10-17 · Responsável: R2.

## Resultado

| PR | O que faz | Base | Resultado | Execução do CI |
| --- | --- | --- | --- | --- |
| [#2](https://github.com/Alexandremocmg/swarmstream/pull/2) | Corrige o travamento no Windows e a config do clang-format (**verde**) | `main` | `build+test` Ubuntu **pass**, `build+test` Windows **pass**, `ASan+UBSan` **pass**, `lint` **pass** | [ci](https://github.com/Alexandremocmg/swarmstream/actions/runs/39081441960) · [lint](https://github.com/Alexandremocmg/swarmstream/actions/runs/39081441964) |
| [#1](https://github.com/Alexandremocmg/swarmstream/pull/1) | Troca uma asserção de `test_cache_store.cpp` por um valor errado (**vermelho**, rascunho, não fazer merge) | branch do #2 | `build+test` Ubuntu **fail**, `build+test` Windows **fail**, `ASan+UBSan` **fail**; `lint` **pass** | [ci](https://github.com/Alexandremocmg/swarmstream/actions/runs/39081465233) · [lint](https://github.com/Alexandremocmg/swarmstream/actions/runs/39081465216) |

O PR vermelho tem como base o branch do verde (PRs empilhados), para provar o vermelho **com** as correções já presentes. A causa do vermelho é só a asserção quebrada: o `lint` do mesmo PR passa.

Mensagens de falha (logs do CI):
- Linux: `test_cache_store.cpp:41 ... Assertion 'lease.size() == test_data.size() + 1' failed`, `Subprocess aborted`.
- Windows: `Assertion failed: lease.size() == test_data.size() + 1, file ...\test_cache_store.cpp, line 41`, `1 - sw-core-tests (Failed)`.
- ASan+UBSan (Linux): mesma asserção, exit code 8.

## O que a primeira tentativa de vermelho revelou (e foi corrigido no #2)

A primeira versão do PR vermelho (contra `main`, sem as correções) **não ficou vermelha no Windows: ficou pendurada** por mais de 6 minutos no passo `Test` até ser cancelada à mão. Sem o #2 o CI não cumpria "PR quebrado fica vermelho".

| # | Defeito | Causa | Correção no #2 |
| --- | --- | --- | --- |
| 1 | Windows: job trava em vez de falhar | Causa provável: `assert` do MSVC em Debug abre caixa de diálogo e espera (não vi o log, o job foi cancelado; a confirmação é indireta: com a correção o mesmo teste falha em ~1 min) | `sw-core/tests/main.cpp` manda o relatório de `assert` para o stderr e aborta (`_set_abort_behavior`, `_CrtSetReportMode`, `SetErrorMode`); `TIMEOUT 120` no teste; `ctest --timeout 120`; `timeout-minutes: 20` nos jobs |
| 2 | `lint` falhava em **todo** PR com C++ | `.clang-format` usava `BreakTemplateDeclarations` (clang-format ≥ 19); o runner tem o 18 e recusa a chave (`unknown key`) | Trocado por `AlwaysBreakTemplateDeclarations: Yes` |
| 3 | Mesmo com a config válida, o passo de formato reprova quase todo o C++ do repositório | O código existente foi escrito com chaves na mesma linha da função; a config manda quebrar (`AfterFunction: true`) e há outras diferenças. Medido com clang-format 18.1.8 sobre o conteúdo versionado | Reformatados só os dois arquivos que o PR toca (`main.cpp`, `test_cache_store.cpp`). **Pendência abaixo** |

## Pendências conhecidas (não bloqueiam o item 3)

1. **Formato do C++ existente.** Contagem de linhas que o clang-format 18 alteraria no conteúdo de `main` (antes do #2): `swarmstream.h` 116, `memory_cache_store.cpp` 67, `types.hpp` 54, `memory_content_source.cpp` 51, `api_v0_smoke.c` 34, `result.hpp` 32, e outros menores. O passo de formato só olha arquivos alterados no PR, então qualquer PR que toque esses arquivos vai ficar vermelho no `lint` até eles serem formatados. Recomendação: um PR mecânico "aplica clang-format em todo o C++" **depois** do congelamento do `swarmstream.h` (28/10), para não conflitar com as edições em andamento do R1 (há alterações não commitadas nesse header e em `api_stub.cpp`/`api_v0_smoke.c`). Alternativa: ajustar a config ao estilo do código (menos mudança, mas ainda não zera o diff).
2. **MSVC só no CI.** O build local desta máquina usa MinGW; o Windows Defender local passou a bloquear o `c++.exe` do WinLibs ("arquivo contém vírus ou software potencialmente indesejável", erro 225), então a correção do #2 foi validada **apenas pelo CI**. Não alterei exclusões nem configurações do Defender. Falso positivo provável, mas não confirmado.
3. O PR #1 permanece aberto como rascunho de prova; fechar sem merge quando o #2 entrar.
4. `sw-core/tests` usa `assert`, que desaparece com `NDEBUG`: um build Release dos testes passaria sem verificar nada. O CI roda só Debug; trocar por um macro de verificação própria está fora deste PR.
