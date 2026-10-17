# Baseline da bancada (F0-12)

Rota gravada do modo robô, Godot 4.7.2-stable, GL Compatibility, vsync desligado, GPU NVIDIA GTX 1050 Ti (a mesma nas rodadas), assets no SSD local.
Comando (180 s de rota, 1 aquecimento descartado, 5 medidas, 2 de cache frio):

    python sw-godot/tools/baseline.py --godot <godot_console.exe> --route-seconds 180 --runs 5 \
        --cold 2 --evict-dir <pasta grande sem relação> --evict-gb 13

Dados brutos em `baseline.json`.

| Rodada | carga (ms) | FPS | quadros >33 ms (após 3 s) | tempo acima de 33 ms (ms) | p99 (ms) | pior (ms) |
|---|---|---|---|---|---|---|
| 1 | 22220 | 533,8 | 3 | 601 | 3,46 | 354 |
| 2 | 21944 | 537,6 | 3 | 423 | 3,44 | 255 |
| 3 | 21959 | 546,6 | 3 | 503 | 3,34 | 346 |
| 4 | 20610 | 527,5 | 3 | 444 | 4,01 | 279 |
| 5 | 21011 | 501,6 | 3 | 362 | 4,74 | 180 |
| **média** | 21549 | 529,4 | 3 | 467 | 3,80 | — |
| **variação (máx−mín)/média** | **7,5%** | **8,5%** | 0% | 51% | 37% | 62% |

`route.log` idêntico nas 5 rodadas.

## Veredito
- **Carga e FPS: confiáveis** (7,5% e 8,5%, abaixo de 10%), mas a variação de FPS está perto do limite; rodada 5 (501,6) puxa o spread.
- **Stutter: a contagem é estável (3 quadros lentos em toda rodada), mas é só 3.** Rota de 180 s deu os mesmos 3–4 quadros que a de 60 s: não crescem com a duração. Uma contagem de 3 não resolve ±5%. A magnitude (tempo acima de 33 ms, 362–601 ms) varia 51% entre rodadas.
- **Critério da Fase 1 ("stutter ± 5% do baseline") não é verificável nesta cena.** Sugestão: usar limite absoluto (ex.: quadros lentos após 3 s ≤ 3 + N e p99 ≤ baseline + 2 ms), decidido pelo sênior. O p99 de ~4 ms varia 1,4 ms em absoluto; a % engana.
- Hipótese não verificada: os 3 picos são eventos fixos da rota (ex.: primeiro uso de região/textura), não ruído aleatório.

## Carga a frio: não demonstrada
Rodadas frias: 21454 e 21518 ms, dentro da faixa das quentes (20610–22220). Duas leituras possíveis, que não consegui separar:
1. a expulsão do cache falhou (li 12 GB de uma pasta não relacionada, 0,96 GB a menos que o pedido; RAM de 19,9 GB), ou
2. a carga é limitada por decodificação de JPEG 8K (CPU), não por leitura do SSD.
Para conclusão sólida: medir após reiniciar o Windows (primeira execução) ou com ferramenta de limpeza do cache com admin. Se for (2), o "tempo de carga lendo do SSD" não mede I/O nesta cena.
