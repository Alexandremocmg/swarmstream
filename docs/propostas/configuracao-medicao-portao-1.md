# Proposta: configuração de build das medições do Portão 1 (e comparabilidade com o baseline)

**Status: PROPOSTA do Planejador (2026-11-09). Não aceita pelo dono. Nenhuma configuração foi escolhida.** Responde ao ponto P5 da reconciliação C2/C1 ([F1-67](../../tarefas/cartoes/F1-67.md), seção "Limitações declaradas"). Não muda critério do Portão 1 nem os limites do baseline.

**Regra central (a preservar):** a configuração é decidida **antes** de qualquer rodada do Portão 1 e vale para **todas** as rodadas, nos três perfis. É proibido escolher ou trocar a configuração **depois de ver qual passa**. Resultados de configurações diferentes não são comparados entre si.

## 1. Fatos lidos (com a fonte)
| # | Fato | Fonte | Nível |
| --- | --- | --- | --- |
| 1 | O baseline foi medido com Godot 4.7.2-stable, GL Compatibility, vsync desligado, GTX 1050 Ti, assets em NVMe (Kingston SNV3S1000G), rota de 60 s | [baseline.md](../medicoes/baseline.md) linha 3 | [Lido] |
| 2 | Validade do baseline: "esta máquina e configuração"; "mudou hardware, driver, versão do Godot ou a rota: refazer" | [baseline.md](../medicoes/baseline.md) linha 58 | [Lido] |
| 3 | Limites aprovados: `frames_over_33ms_steady` 2, `frames_over_100ms` 0, `p99_frame_ms` 6,238, `load_ms` 3059,05 | [baseline-limits.json](../medicoes/baseline-limits.json) | [Lido] |
| 4 | O texto do baseline **não** menciona o tipo de build do `sw-core` nem da extensão GDExtension (não há `Release`, `Debug`, `template_*` ali) | busca por esses termos em `baseline.md`, `procedimento-baseline-10-rodadas.md` e `protocolo-mudanca-conhecida.md` (sem ocorrência) | [Lido, por busca]; **não** prova que a extensão não estava carregada |
| 5 | O portão do loader (F1-20) usou GCC 16.2 (MinGW-w64), CMake + Ninja, **Debug**, e lista como pendência o export com `template_release` e o **MSVC** | [f1-20-portao.md](../medicoes/f1-20-portao.md) linhas 19, 62 e 74 | [Lido] |
| 6 | O CI roda build+test no Windows com **MSVC**, em Debug e Release, e "release tests" no Ubuntu | [CLAUDE.md](../../CLAUDE.md) (seção Comandos) | [Lido] |
| 7 | Os seis sítios do F1-73 (terminate por falta de memória) só existem no **MSVC Debug**; a análise do R1 diz que o binário medido em Release ou GCC não os tem | [análise do R1](analise-r1-caminho-minimo.md), seção 4, item 1 | [Lido no documento]; não reexecutado |

## 2. Proposta
1. **Windows (a máquina do baseline), MSVC, Release** (`CMAKE_BUILD_TYPE=Release`, `NDEBUG`) para `sw-core` e para a extensão GDExtension, em **todas** as rodadas (`cabo_50`, `fibra_300`, `adsl_10`, controle de deriva e a comparação A–B–A do F1-47). Motivos: (a) é o produto que o jogador usa; (b) o Debug do MSVC inclui verificações de iteradores que alteram tempo e alocação e **não** representam o produto; (c) é o mesmo compilador do CI de referência, com o passo `[Release]` já existente.
2. **Mesmo Godot 4.7.2-stable, GL Compatibility, vsync desligado, GTX 1050 Ti e mesmo driver, NVMe, rota de 60 s e modo de textura do baseline.** A extensão carregada no Godot é o único acréscimo; o custo dela é justamente o que o limite de stutter pretende limitar.
3. **Rodadas de controle de deriva** da bancada antes e depois do conjunto, como já previsto no F1-47.

## 3. Conferência de comparabilidade com o baseline
| Item | Estado |
| --- | --- |
| Hardware, driver, versão do Godot, rota, renderizador, vsync | **Iguais**, se a máquina e o driver não mudaram desde o baseline. **A conferir** na execução: a versão do driver e a data do baseline precisam constar no relatório da rodada |
| Tipo de build do `sw-core` e da extensão | O baseline **não o registra** (fato 4). Se o baseline foi medido **sem a extensão carregada**, nenhuma refação é exigida pelo tipo de build; a comparação "com streaming × sem" mede o custo do produto. Se foi medido **com** a extensão em outro tipo de build (por exemplo o Debug do MinGW do F1-20), o baseline **precisa ser refeito** na configuração escolhida ou a comparação não vale. **Isso não está verificado.** Antes de qualquer rodada: ler o procedimento e o relatório do baseline e conferir, na máquina, se a extensão estava carregada |
| Alvo do godot-cpp (`template_debug` × `template_release`) compatível com o `godot_console.exe` que o `baseline.py` usa | **Não verificado.** A escolha tem de ser a mesma em todas as rodadas e registrada antes |

**Se o baseline tiver de ser refeito** (hardware, driver, Godot, rota ou configuração da extensão mudaram): refazer com o procedimento de 10 rodadas e submeter os novos limites ao dono; **os limites atuais (F1-44) continuam vigentes até lá** e **não** se comparam resultados de configurações incompatíveis.

## 4. O que esta escolha implica e que o dono precisa saber
- Em Release os sítios do F1-73 **não existem no binário medido**. Isso **não** torna o F1-73 desnecessário: ele continua exigido (L1, MSVC Debug), mas deixa de influenciar a medição. É um efeito da escolha, declarado aqui antes de qualquer resultado.
- Os testes do `sw-core` continuam rodando em Debug **e** Release no CI; a configuração de **medição** não reduz nenhum critério de teste.
- Esta proposta não escolhe a configuração por ela ser mais favorável: o critério de escolha é "o que o jogador usa". Se o dono preferir outra (por exemplo GCC Release ou MSVC Debug), a decisão e o motivo ficam registrados **antes** das rodadas.

## 5. O que preciso do dono
1. Aceita **MSVC Release** (sw-core e extensão) para todas as rodadas do Portão 1?
2. Autoriza conferir, na máquina do baseline, se a extensão estava carregada e a versão do driver (só leitura, sem rodar benchmark)?
