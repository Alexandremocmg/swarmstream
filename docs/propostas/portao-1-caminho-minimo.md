# Proposta: caminho mínimo até a evidência do Portão 1

**Status: PROPOSTA do Planejador (2026-11-06). Não aceita pelo dono. Não altera a ordem registrada, as autorizações, os critérios nem nenhum cartão.** Até a decisão do dono vale a ordem F1-73 → conclusão do F1-69/#37 → F1-74 → F1-71 → F1-72. O início de F1-71 a F1-74, F1-67 e F1-68 continua sem autorização; ADR-0018 segue Proposta.

**Base da análise:** texto dos cartões e do [docs/16](../16-briefing-fase-1a.md) na `main` `bd20afa` (2026-11-06). Li cartões, não o código; onde a necessidade técnica de uma dependência não está demonstrada, digo isso.

## 0. Duas correções do diagnóstico anterior
1. **A sequência F1-73 → F1-69 → F1-74 → F1-71 → F1-72 não é um desvio.** Pelas dependências registradas, ela bloqueia o F1-67 (que "precisa de F1-69, F1-70, F1-71 e F1-72 FEITAS antes do início"), e o F1-67 bloqueia o F1-68, o F1-59 e a integração real. Hoje ela **está no caminho crítico** da medição integrada. Adiá-la exige mostrar o que continua válido sem ela e quais contratos mudam (seção 4).
2. **"7–9 meses" não é previsão.** É a soma ilustrativa de estimativas antigas do [doc 15](../15-portao-fase-0-e-plano-3-recursos.md) (F1a 8–9 + F1b 5–6 + F2 12 + F3 8–10 semanas), sem trabalho restante, caminho crítico, capacidade nem incerteza. Uma previsão exigiria: esforço restante por cartão, a cadeia de dependências, horas reais de cada recurso e as incertezas (falhas de CI, revisões independentes, o resultado do próprio Portão 1). Nada disso está levantado.

## 1. Evidência que falta para decidir seguir, ajustar ou parar
Critério fixado em [docs/16](../16-briefing-fase-1a.md) (não muda): em `cabo_50`, no mapa de teste, com a rota do robô.

| Evidência exigida | Existe hoje? | O que falta |
| --- | --- | --- |
| Taxa de falta < 1% **em cada rodada**; zero faltas em lógica e colisão | Não | Leitura real passando por cache, agendador, fonte HTTP e telemetria `read_miss` |
| p99 da espera < 300 ms sobre os eventos reunidos de ≥ 3 rodadas comparáveis, N ≥ 300 faltas por perfil | Não | Idem + [F1-33](../../tarefas/cartoes/F1-33.md) completo (hoje com os eventos reais pendentes) + [F1-34](../../tarefas/cartoes/F1-34.md) |
| Stutter dentro dos limites do baseline | Limites aprovados; medição **com streaming** não existe | Rodadas com o caminho real; [F1-44](../../tarefas/cartoes/F1-44.md) espera o dono (limites vigentes mantidos) |
| Pop-in: avaliação visual registrada | Não | Política de textura [F1-21](../../tarefas/cartoes/F1-21.md) + avaliação do dono |
| Perfis `fibra_300` e `adsl_10` medidos e reportados | Não | Mesmas rodadas nos três perfis |
| Bancada validada: rede mais lenta piora a espera (R15) | Protocolo e script prontos; **sem medição** | [F1-47](../../tarefas/cartoes/F1-47.md) com o jogo lendo pela rede |

**Leitura de seguir / ajustar / parar** (docs/16; não é regra nova):
- **Incompleto** (faltou amostra, rodada não comparável, bancada não validada): **não decide nada**. Nunca vale como aprovação.
- **Seguir:** todas as métricas automáticas aprovadas, bancada validada, pop-in avaliado e registrado.
- **Ajustar** (até +4 semanas, prefetch e `core`): reprovação com causa atacável por prefetch ou `core`.
- **Parar ou repensar o produto:** o docs/16 diz só "repensar" (por exemplo P2P só para instalação e patches). **Proposta ao dono:** definir já o que contará como "ajuste insuficiente" (quantas rodadas de ajuste e qual critério), para não decidir depois de ver o resultado.

