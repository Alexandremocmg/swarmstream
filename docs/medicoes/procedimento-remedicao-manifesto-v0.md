# Procedimento: remedir o manifesto v0 com o libsodium real (F1-04)

**Estado: escrito, NÃO executado.** O dono combina o horário. Até esta medição ser feita e aprovada, os três
critérios abaixo da F1-04 estão **pendentes (não aprovados)**. Os números de [manifesto-v0.md](manifesto-v0.md)
são **históricos**: foram medidos com o Ed25519 e o SHA-256 próprios do `sw-pack`, que já não existem.

## O que se mede e contra o quê
| # | Critério | Meta | Fonte |
| --- | --- | --- | --- |
| 1 | **Tempo de abertura completa** do manifesto de 150 GB (1,2 M blocos): cabeçalho, chave, assinatura, `min_reader`, `manifest_id`, verificador e validações do §8, com o arquivo já na memória | **≤ 300 ms** (se passar, reabre-se a escolha do FlatBuffers) | [ADR-0012](../adr/0012-serializacao-do-manifesto.md), "Verificação" |
| 2 | **Tamanho do arquivo** do manifesto de 150 GB | **< 50 MiB** | [03 §7.3](../03-formato-de-conteudo.md#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo) |
| 3 | **Memória**: residente do manifesto aberto e **pico adicional da verificação** (o libsodium assina mensagem contígua, então `ManifestView::open` copia `prefixo ‖ cabeçalho ‖ payload`, ≈ o tamanho do arquivo) | residente do manifesto < 50 MiB; o pico extra é reportado, sem meta numérica | ADR-0012, item 6 |

O critério 2 já se sabe que não passa (61,6 MiB); ele é medido de novo só para confirmar o número com o código atual.
A **residência depois de uma rota completa** (mapear e devolver as páginas ao sistema) é do `sw-core` e **não** faz parte
desta medição.

## 1. Preparar a máquina ("máquina parada")
Regra do projeto: benchmark de desempenho roda **um de cada vez, com a máquina parada**.
1. Máquina na tomada, plano de energia "Alto desempenho" (Windows) ou governador `performance` (Linux).
2. Fechar navegador, editores, **outras sessões do Claude Code**, torrents, sincronizadores (OneDrive etc.) e qualquer
   compilação ou teste em andamento. Não rodar nada em paralelo durante a medição.
3. Não desligar o antivírus (é proteção); basta não deixar varredura rodando. Se o antivírus tiver exclusão por pasta
   para o diretório de build, pode usá-la e **anotar no relatório**.
4. Esperar a carga de CPU ficar abaixo de 5% por 2 minutos seguidos e anotar o valor:
   `powershell -NoProfile -Command "(Get-CimInstance Win32_Processor).LoadPercentage"` (Windows) ou `uptime` (Linux).
5. Anotar: modelo da CPU (e se tem SHA-NI), RAM, SO e versão, e o **commit** do `sw-pack` (`git rev-parse HEAD`).

## 2. Compilar com o libsodium REAL (nunca com um shim)
É o mesmo caminho do CI: toolchain do vcpkg (o `vcpkg.json` declara `libsodium` com `builtin-baseline`).
```bash
# Linux/macOS/Git Bash com o vcpkg instalado
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-rel --target sw-manifest-bench sw-pack-manifest-tests
```
```powershell
# Windows (terminal x64 Native Tools do MSVC)
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON `
      -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build-rel --target sw-manifest-bench sw-pack-manifest-tests
```
Antes de medir, **os testes têm de passar** (senão a medição não vale):
```bash
ctest --test-dir build-rel -R sw-pack --output-on-failure
```
Confirme no relatório que o `sw-manifest-bench open` imprime a linha `libsodium <versão>` e que **não** é `shim-...`.

## 3. Gerar os manifestos (uma vez; o arquivo é determinístico)
```bash
B=build-rel/sw-pack/sw-manifest-bench        # no Windows: build-rel\sw-pack\sw-manifest-bench.exe
mkdir -p medicao && $B generate --blocks 24576   --out medicao/m3.bin      # mapa de 3 GB
$B generate --blocks 1200000 --out medicao/m150.bin                         # 150 GB (≈ 165 GiB crus)
```
Guarde a saída: ela traz o **tamanho do arquivo** (critério 2).

## 4. Medir (5 processos por arquivo, 9 execuções cada)
Cada processo é uma invocação nova: a memória (pico) começa limpa, e a primeira execução de cada uma aquece o arquivo.
```bash
for i in 1 2 3 4 5; do $B open medicao/m150.bin --runs 9; sleep 10; done | tee medicao/saida-m150.txt
for i in 1 2 3 4 5; do $B open medicao/m3.bin   --runs 9; sleep 10; done | tee medicao/saida-m3.txt
```
Cada invocação imprime: versão do libsodium; `memoria (MiB): base -> depois de ler -> depois de abrir (pico)`;
`residente do manifesto`; `abertura completa (mediana de 9)` e as partes (assinatura do libsodium, SHA-256 do `manifest_id`,
verificador e validações).
- **Descarte e repita** o conjunto inteiro se qualquer invocação mostrar "resto" **negativo** ou uma mediana mais de 25% acima
  das outras quatro (sinal de interferência); anote que descartou e por quê.
- Se a carga de CPU subir durante a medição, pare e recomece.

## 5. Como calcular e comparar
Para o manifesto de 150 GB (`saida-m150.txt`), com os 5 valores de cada grandeza:
| Grandeza | Como obter | Comparar com |
| --- | --- | --- |
| **Abertura completa** | mediana das 5 medianas; reporte também o mínimo e o máximo dos 5 | critério 1: **≤ 300 ms**. Passa só se a **mediana das medianas** e o **máximo** ficarem ≤ 300 ms. |
| Partes da abertura | mediana das 5 medianas de cada linha (assinatura, SHA-256, resto) | só para explicar; o critério é o total |
| **Tamanho do arquivo** | linha `manifesto` do `generate` | critério 2: **< 50 MiB** |
| **Memória residente do manifesto** | `residente do manifesto (aberto - base)` (mediana das 5) | critério 3: **< 50 MiB** |
| **Pico extra da verificação** | `pico − depois de ler` (mediana das 5): é o que a cópia contígua do payload e os vetores temporários de `open` somam acima do arquivo já lido | sem meta; reporte e compare com o tamanho do arquivo (esperado ≈ 1×) |

Como medir a memória: o `sw-manifest-bench` lê o **conjunto de trabalho** do processo (`GetProcessMemoryInfo`, Windows)
ou `VmRSS`/`VmHWM` (Linux): "base" antes de ler, "depois de ler" com o arquivo inteiro em um `std::vector`, "depois de abrir"
com o manifesto aberto, e "pico" é o máximo do processo (inclui a cópia da mensagem feita pelo libsodium). Para isolar o
efeito da cópia, calcule `pico − depois de ler`; a cópia temporária é liberada ao fim da verificação, então `depois de abrir`
tende a não a incluir (o sistema pode demorar a devolver as páginas; por isso o que vale é o pico). No Windows o pico é `PeakWorkingSetSize`; no Linux, `VmHWM`.

## 6. O que reportar (e onde)
Criar `docs/medicoes/manifesto-v0-remedicao-AAAA-MM-DD.md` com:
1. **Identificação:** data, commit do `sw-pack`, **versão do libsodium** (saída do bench), compilador e flags
   (`CMAKE_BUILD_TYPE=Release`), CPU (com/sem SHA-NI), RAM, SO, carga de CPU antes de medir, o que foi fechado.
2. **Tabela** das grandezas acima, com mínimo, mediana e máximo das 5 invocações, para 3 GB e 150 GB.
3. **Comparação** com os números históricos de [manifesto-v0.md](manifesto-v0.md) (705 ms, 61,6 MiB, 63 MiB residentes) e
   com os três critérios, dizendo **passa / não passa** para cada um, sem arredondar a favor.
4. As saídas brutas (`saida-m150.txt`, `saida-m3.txt`) coladas ou anexadas, e o que foi descartado e por quê.
5. Se o critério 1 **não** passar: o que domina o tempo (SHA-256 do `manifest_id`? assinatura? validações?) e as saídas
   possíveis (hashes em duas threads; não recalcular o `manifest_id` a cada início; baixar o teto de 256 MiB e a cópia contígua;
   03 §7.3, saída (b), hash de bloco de 16 B). A **escolha** é do dono, por ADR com o número medido.
6. Atualizar o cartão da F1-04 (Evidência e "Critérios pendentes") no PR que trouxer o relatório; **só o dono** aceita os
   critérios.

## 7. Limites conhecidos desta medição
- Manifesto **sintético** (a forma do mapa da F0-09, não o mapa real: os assets não são versionados).
- O arquivo já está na memória (não conta ler do disco).
- É uma máquina só; o runner do CI não serve para isto (carga e hardware variam).
