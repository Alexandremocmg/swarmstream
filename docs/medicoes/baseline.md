# Baseline da bancada (F0-12), método corrigido

Rota gravada do modo robô, Godot 4.7.2-stable, GL Compatibility, vsync desligado, GTX 1050 Ti, assets em NVMe (Kingston SNV3S1000G). Itens corrigidos em relação à versão anterior (item 5 do fechamento da Fase 0, [15](../15-portao-fase-0-e-plano-3-recursos.md)):

1. **Texturas em formato GPU comprimido** (era JPEG 8K decodificado na CPU).
2. **Limites absolutos de stutter** (era "± 5% do baseline").
3. **Carga a frio**: tentativa por esvaziamento de cache feita (abaixo); **a carga com reinício do Windows ainda não foi feita** (ver "Pendente").

Dados brutos: [baseline-gpu.json](baseline-gpu.json) (quente, método novo), [baseline-gpu-evict.json](baseline-gpu-evict.json) (frio por esvaziamento), [baseline-jpg.json](baseline-jpg.json) (método antigo, para comparação), [baseline-limits.json](baseline-limits.json) (limites). O resultado da versão anterior ficou em [baseline.json](baseline.json).

## Como reproduzir

    # 1. gerar as texturas GPU (BC1/S3TC com mipmaps, 16 arquivos de 42,7 MiB = 683 MiB), ~1 min
    godot --headless --path sw-godot/project --script res://scripts/bake_gpu_textures.gd
    # 2. baseline quente: 1 aquecimento descartado + 5 rodadas de 60 s; grava os limites
    python sw-godot/tools/baseline.py --godot <godot_console.exe> --tex gpu --route-seconds 60 --runs 5 \
        --out docs/medicoes/baseline-gpu.json --limits-out docs/medicoes/baseline-limits.json
    # 3. conferir rodadas contra os limites (sai com código 1 se violar); a carga exige 3 rodadas
    python sw-godot/tools/baseline.py --godot x --check r1.json r2.json r3.json --limits docs/medicoes/baseline-limits.json

`--tex jpg` repete o método antigo. A carga é medida em três partes: `texture_read_ms` (só ler os bytes do disco), `texture_load_ms` (ler + interpretar + enviar à GPU) e `load_ms` (cena inteira).

## 1. Texturas GPU: a carga deixou de ser decodificação de JPEG

| Método | Carga (`load_ms`) | Quadros > 100 ms | Pior quadro |
| --- | --- | --- | --- |
| JPEG 8K → `Image` na CPU (antigo), 3 rodadas | 26,1 / 29,9 / 33,0 s | 3 em toda rodada | 132–554 ms |
| BC1 pré-comprimido `.res` (novo), 5 rodadas | 2,30 / 2,41 / 2,72 / 2,82 / 2,55 s | 0 | 20–34 ms |

A carga caiu mais de 10×. Isso confirma a hipótese do parecer: **no método antigo a carga era limitada pela CPU (decodificação de JPEG), não pelo disco**, e por isso a carga "a frio" não mudava. Os 3 quadros lentos que a rota tinha também sumiram (provavelmente o envio das texturas 8K sem compressão à GPU; não isolei a causa). (A carga JPEG desta rodada ficou mais lenta que as 21,5 s anteriores e variou 23%: a máquina estava mais carregada. O que vale é a ordem de grandeza.)

## 2. Baseline quente do método novo (5 rodadas de 60 s)