## 2. Portão do loader com mocks × medição integrada
| | Portão do ADR-0016 (passou) | Medição integrada do Portão 1 |
| --- | --- | --- |
| Pergunta | O loader consegue materializar textura BCn 4K com mips, malha com 2 LODs e OGG a partir de bytes de uma fonte, sem `FileAccess`, com troca no lugar? | O jogo roda bem lendo do cache local, com faltas atendidas pela "CDN" lenta? |
| Fonte dos bytes | Adaptador sobre mocks do `sw-core` (`MemoryContentSource`) | Cache em disco, agendador, fonte HTTP, telemetria reais, via API C |
| Evidência | [f1-20-portao.md](../medicoes/f1-20-portao.md), PR #26 | Rodadas nos três perfis, relatório do F1-33 |
| Prova | A viabilidade do **caminho do Godot** (decisão do ADR-0016) | O **critério de produto** (R1) |
| O que não prova | Taxa de falta, p99 de espera, stutter com I/O real, rede | — |

**Um resultado com mocks não substitui a validação do caminho real.** Ele só reduz o risco de integração do lado do Godot.

## 3. Tarefas estritamente necessárias e dependências
Pelas dependências **hoje registradas**:

```
F1-73 → F1-69/#37 (conclusão) → F1-74 → F1-71 → F1-72 ─┐
F1-70 (FEITA) ───────────────────────────────────────────┤
ADR-0018 Aceito (decisão do dono) ───────────────────────┴→ F1-67 → F1-68 → F1-59
                                                              │         │      │
                                                              │         │      └→ F1-33 completo (eventos reais)
F1-04 (aceite do dono) → F1-05 → F1-07 (core, mips) ──────────┴─────────┴→ F1-20 parte 2 sobre o sw-core real → F1-21
F1-47 (bancada) → F1-34 (rodadas) → relatório e avaliação visual → decisão do Portão 1
```
Necessárias para o Portão 1 (nenhuma é opcional, pelo critério atual): leitura real (F1-67, F1-68), telemetria real (F1-59, F1-33 completo), conteúdo do mapa com `core` e mips (F1-04, F1-05, F1-07), loader sobre o `sw-core` real (F1-20 parte 2), política de textura (F1-21), validação da bancada (F1-47), rodadas (F1-34), baseline com as mesmas condições (F1-44, limites vigentes). **F1-04 está AGUARDANDO_DONO**: é uma decisão do dono que já está no caminho crítico, independente de código.

## 4. O que pode avançar em paralelo, sem mudar a ordem
| Frente | O que avança | Por que é válido sem a cadeia | Limite |
| --- | --- | --- | --- |
| **Bancada** | Revisão do protocolo e do script do F1-47 contra o [F1-53](../../tarefas/cartoes/F1-53.md) já na `main`; rodadas de **controle de deriva** da própria bancada (sw-netlab sem o jogo) | Não depende do caminho do jogo | **Não** é a validação do F1-47: ela exige o jogo lendo pela rede |
| **Relatório** | F1-33 versão completa **contra fixtures**; método do percentil e mínimo de amostras já aceitos (F1-48) | Lógica de agregação independe da origem dos eventos | Os eventos reais só vêm com o F1-59 |
| **Godot** | F1-20 parte 2 **com mock** (o mapa carrega pelo loader; `_load` nunca espera a rede, medido); F1-24 (overlay); F1-21 desenvolvida contra o mock | Testa o comportamento do loader, não o produto | Resultado marcado como "com mock". F1-21/F1-24 só se **verificam** com o `sw-core` real depois do F1-68 (F1-60 §8) |
| **Dono** | Decidir F1-04; avaliação visual; F1-44 | Decisões que estão paradas | — |

Nada disso exige autorização nova de cartão crítico; **mas cada item acima é um cartão ainda não autorizado ou em andamento, e eu não o inicio**. A lista é o que o dono pode escolher autorizar.

## 5. O que se poderia adiar, com justificativa e limite
**Não adiar sem decisão do dono:** os pré-requisitos do F1-67 foram escolhas dele (2026-11-02). As opções abaixo só existem como propostas.

