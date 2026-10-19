# Relatório da rodada exemplo-baseline-gpu-ssd-local

**Métricas automáticas: incompleto**

**Métricas automáticas aprovadas NÃO aprovam sozinhas o Portão 1:** falta a avaliação visual registrada de pop-in (manual, fora deste relatório).

Regra do Portão 1 (docs/16 e docs/08), decidida em `cabo_50`: taxa de falta < 1% em cada rodada; zero faltas em lógica e colisão; espera p99 < 300 ms sobre os eventos individuais reunidos de pelo menos 3 rodadas completas e comparáveis (nunca a média dos p99). Stutter e carga dentro dos limites absolutos aprovados em todos os perfis. `n/d` = métrica ausente, inválida ou sem amostras (nunca 0); a rodada fica `incompleto`, nunca `passou`.

Janela da telemetria: rota do robô com `SW_GAME_STATE_PLAYING`, carga inicial à parte. **mínimo pendente do F1-48** (o p99 agregado fica `incompleto` até informar --min-samples).

| Perfil | Rodadas | Veredito |
| --- | --- | --- |
| fibra_300 | 0 | incompleto |
| cabo_50 | 0 | incompleto |
| adsl_10 | 0 | incompleto |
| ssd_local | 5 | incompleto |

## fibra_300: incompleto

Motivos:
- perfil ausente

## cabo_50: incompleto

Motivos:
- perfil ausente

## adsl_10: incompleto

Motivos:
- perfil ausente

## ssd_local: incompleto

| Métrica | Valor | Critério | Situação |
| --- | --- | --- | --- |
| Quadros > 33 ms (após 3 s), pior rodada | 1 | <= 2 | ok |
| Quadros > 100 ms, pior rodada | 0 | <= 0 | ok |
| p99 do quadro (ms), pior rodada | 4.238 | <= 6.238 | ok |
| Carga (ms) | 2549.51 | mediana <= 3059.05 ms (>= 3 rodadas) | ok |
| Taxa de falta, por rodada | n/d · n/d · n/d · n/d · n/d | informativo | n/d |
| Espera numa falta, p99 agregado (ms) | n/d (N=0, 0 rodadas) | informativo | n/d |
| Faltas não concluídas (failed / cancelled / pending_at_end) | n/d | = 0 | n/d |

| Rodada | Leituras | Faltas | Taxa | Faltas core | Amostras (N) | p99 (ms) | Completa |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | n/d | n/d | n/d | n/d | n/d | n/d | não |
| 2 | n/d | n/d | n/d | n/d | n/d | n/d | não |
| 3 | n/d | n/d | n/d | n/d | n/d | n/d | não |
| 4 | n/d | n/d | n/d | n/d | n/d | n/d | não |
| 5 | n/d | n/d | n/d | n/d | n/d | n/d | não |
| **Agregado** (0 completas) | 0 | 0 | n/d | — | 0 | n/d | — |

Motivos:
- rodada 1: telemetria ausente
- rodada 2: telemetria ausente
- rodada 3: telemetria ausente
- rodada 4: telemetria ausente
- rodada 5: telemetria ausente