| Rodada | carga (ms) | leitura pura (ms) | FPS | quadros > 33 ms (após 3 s) | quadros > 100 ms | p99 (ms) | pior (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 2296 | 424 | 506,6 | 0 | 0 | 3,7 | 27,0 |
| 2 | 2411 | 450 | 505,3 | 0 | 0 | 3,8 | 22,8 |
| 3 | 2719 | 499 | 499,4 | 1 | 0 | 4,0 | 33,8 |
| 4 | 2819 | 499 | 483,9 | 0 | 0 | 3,9 | 20,5 |
| 5 | 2550 | 454 | 504,6 | 0 | 0 | 4,2 | 22,6 |

`route.log` idêntico nas 5 rodadas. Variação (máx−mín)/média: carga **20,4%**, leitura pura 16,1%, FPS 4,5%.

**Veredito do rigor:** pela regra antiga (variação < 10% na carga e no FPS) a carga **não é confiável** neste método: a variação absoluta é de 0,52 s, mas sobre uma base de 2,5 s ela pesa 20%. O FPS está confiável (4,5%). Em vez de relaxar o limite em silêncio, a comparação da carga passa a usar tolerância absoluta (seção 3). A carga também mostrou tendência de subir nas rodadas 3–4 e voltar: ruído do sistema, não explicado.

## 3. Limites absolutos (substituem "stutter ± 5%"): critério do Portão 1

Calculados por `baseline.py` a partir das rodadas quentes e gravados em [baseline-limits.json](baseline-limits.json). Propostos pelo R2; **aprovados com ajustes pelo R1 em 2026-10-18**.

| Métrica | Proposta R2 | **Aprovado** | Regra aprovada |
| --- | --- | --- | --- |
| Quadros > 33 ms após 3 s, **por rodada** de 60 s | ≤ 4 | ≤ **2** | pior do baseline (1) + 1. Derivado: 1 quadro lento em 300 s de baseline (taxa ≈ 0,2 por rodada); por Poisson, uma rodada sem regressão passa de 2 em ~0,1% das vezes. ≤ 4 deixaria passar 3 travadas por minuto a mais que o SSD |
| Quadros > 100 ms, por rodada | ≤ 1 | **0** | pior do baseline (0) + 0. Um congelamento de 100 ms é o defeito que o produto promete não ter. Se aparecer, investigar a causa (evento `frame_stutter`), não repetir a rodada até passar |
| p99 do tempo de quadro, por rodada | ≤ 6,24 ms | ≤ **6,24 ms** | pior p99 do baseline (4,24) + 2 ms: o que o `sw-core` pode tomar de CPU (12% de um quadro a 60 FPS) |
| Carga (`load_ms`), quente com quente | ≤ 3059 ms | ≤ **3059 ms**, pela **mediana de 3 rodadas** | média (2559) + max(10%, 500 ms). Uma rodada sozinha não decide: a carga varia 20% entre rodadas (seção 2) |

Regras de uso (R1):
- **Validade:** os limites valem para esta máquina e configuração (GTX 1050 Ti, NVMe, Godot 4.7.2, GL Compatibility, vsync desligado, rota de 60 s). Mudou hardware, driver, versão do Godot ou a rota: refazer o baseline antes de comparar.
- **O que eles provam:** que o `sw-core` com cache local cheio não piora o jogo frente ao SSD direto. Eles **não** medem o streaming: para isso o Portão 1 usa a taxa de falta e a espera p99 de [08](../08-metricas-e-testes.md) no perfil `cabo_50`.
- Sem piso de FPS: a 500 FPS ele não mede streaming, e o p99 já cobre perda de CPU.
- `texture_read_ms` fica registrado em toda rodada, sem limite: é o número de I/O que acompanha a decisão da seção 4.

Por que absolutos: com 0–1 quadros lentos por rodada, "± 5%" não tem significado (um quadro a mais é +100%). Os limites acima só acusam regressões reais de stutter, e um quadro de 100 ms já viola o limite de 1.

Pendências de método que continuam: (a) as margens de p99 (+2 ms) e de carga (500 ms) seguem escolhas de projeto, não derivadas de dados; as de quadros lentos agora são; (b) só 5 rodadas de uma máquina: o baseline deve ser refeito com 10 rodadas antes do Portão 1, e os limites recalculados pela mesma regra.

## 4. Carga a frio

### 4.1 Esvaziamento de cache (feito)

Antes de cada rodada fria, o script leu 16 GB de uma pasta não relacionada (sem falta de leitura: `evict shortfall 0 B`; RAM de 19,9 GB). Rota de 20 s, 2 rodadas quentes e 3 frias:

| Condição | carga (ms) | **leitura pura (ms)** |
| --- | --- | --- |
| Quente (esta rodada) | 2331 · 2377 | 437 · 438 |
| Quente (5 rodadas do item 2) | 2296 – 2819 | 424 – 499 |
| **Fria (após esvaziar)** | 3220 · 3520 · 2669 | **874 · 952 · 755** |

- **Leitura pura: a frio é sempre maior que a quente** (755–952 ms contra 424–499 ms, sem sobreposição; cerca de 2×). O esvaziamento funcionou e o I/O aparece quando isolado.
- **Carga total: a frio e a quente se sobrepõem** (a rodada fria 3 deu 2669 ms, dentro da faixa quente de 2296–2819). O acréscimo médio da carga fria é ~0,4–0,8 s sobre ~2,5 s.

**Conclusão do que vale hoje:** neste mapa (683 MiB de texturas GPU, em NVMe) a leitura do disco é cerca de 18% da carga a quente e 27% a frio; o resto é interpretar o recurso e enviar à GPU. Portanto **`load_ms` sozinho não mede I/O**; para streaming, o número a acompanhar é **`texture_read_ms`** (e, na Fase 1, o tempo de espera por falta do próprio cache). Se a carga fria por reinício também se sobrepuser à quente em `load_ms`, a conclusão a documentar é: *esta cena, neste disco, não mede I/O pela carga total*.

### 4.2 Reinício do Windows (pendente, preparado)

O esvaziamento por leitura é o método fraco que o parecer pediu para substituir. A medição definitiva é a primeira execução depois de reiniciar o Windows. Está pronta e **não foi executada**, porque reiniciar a máquina interrompe tudo o que estiver aberto e precisa de autorização do dono da máquina:

    sw-godot\tools\cold_boot.ps1 -Arm -Godot <godot_console.exe> -Python <python.exe>   # registra 1 execução no próximo logon (HKCU RunOnce)
    (reiniciar)                                                                        # ou -Arm -Reboot

No logon o script espera 120 s (a inicialização assenta), roda a **primeira** rodada (fria) com `baseline.py --after-boot`, depois 3 rodadas quentes, e grava `docs/medicoes/baseline-cold-boot-gpu.json` e `.log`. O JSON registra os segundos desde o boot, o que prova que a rodada foi a primeira. Limites desta medição: a inicialização do Windows ainda gera I/O de fundo depois de 120 s; por isso o resultado vale como ordem de grandeza e deve ser repetido uma vez.

## Veredito

- **Carga e stutter agora são medidos de forma útil**: a carga não é mais dominada por JPEG, os quadros lentos sumiram do baseline e há limites absolutos verificáveis (`--check`).
- **A carga total continua instável em termos relativos (20%)** porque o valor ficou pequeno (2,5 s); por isso o critério de carga é absoluto.
- **Esta cena ainda mede pouco I/O**: com 683 MiB em NVMe, a leitura pura é uma fração da carga. Para o Portão 1 será preciso (a) um conjunto de dados maior que a RAM, ou limitar a banda, e (b) olhar `texture_read_ms`/espera por falta, não `load_ms`.
- **Ainda não provado**: carga fria por reinício (4.2), BC7 (as texturas foram baixadas em BC1; BC7 dobra os bytes e não foi medido), e a repetição em outra máquina.