| Opção | O que muda | Experimento que continua válido | Contratos afetados | Risco / limite |
| --- | --- | --- | --- | --- |
| **A. Manter a ordem registrada** | Nada | Todos, só na ponta da cadeia | Nenhum | Medição integrada só depois de 5 cartões críticos serializados, cada um com revisão independente |
| **B. Pergunta ao R1 antes de decidir** (só leitura, sem implementar) | Uma análise: quais partes de F1-69..F1-72 o caminho **mínimo** do F1-67 usa para uma leitura funcional (lock e piso no `sw_init`; parada; recuperação) e quais são só robustez contra falta de memória no MSVC Debug | Nenhum é alterado | Nenhum | Barata; transforma a discussão em fatos. **Recomendada antes de qualquer corte** |
| **C. Separar a robustez de falta de memória** (F1-71, F1-72, F1-74, F1-73) do início do F1-67, mantendo F1-69 (lock/piso) | Mede-se antes com contrato funcional completo e robustez de OOM depois | Medição de falta, p99 e stutter (não dependem de OOM) | `sw_shutdown` e `sw_init` sem a garantia "falta de memória não termina o processo" (F1-60 §1.1), `CacheErrorCode::OutOfMemory/Internal` (F1-74), ADR-0017/0018 | Retrabalho em `stop()`/`start()` depois; o dono teria de rever a regra de 2026-11-02; **só se o R1 confirmar** que o caminho funcional não depende disso |
| **D. Adaptador de medição fora da API C** (sw-godot sobre cache, agendador e fonte HTTP reais, sem `sw_*`) | Responde cedo à pergunta da bancada (R15) | Validação da bancada; talvez taxa de falta e espera | `swarmstream.h` não muda, mas **o que se mede não é a API do produto**: sem handles, `sw_shutdown`, lock e piso | Risco de medir outra coisa; precisaria rotular como **preliminar** e não vale como aprovação do Portão 1; é **implementação nova**, portanto fora do que o dono permite hoje |

**Recomendação do Planejador:** B agora (sem custo de implementação); decidir entre A e C com a resposta. D só se o dono quiser a validação da bancada antes e aceitar o rótulo "preliminar".

## 6. Perguntas ao dono
1. Autoriza pedir a análise B ao R1 (só leitura)?
2. Decide o F1-04 (aceite do parcial ou lista do que falta)? Está no caminho crítico do conteúdo do mapa.
3. Quer definir antes das rodadas o que conta como "ajuste insuficiente" (seção 1)?
4. D: permite ou não, mesmo como medição preliminar?

Nada nesta proposta reduz critério, amplia implementação ou autoriza início.

## 7. Respostas do dono recebidas em 2026-11-06 (registro)
| Ponto | Resposta (texto do dono) | Como tratei |
| --- | --- | --- |
| 1. Análise do R1 (opção B) | "Autorizo pedir ao R1 a análise só de leitura (opção B)…" | **Claro.** Pedido pronto em [pedido-r1-analise-caminho-minimo.md](pedido-r1-analise-caminho-minimo.md); o dono abre a sessão. |
| 2. F1-04 | "o F1-04 (aceite do que falta)" | **Ambíguo; aguardo confirmação** (ver abaixo). Não alterei o F1-04 nem os limites. |
| 3. Opção D (adaptador fora da API C) | "Permite o adaptador de medição fora da API C (opção D)" | **Ambíguo; aguardo confirmação.** Não inicio nada: não existe cartão, e a opção D é implementação nova. |
| 4. R12 | "autorizar buscas em arquivos públicos" | **Claro.** Feito e registrado no [estudo](../estudos/r12-blizzard-e-premissas-comerciais.md) (F8). |

**O que preciso confirmar:**
- **F1-04:** você aceita que o parcial já mesclado (`a4bd5580`) **fique como está**, com tempo, tamanho e memória pendentes no F1-58, sem aprovar esses três critérios? Ou "aceite do que falta" é aprovar os critérios pendentes (isso exigiria a remedição do F1-58 e a sua decisão sobre 705 ms e 61,6 MiB)? São decisões diferentes.
- **Opção D:** você autoriza **criar um cartão** para o adaptador de medição preliminar (que continua **sem início autorizado** e com o resultado rotulado "preliminar", sem valer como aprovação do Portão 1), ou autoriza **iniciar** a implementação já?
