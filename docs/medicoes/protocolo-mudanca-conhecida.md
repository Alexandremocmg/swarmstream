# Protocolo: validar a bancada com uma mudança conhecida (F1-47)

**Estado: protocolo e script prontos; NENHUMA medição feita.** A medição só é possível depois do F1-27 (fonte HTTP) e do F1-20 (loader do Godot), porque hoje o jogo não lê pela rede e trocar o perfil não altera nada no baseline ([docs/17](../17-pedidos-semana-1.md)). É condição do Portão 1 ([docs/16](../16-briefing-fase-1a.md)) e a mitigação do risco R15 ([docs/12](../12-riscos.md)).

## O que se quer provar
Se o mesmo jogo, na mesma máquina, na mesma rota, lê por uma rede **mais lenta** (`adsl_10` contra `cabo_50`), a espera numa falta e/ou a carga têm de **piorar de forma coerente**. Se não piorarem, a bancada não mede o que deveria e o Portão 1 não pode ser decidido.

Valores esperados, só da rede simulada (`sw-netlab/server.py`, `NETWORK` em `compare_profiles.py`): espera de uma peça de 1 MiB = latência + 1 MiB / banda.

| Perfil | Banda | Latência | Espera esperada (1 MiB) |
| --- | --- | --- | --- |
| `cabo_50` | 50 Mbps | 25 ms | ≈ 192,8 ms |
| `adsl_10` | 10 Mbps | 60 ms | ≈ 898,9 ms |

Razão esperada ≈ **4,66×** (a perda de 1% do `adsl_10` aborta pedidos e acrescenta tentativas; por isso a razão medida pode ficar acima da esperada).

## Pré-condições (todas, antes de medir)
1. F1-27 e F1-20 integrados: o jogo lê **pela rede** (confirmar que `bytes_from_cdn` cresce durante a rota; senão, trocar o perfil não muda nada).
2. F1-30 gravando `read_miss_start` e `read_miss` e F1-33 produzindo o arquivo da rodada ([docs/08](../08-metricas-e-testes.md#espera-do-jogo-numa-falta)).
3. Mínimo de amostras definido pelo F1-48 (R1). Enquanto não existir, `compare_profiles.py` **não** dá `coerente`: sem `--min-samples` o veredito é `incompleto`. Para um teste provisório, informe o valor explicitamente e registre que é provisório.
4. Máquina parada, a mesma para os dois perfis, a mesma GPU (conferir o campo `renderer` de cada rodada), sem outras sessões nem builds rodando, na tomada. A data e o horário são combinados com o dono do produto.

## Procedimento (A-B-A: o controle de deriva)
Mesma máquina, mesma build, mesma rota, um perfil de cada vez, **uma rodada de aquecimento descartada** por perfil:

1. `cabo_50`, N rodadas (conjunto A).
2. `adsl_10`, N rodadas (conjunto B).
3. `cabo_50` de novo, N rodadas (conjunto A′, controle: se A′ difere muito de A, houve deriva da máquina e a comparação é inválida).

N ≥ 3 (a carga exige 3 rodadas); o número de faltas reunidas vem do F1-48. Comandos (os do jogo com a fonte HTTP serão definidos pelo F1-27/F1-20 e estão marcados `<…>`):

```bash
# servidor de rede simulada (um perfil por vez); o diretório de fixtures é o do build do mapa de teste
python sw-netlab/server.py --root <fixtures> --profile cabo_50 --port 8080     # depois adsl_10, depois cabo_50
# rodada do jogo lendo da rede (comando do robô com a fonte HTTP: a definir no F1-27/F1-20)
<comando do robô com --source http://127.0.0.1:8080> --route-seconds 60
```

Para cada rodada, gravar: `load_ms` e o bloco `telemetry` do [formato da rodada](../../sw-godot/tools/round_report.py) (deltas de `sw_stats`, `miss_start_events`, `miss_wait_us`, `failed`/`cancelled`/`pending_at_end`) e, por perfil, o objeto `environment`:

```text
"environment": {"machine": <hostname>, "gpu": <renderer da rodada>, "build": <git rev-parse HEAD>,
                "route_sha256": <sha256 do route.log>}
```
Os quatro campos têm de ser **idênticos** nos três conjuntos; senão a comparação é `incompleto`.

## Comparação
Monte `docs/medicoes/mudanca-conhecida-AAAA-MM-DD.json` com os perfis `cabo_50`, `adsl_10` e `cabo_50_controle` (o conjunto A′) e rode:

```bash
python sw-godot/tools/compare_profiles.py docs/medicoes/mudanca-conhecida-AAAA-MM-DD.json \
    --min-samples <F1-48> --out docs/medicoes/mudanca-conhecida-AAAA-MM-DD.md
# controle de deriva: A contra A′ tem de dar "nao_mede" (nada mudou)
python sw-godot/tools/compare_profiles.py docs/medicoes/mudanca-conhecida-AAAA-MM-DD.json \
    --fast cabo_50 --slow cabo_50_controle --min-samples <F1-48>
```

| Veredito | Significa | O que fazer |
| --- | --- | --- |
| `coerente` | o perfil lento piora a espera (razão do p50 ≥ 2,0×) e/ou a carga (razão ≥ 1,2×) e não melhora nenhuma métrica | Registrar e seguir; a bancada acompanha a rede |
| `nao_mede` | nada piora | **Não** confiar nos números do Portão 1. Investigar: o jogo lê mesmo pela rede? o prefetch esconde toda a espera? a espera está sendo medida no ponto certo? (risco R15) |
| `incoerente` | algo **melhora** no perfil lento (p50, p95, p99 ou carga) em **mais de 10%** (dentro de 10% é ruído) | Algo além da rede move os números (cache quente, deriva, outra GPU). Repetir com a máquina parada e conferir o controle A–A′ |
| `incompleto` | dado faltando ou inválido, ambientes diferentes, amostras abaixo do mínimo, ou mínimo não informado | Corrigir a causa listada em "Motivos e avisos" e refazer |

O controle A–A′ **não** deve dar `incoerente`; o esperado é `nao_mede` (as duas rodadas iguais). Se der `incoerente`, a máquina derivou e a comparação A–B não vale.

**Limiares são propostas** (razão da espera ≥ 2,0×, da carga ≥ 1,2×; ruído tolerado de 10%, o mesmo limite de variação da bancada; aviso se a razão passa de 3× a esperada) e **precisam ser confirmados pelo R1 e pelo dono antes da medição final**. Estão como opções do script (`--min-wait-ratio`, `--min-load-ratio`, `--noise`).

**Avaliação do R1 (F1-48):** razão da espera ≥ 2,0×, razão da carga ≥ 1,2× e aviso de surpresa em 3× ficam; o ruído **continua 10%**, e se o controle A–A′ variar mais de 10% em alguma métrica comparada a comparação é `incompleto`; o mínimo é **300 faltas por perfil**. Motivos em [docs/08](../08-metricas-e-testes.md#método-do-percentil-amostras-e-rodadas-comparáveis-f1-48). Valem depois do aceite do dono.

## Registro
Gravar em `docs/medicoes/`: o arquivo da rodada (`.json`), o relatório (`.md`), os comandos exatos usados, a máquina e a GPU, as versões (Godot, build) e a data. Sem o comando, a máquina e a data, o número não vale ([CLAUDE.md](../../CLAUDE.md)).
