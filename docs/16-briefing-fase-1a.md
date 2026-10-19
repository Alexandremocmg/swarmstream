# Briefing da Fase 1a: o que pedir a cada recurso

Fase 1a aberta pelo dono do produto em 2026-10-18, depois do [parecer final da Fase 0](15-portao-fase-0-e-plano-3-recursos.md#parecer-final-do-portão-da-fase-0-2026-10-18). Duração prevista: **≈ 8–9 semanas** com 3 recursos. Termina no **Portão 1**.

## Objetivo
Provar, **sem P2P**, que um jogo roda bem lendo só do cache local, com as faltas atendidas por uma "CDN" lenta simulada. É a validação do maior risco do projeto ([12-riscos.md](12-riscos.md), R1).

## Critério do Portão 1 (definido agora, para ninguém ajustar depois)
Medido na bancada, perfil `cabo_50` (50 Mbps, 25 ms), no mapa de teste, com a rota do robô:

| Métrica | Meta |
| --- | --- |
| Taxa de falta (leituras sem dado local / total) | **< 1% em cada rodada** (não vale a média das rodadas); **zero faltas em lógica e colisão** |
| Espera do jogo numa falta, p99 | **< 300 ms, calculado sobre os eventos individuais reunidos de pelo menos 3 rodadas completas e comparáveis** (não é a média dos p99 das rodadas). Sem faltas suficientes o resultado é `incompleto`: **ausência de faltas não vira p99 = 0**. Método do percentil e mínimo de amostras: proposta do R1 no cartão F1-48, aceita antes das medições finais |
| Stutter | Dentro dos **limites absolutos aprovados** em [baseline.md](medicoes/baseline.md): quadros > 33 ms ≤ 2 por rodada de 60 s, > 100 ms = 0, p99 ≤ 6,24 ms, carga ≤ 3,06 s pela mediana de 3 rodadas |
| Pop-in | Avaliação visual registrada, com o tempo médio em mip ou LOD rebaixado |
| Perfis `fibra_300` e `adsl_10` | Medidos e reportados; `adsl_10` pode ter pop-in maior, mas **sem travar o frame** |

Regra decidida pelo dono do produto em 2026-10-19 ([tarefas/decisoes.md](../tarefas/decisoes.md)). O relatório da rodada registra, **por rodada**, o número de leituras, de faltas e o p99, e também o **resultado agregado** com o número de amostras. **Métricas automáticas aprovadas não aprovam sozinhas o Portão 1:** é obrigatória a avaliação visual registrada de pop-in.

Se falhar: ajustar prefetch e `core` (até +4 semanas) ou **repensar o produto** (por exemplo, usar o P2P só para instalação e patches).

Duas condições antes de confiar nos números: **F1-36** pronta (a checagem não pode passar com relatório vazio) e **a bancada validada com uma mudança conhecida** (rede mais lenta tem de piorar a espera; risco R15), o que só é possível quando o jogo já lê pela rede (semana 5).

## O que entra e o que fica para a F1b
| Na F1a | Na F1b (depois do Portão 1) |
| --- | --- |
| Cache em disco com journal, slots, leases, remoção CLOCK, checksum (F1-10 a F1-14, F1-16) | RAM descomprimida e pool de buffers (F1-15) |
| VFS (F1-17) | Ordenação de peças por localidade (F1-06) |
| `sw-pack` mínimo: chunker, compressão, manifesto, classificação, mips no `core` (F1-01, 03, 04, 05, 07) | Traces e índice espacial (F1-25, F1-26) |
| Fonte HTTP, estimador, agendador v1 (F1-27, F1-29, F1-28) | Política de malha e áreas de dica (F1-22, F1-23) |
| Loader do Godot e política de falta de textura (F1-20, F1-21) | Experimento de tamanho de bloco (F1-35) e decisão do tamanho do manifesto |
| Telemetria, relatórios, rodadas (F1-30, F1-33, F1-34), testes de queda (F1-31, F1-32) | |
| F1-36, inspetor do cache (F1-18), overlay (F1-24), testes e relatório do `sw-pack` (F1-02, F1-08) | |

## Regras para os três
1. **Contrato primeiro, mocks sempre.** Quem depende de outro trabalha contra os mocks de [`sw-core`](../sw-core/) (`MemoryCacheStore`, `MemoryContentSource`) e contra o `swarmstream.h` v0 congelado. Mudar o header exige ADR novo ([ADR-0014](adr/0014-contrato-swarmstream-h-v0.md)).
2. **PR pequeno, CI verde, evidência colada.** Nenhum PR passa sem build e teste em Ubuntu e Windows, ASan+UBSan e lint.
3. **Código crítico exige dois olhos** (journal, índice, leases, agendador, verificação de hash, parsing): o autor e outro recurso. Se o autor é o R1, o segundo revisor é o R2.
4. **Medir antes de afirmar.** Número sem o comando e a máquina que o geraram não vale.
5. **Falha silenciosa é bug.** Todo valor de retorno de I/O é conferido. Teste que passa sem testar nada (como a checagem vazia do F1-36) é defeito.
6. **Descobriu que a premissa está errada? Pare e avise** (como o spike do `FileAccess`). Isso é resultado, não atraso.

## Cronograma sugerido (semanas)
| Semana | R1 | R2 | R3 |
| --- | --- | --- | --- |
| 1 | **F1-20 portão:** textura BCn 4K, malha com 2 LODs e áudio OGG materializados a partir do mock | **F1-36** primeiro, depois F1-01 (chunker C++) | Teste de 1 hora no Windows; esqueleto do F1-33 (pedidos prontos em [17](17-pedidos-semana-1.md)) |
| 2–3 | F1-10, F1-12 (slots, thread dona) | F1-03, F1-04 (compressão, manifesto) | F1-02, F1-08 (testes e relatório do `sw-pack`) |
| 4–5 | F1-11, F1-13 (journal, leases) | F1-27, F1-29 (fonte HTTP, estimador); F1-17 (VFS) | F1-18 (inspetor do cache), F1-30 (telemetria); **validar a bancada com uma mudança conhecida assim que o jogo ler pela rede** |
| 6–7 | F1-28 (agendador v1); F1-21 (política de textura) | F1-14, F1-16 (remoção, checksum); F1-05, F1-07 (core, mips) | F1-24 (overlay), F1-33 (relatório da rodada) |
| 8–9 | Integração, medição no mapa de teste | F1-31, F1-32 (queda, disco cheio) | F1-34 (rodadas nos 3 perfis); relatório do Portão 1 |

Folga: os prazos são estimativa. Se uma tarefa passar de 5 dias, o R1 quebra em partes.

---

## Pedido ao R1 (líder técnico)

> Você abre a Fase 1a. Sua primeira entrega, antes de qualquer outra, é o **portão obrigatório do ADR-0016**: a partir de bytes servidos pelo mock do `sw-core`, sem usar `FileAccess`, o loader do Godot tem de materializar (1) uma textura BCn 4K com mips, (2) uma malha com 2 LODs e (3) um áudio OGG. Se isso não fechar, a F1-21 e a F1-22 não começam e o plano de integração com o Godot muda (o `.pck` por grupo entra como reserva).
>
> Depois, o cache em disco: slots pré-alocados (F1-10) e thread dona do cache (F1-12), depois journal (F1-11) e leases (F1-13). Siga o protocolo do [04](04-cache-local.md): dados, `fdatasync`, registro no journal, `fdatasync`, publicação no índice; todo retorno de escrita conferido. Em seguida o agendador v1 (F1-28) e a política de falta de textura (F1-21). Todo código seu de journal, índice, leases e agendador precisa da revisão do R2.
>
> **Pronto quando:** o portão do ADR-0016 passa (semana 1) e o Portão 1 fecha (semana 9). Skills úteis: `gdextension`, `multithreading`, `db-acid-in-practice`, `memory-model`, `concurrency-debugging`, `sanitizers`.
>
> **Atenção:** você acumula cache, agendador e Godot (risco R14). Documente cada decisão em ADR ou no PR, para que os outros dois consigam trabalhar sem você. O spike do Godot mostrou que as cargas em thread usam só ~30% dos núcleos; confirme a causa lendo o código do `WorkerThreadPool` no tag 4.7.2.

## Pedido ao R2 (engenharia e qualidade)

> Sua primeira entrega é a **F1-36**: o `baseline.py --check` hoje diz "within limits" e sai com código 0 quando o relatório não tem as métricas. Corrija para falhar quando faltar `frames_over_33ms_steady`, `frames_over_100ms`, `p99_frame_ms` ou `load_ms`, e acrescente teste (3 relatórios vazios e 3 com chaves erradas devem sair com código 1 e dizer qual métrica faltou). Sem isso, o Portão 1 pode passar sem medir nada.
>
> Depois, o **chunker em C++ (F1-01)**, com a condição aprovada: escreva **só a partir da spec** ([03 §11.1](03-formato-de-conteudo.md)), sem ler as versões em Python, e reproduza todos os vetores de `sw-pack/testdata/` antes de entrar na `main`. Em seguida compressão, manifesto assinado com a chave de teste, fonte HTTP, estimador de banda, VFS, remoção CLOCK, checksum e classificação core/grupo. Por último, os testes de queda (matar o processo em pontos aleatórios da escrita; meta: 1.000 quedas sem peça corrompida aceita) e de disco cheio. Você é o segundo revisor de todo código crítico do R1.
>
> **Pronto quando:** F1-36 em PR com teste (semana 1), chunker com 100% dos vetores, e as tarefas acima com critério de aceite do [backlog](11-backlog-inicial.md). Skills úteis: `cpp-pro`, `modern-cpp`, `cmake`, `boost-asio-pro`, `sanitizers`, `static-analysis`, `test-driven-development`.

## Pedido ao R3 (conteúdo e apoio)

> Sua primeira semana é de **preparar a medição**: (1) repita o **teste de 1 hora no Windows** (Windows Sandbox, pessoa que não montou o ambiente, só o guia do CONTRIBUTING, cronômetro) e registre em `docs/medicoes/guia-1h.md`; (2) faça o esqueleto do script de relatório da rodada (F1-33), lendo o formato dos relatórios do `baseline.py`. A **validação da bancada com uma mudança conhecida** (trocar `cabo_50` por `adsl_10` e ver a espera piorar de forma coerente; se não piorar, a bancada não mede o que deveria) fica para o primeiro momento em que o jogo ler pela rede, previsto para a semana 5: antes disso não existe caminho de rede e a troca de perfil não altera nada.
>
> Depois: testes de borda e relatório do `sw-pack` (F1-02, F1-08), inspetor do cache (F1-18), telemetria de `read_miss`, `eviction` e `frame_stutter` (F1-30), overlay de debug no Godot (F1-24) e as rodadas semanais nos 3 perfis com comparação ao baseline (F1-34). Ao fim, monte o **relatório do Portão 1** com a tabela de métricas acima.
>
> **Pronto quando:** cada item tem evidência colada no PR (saída de comando, números, link de execução do CI). Quando algo não fizer sentido, pergunte ao R1 ou ao R2 antes de gastar mais de meio dia. Skills úteis: `python-pro`, `property-based-testing`, `godot-debugging`, `godot-testing`.

---

## Papel do dono do produto
| Quando | O que fazer |
| --- | --- |
| Toda sexta | Ler o relatório da rodada da semana (R3) e decidir se algo mudou o plano |
| A cada PR crítico | Aceitar depois da revisão do segundo recurso; conferir que o CI está verde |
| Semana 4 | Revisar o ponto de controle: o portão do ADR-0016 passou? o journal está testado contra queda? |
| Semana 9 | Decidir o Portão 1 com o relatório do R3 (siga os critérios no topo; não ajuste depois de ver o resultado) |
| Durante a F1a | Seguir procurando o estúdio indie em Godot para o piloto; tomar nota de contatos |

## O que continua pendente da Fase 0 (herdado)
Ver a tabela "Pendências que seguem para a Fase 1" no [parecer final](15-portao-fase-0-e-plano-3-recursos.md#parecer-final-do-portão-da-fase-0-2026-10-18). Em especial: F1-36 antes do Portão 1, chunker em C++ por pessoa diferente dos autores dos vetores, formatação mecânica do C++ depois de 28/10, e o teste de 1 hora no Windows.

## Andamento da Fase 1a (verificado no GitHub)

### Semana 1, atualizado em 2026-10-18
| Tarefa | Estado | Evidência |
| --- | --- | --- |
| **F1-36** checagem vazia do baseline | **Feita** (PR #11, merge b4eddb0) | CI verde nos 4 jobs, inclusive o novo passo de testes no `lint`. Verificado por mim do zero: os 14 testes passam e 16 casos adversos se comportam como o esperado (relatórios vazios, chaves erradas, arquivo-resumo no lugar de rodada, valor texto, NaN, negativo, uma rodada sem p99, nenhuma com `load_ms`; mais os 7 casos de limite e de mediana que já funcionavam) |
| **F1-01** chunker em C++ | Não iniciada (nenhum branch no GitHub) | |
| **F1-20** portão do Godot | Não iniciada (nenhum branch no GitHub) | |
| **F1-33** esqueleto do relatório | Não iniciada | |
| `CLAUDE.md` e pedidos da semana 1 | **Na `main`** (PR #10) | |

### Pontos de atenção
- **Aceite sem registro no GitHub:** os PRs #10 e #11 foram aceitos sem nenhuma revisão ou comentário registrado (0 e 0). A revisão do #11 foi feita em conversa, e o registro não foi postado porque o modo automático bloqueou. Sem registro, a regra de "dois olhos" não deixa rastro. Daqui em diante, a sessão que revisa **comenta o veredito no PR** antes do aceite.
- **PR #10 aceito por outra sessão:** não foi aceito pela sessão que o escreveu. Confirmar com o dono do produto que foi autorizado.
- **Agentes com contexto desatualizado:** um relato de status citou os PRs #2 e #3 como "aguardando merge", o #1 como "a fechar" e o reinício do Windows como "esperando OK". Os três já estão resolvidos (PRs #2 a #9 aceitos, #1 fechado, reinício dispensado em 2026-10-17). Toda sessão nova deve começar lendo o `CLAUDE.md` e o parecer em [15](15-portao-fase-0-e-plano-3-recursos.md).
- **Nota do ruff:** os avisos `S603` e `S607` (uso de `subprocess` por `sys.executable` no teste) já existiam e o lint do CI não cobre esses arquivos com a regra de segurança. Aceitável; reavaliar se o escopo do lint mudar.
- **Pasta compartilhada:** continua no branch `feat/F0-08-vetores-de-teste`, atrasada em 1 commit, com uma alteração em `docs/medicoes/guia-1h.md`. Alguém deve decidir se essa alteração vira commit ou é descartada, e então voltar a pasta para a `main`.
