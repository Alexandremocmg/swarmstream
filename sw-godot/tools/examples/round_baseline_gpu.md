# Relatório da rodada exemplo-baseline-gpu-ssd-local

**Portão 1 (automático): incompleto**

Critérios: taxa de falta < 1% e espera numa falta p99 < 300 ms (decididos em `cabo_50`); stutter e carga dentro dos limites absolutos aprovados em todos os perfis. `n/d` = métrica ausente ou inválida; a rodada fica `incompleto`, nunca `passou`.

Janela da telemetria: rota do robô com `SW_GAME_STATE_PLAYING`, carga inicial à parte; taxa e p99 por rodada e com as rodadas juntas (docs/08). Agregação no Portão 1: **pendente do dono do produto**; só dá `passou` quando as duas regras concordam.

Pop-in: avaliação visual manual, fora deste relatório (registrar à parte).

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
| Taxa de falta, por rodada | n/d | informativo | n/d |
| Taxa de falta, rodadas juntas | n/d | informativo | n/d |
| Espera numa falta p99 por rodada (ms) | n/d | informativo | n/d |
| Espera numa falta p50/p95/p99, rodadas juntas (ms) | n/d | informativo | n/d |
| Faltas não concluídas (failed / cancelled / pending_at_end) | n/d | = 0 | n/d |

Motivos:
- rodada 1: telemetria ausente
- rodada 2: telemetria ausente
- rodada 3: telemetria ausente
- rodada 4: telemetria ausente
- rodada 5: telemetria ausente
