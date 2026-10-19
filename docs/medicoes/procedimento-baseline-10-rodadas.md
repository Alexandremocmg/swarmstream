# Procedimento: baseline com 10 rodadas (F1-44)

**Estado: procedimento escrito; a medição NÃO foi iniciada e ninguém foi chamado a parar nada.** Quem escolhe o horário é o dono do produto; a gestão pede a pausa das sessões só depois da confirmação dele. Este texto é o roteiro, não um pedido.

## O que se quer
Refazer o baseline quente do método novo (texturas GPU) com **10 rodadas de 60 s** (hoje são 5, em [baseline.md](baseline.md)), na mesma máquina e configuração, e **recalcular os limites pela mesma regra já aprovada** (R1, 2026-10-18, [baseline.md §3](baseline.md#3-limites-absolutos-substituem-stutter--5-critério-do-portão-1)). Os limites em vigor ([baseline-limits.json](baseline-limits.json)) **só mudam com a aprovação do dono**: a medição só gera uma **proposta**.

Limites em vigor (não mudam por causa desta medição): quadros > 33 ms após 3 s ≤ 2 por rodada; quadros > 100 ms = 0; p99 do quadro ≤ 6,238 ms; carga ≤ 3059,05 ms pela mediana de 3 rodadas.

## Validade (R1): a configuração tem de ser a mesma do baseline
Se qualquer item abaixo mudou, o resultado não vale para comparar e o baseline é outro baseline (anote a diferença):

| Item | Valor do baseline atual |
| --- | --- |
| Máquina e GPU | a mesma; GTX 1050 Ti (o `renderer` de **toda** rodada tem de ser esse; o notebook também tem Intel HD 630) |
| Disco | NVMe (Kingston SNV3S1000G) |
| Godot | 4.7.2-stable, GL Compatibility, vsync desligado |
| Texturas | BC1 pré-comprimido, 16 arquivos (683 MiB), geradas por `bake_gpu_textures.gd` |
| Rota | a mesma, 60 s; o hash do `route.log` tem de ser idêntico nas 10 rodadas |
| Build | o commit da `main` no dia (registrar); só `baseline.py` e o projeto Godot importam |

## Máquina parada: o que precisa estar quieto
A medição mede desempenho; qualquer coisa pesada em segundo plano a contamina. Durante toda a janela:

1. **Sessões do projeto:** nenhuma sessão pode estar rodando build, teste, benchmark, Godot, `gh`/`git` com checkout grande ou `pip`. Em termos práticos, as sessões locais do Claude Desktop (hoje: **Planejador, Arquiteto, Sênior e Pleno**) ficam **ociosas** (sem turno em andamento), exceto a que executa o procedimento. A gestão confirma que ninguém está ocupado antes de liberar o início; sessões na nuvem e Remote Control offline não usam esta máquina.
2. **Aplicativos:** fechar navegadores com muitas abas, editores/IDEs, Docker Desktop (roda uma VM) e qualquer download ou sincronização (OneDrive, Google Drive). Notebook **na tomada**, plano de energia "Alto desempenho" (ou equivalente) e tela sem proteção de tela.
3. **Windows:** sem atualização em andamento e sem varredura do Defender rodando (conferir; **não** desativar o Defender).

Verificação de que está parada (antes de começar, e registrar os números no relatório). Os limiares são propostas. Os comandos usam contadores do CIM porque `Get-Counter` falhou nesta máquina (nomes de contador dependem do idioma do Windows):
```powershell
# CPU e disco: 12 amostras a cada 5 s (60 s); a média da CPU tem de ficar abaixo de 5% e a do disco abaixo de 5%
1..12 | ForEach-Object {
    $cpu = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
    $disk = (Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk -Filter "Name='_Total'").PercentDiskTime
    [pscustomobject]@{ Cpu = $cpu; Disco = $disk }
    Start-Sleep -Seconds 5
} | Measure-Object Cpu, Disco -Average
# GPU ociosa (utilização abaixo de 5%, memória usada ~0)
nvidia-smi --query-gpu=utilization.gpu,memory.used,driver_version --format=csv
# processos que mais consomem; confira se MsMpEng (Defender) ou outro está ativo
Get-Process | Sort-Object CPU -Descending | Select-Object -First 10 Name, CPU, WS
```
Se a CPU média passar de 5%, a GPU de 5%, ou o Defender (`MsMpEng`) estiver consumindo CPU (varredura), **não comece**: espere ou remarque. **Observação de 2026-10-19** (leitura de apoio, nada foi alterado): no momento em que este procedimento foi escrito havia o navegador Opera (33 processos), 3 processos de `VirtualBoxVM` e o `wslservice` em execução, e a CPU em 95% (as próprias sessões de trabalho). Tudo isso precisa estar fechado ou ocioso na janela.

## Duração estimada (estimativa, ainda não cronometrada)
| Etapa | Tempo |
| --- | --- |
| Conferir a máquina e registrar a configuração | ~5 min |
| 1 rodada de aquecimento (descartada) + 10 rodadas de 60 s (≈ 70–80 s cada com a carga) | ~13–15 min |
| Conferência e relatório | ~10 min |
| **Janela total a reservar** | **~30 min** |

## Comandos exatos
Na raiz do repositório, com o ambiente Python do projeto ativado (`.venv`). O Godot de console fica em `%TEMP%\swarmstream-godot-4.7.2\` (conferir que o arquivo existe antes: pasta temporária pode ter sido limpa).

```powershell
# 0. registrar a configuração
git rev-parse HEAD
& "$env:TEMP\swarmstream-godot-4.7.2\Godot_v4.7.2-stable_win64_console.exe" --version
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
python --version

# 1. (só se as texturas GPU ainda não existirem) gerar BC1, ~1 min
& "$env:TEMP\swarmstream-godot-4.7.2\Godot_v4.7.2-stable_win64_console.exe" --headless --path sw-godot/project --script res://scripts/bake_gpu_textures.gd

# 2. o baseline: 1 aquecimento descartado + 10 rodadas de 60 s; NÃO sobrescreve os arquivos aprovados
python sw-godot/tools/baseline.py `
    --godot "$env:TEMP\swarmstream-godot-4.7.2\Godot_v4.7.2-stable_win64_console.exe" `
    --tex gpu --route-seconds 60 --runs 10 `
    --out docs/medicoes/baseline-gpu-10.json `
    --limits-out docs/medicoes/baseline-limits-10-proposta.json
```
Enquanto isso roda, **nenhuma outra tarefa** na máquina. Se algo interferir (CPU alta, suspensão, janela de outro programa no foreground), anote a hora e **refaça a sessão inteira**; não descarte rodadas isoladas.

## Onde ficam os resultados (`docs/medicoes/`)
| Arquivo | Conteúdo |
| --- | --- |
| `baseline-gpu-10.json` | dados brutos das 10 rodadas (formato do `baseline-gpu.json`: `runs`, `spread_percent`, `route_log_identical`, `renderers`) |
| `baseline-limits-10-proposta.json` | limites recalculados pela mesma regra (**proposta, não substitui** `baseline-limits.json`) |
| `baseline-10-rodadas.md` | o relatório: configuração, verificação da máquina parada, tabela das 10 rodadas, comparação com os limites atuais, regra aplicada, comandos e data |

## Como comparar com os limites atuais
**1. Validade da sessão:** `route_log_identical = true`; um só `renderer` (GTX 1050 Ti) nas 10 rodadas; verificação de máquina parada anexada. Se qualquer item falhar, a sessão é inválida.

**2. Regra aplicada** (a mesma de [baseline.md §3](baseline.md#3-limites-absolutos-substituem-stutter--5-critério-do-portão-1), implementada em `derive_limits` e `LIMIT_MARGINS` de `baseline.py`; a proposta sai de `--limits-out`):
- quadros > 33 ms após 3 s: **pior das 10 rodadas + 1**;
- quadros > 100 ms: **pior das 10 rodadas + 0**;
- p99 do quadro: **pior p99 das 10 rodadas + 2 ms**;
- carga: `max(média × 1,10, média + 500 ms)`, conferida pela **mediana de 3 rodadas**.

**3. Tabela de comparação** (preencher no `baseline-10-rodadas.md`):

| Métrica | Limite em vigor | Pior das 10 rodadas | Limite proposto (mesma regra) | Muda? |
| --- | --- | --- | --- | --- |
| Quadros > 33 ms após 3 s, por rodada | ≤ 2 | | | |
| Quadros > 100 ms, por rodada | 0 | | | |
| p99 do quadro (ms) | ≤ 6,238 | | | |
| Carga, mediana de 3 rodadas (ms) | ≤ 3059,05 | (mediana e média das 10) | | |

**4. As 10 rodadas passam nos limites em vigor?** Separar as rodadas em arquivos e conferir com o `--check` do `baseline.py` (sai com código 1 se violar; a carga exige 3 rodadas por vez):
```powershell
New-Item -ItemType Directory -Force baseline-10-runs | Out-Null
$d = Get-Content docs/medicoes/baseline-gpu-10.json -Raw | ConvertFrom-Json
$utf8 = New-Object System.Text.UTF8Encoding $false   # sem BOM: o baseline.py lê JSON sem BOM
$i = 1; foreach ($run in $d.runs) {
    [System.IO.File]::WriteAllText("$PWD/baseline-10-runs/r$i.json", ($run | ConvertTo-Json -Depth 5), $utf8); $i++
}
foreach ($set in @(@(1,2,3), @(4,5,6), @(7,8,9), @(8,9,10))) {
    $files = $set | ForEach-Object { "baseline-10-runs/r$_.json" }
    python sw-godot/tools/baseline.py --godot x --check @files --limits docs/medicoes/baseline-limits.json
}
```
(A pasta `baseline-10-runs` é temporária; não vai para o git.)

**5. Se o pior valor novo passar do limite em vigor** (por exemplo, uma rodada com 3 quadros lentos): **não troque nenhum limite**. Registre o dado, investigue a causa (não repita a rodada até passar, como diz o `baseline.md`) e leve a pergunta ao dono.

## Depois da medição
- Abrir **um PR de documentação** com os três arquivos de `docs/medicoes/` e o cartão F1-44 em `AGUARDANDO_DONO`, com a proposta de limites e a comparação.
- **Mudar `baseline-limits.json` só em PR separado, depois do "aceito" do dono** no chat com a gestão. Os testes de `test_baseline.py` e de `test_round_report.py` usam esses valores; mude o arquivo, rode a suíte e atualize os números dos documentos que os citam (`docs/16`, `docs/08`, `baseline.md`) no mesmo PR.
- A carga fria com reinício do Windows é outra tarefa ([F1-40](../../tarefas/cartoes/F1-40.md)); **não** faz parte desta janela.
