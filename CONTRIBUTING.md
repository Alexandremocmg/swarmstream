# Como contribuir

## Antes de tudo
1. Leia [docs/01-visao-e-escopo.md](docs/01-visao-e-escopo.md), [docs/02-arquitetura.md](docs/02-arquitetura.md) e o [glossário](docs/glossario.md).
2. Veja o que se espera do seu nível em [docs/10-equipe-e-niveis.md](docs/10-equipe-e-niveis.md).
3. Escolha uma tarefa em [docs/11-backlog-inicial.md](docs/11-backlog-inicial.md). Chegando agora? Procure `boa-primeira-tarefa`.

## Ambiente de desenvolvimento (F0-13)

Meta: pessoa nova compila e roda os testes em **menos de 1 hora** seguindo só esta seção. Se um passo falhar ou estiver confuso, **anote o passo e corrija o guia no mesmo PR** (o resultado do teste de 1 hora fica em [docs/medicoes/](docs/medicoes/)).

### Versões
| Ferramenta | Versão | Para quê |
| --- | --- | --- |
| Git | 2.40+ | clonar, commits com `-s` (DCO) |
| Compilador C++20 | MSVC 2022 (17.x) · GCC 12+ · Clang 17+ | `sw-core`, `sw-pack` |
| CMake | 3.20 mínimo (o `CMakeLists.txt` exige); **3.28+ recomendado** | build |
| Ninja | qualquer recente | gerador usado no guia |
| vcpkg | commit atual do repositório oficial | dependências C++ (hoje o `vcpkg.json` não lista nenhuma, então é opcional até a F1) |
| Python | **3.11+** | `sw-netlab`, `sw-godot/tools`, lint |
| Godot | **4.7.2-stable** (a versão está em `sw-godot/.godot-version`) | mapa de teste e rota do robô |

Docker **não** é necessário hoje: o `sw-netlab` roda com Python puro.

### 1. Clonar
```bash
git clone <URL do repositório> swarmstream
cd swarmstream
```

### 2. Compilador, CMake e Ninja
**Windows (recomendado, é o que o CI usa):** instale o *Visual Studio 2022 Build Tools* com a carga de trabalho "Desenvolvimento para desktop com C++", depois CMake e Ninja:
```powershell
winget install Kitware.CMake Ninja-build.Ninja
```
Abra o **"x64 Native Tools Command Prompt for VS 2022"** (ou rode `vcvars64.bat`) para que o `cl` esteja no PATH.

*Alternativa MinGW (WinLibs):* `winget install BrechtSanders.WinLibs.POSIX.UCRT`. Dois cuidados reais já encontrados:
- O Windows Defender pode pôr `g++.exe`/`c++.exe` em quarentena (`Trojan:Win32/Bearfoos.A!ml`, detecção heurística). Sintoma: `CreateProcess failed ... GetLastError() = 225`. Atualize o pacote (`winget upgrade BrechtSanders.WinLibs.POSIX.UCRT`) e confira o hash antes de restaurar qualquer arquivo; exclusões do Defender são decisão sua.
- Se houver outro MinGW no PATH (ex.: MSYS2), os testes podem sair com `0xc0000139`. O `CMakeLists.txt` já liga `libgcc`/`libstdc++` estaticamente para evitar isso.

**Ubuntu 22.04+:**
```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git python3 python3-venv curl zip unzip pkg-config
```

Confira: `cmake --version` (≥ 3.20) e `c++ --version` (ou `cl` no Windows).

### 3. vcpkg (opcional até a F1)
```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics          # Windows: bootstrap-vcpkg.bat
export VCPKG_ROOT=~/vcpkg                           # Windows: setx VCPKG_ROOT C:\vcpkg (novo terminal)
```
O manifesto é o `vcpkg.json` na raiz; o toolchain é passado ao CMake (passo 4). Não rode `vcpkg install` à mão: o modo manifesto instala sozinho no configure.

### 4. Compilar e testar
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
Com vcpkg, acrescente `-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"` ao primeiro comando. Resultado esperado: `100% tests passed`.

Sanitizers (só Linux/GCC/Clang, é o que o CI roda): `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSANITIZE=ON`.

### 5. Python (bancada, ferramentas e lint)
```bash
python3 -m venv .venv                 # Windows: python -m venv .venv
. .venv/bin/activate                  # Windows: .venv\Scripts\Activate.ps1
python -m pip install black ruff mypy
python sw-netlab/benchmark.py --size-mib 64     # a bancada de rede passa quando termina sem erro
```
Lint igual ao do CI:
```bash
python -m black --config sw-netlab/pyproject.toml --check sw-netlab sw-godot/tools/download_assets.py
python -m ruff check --config sw-netlab/pyproject.toml sw-netlab sw-godot/tools/download_assets.py
python -m mypy --strict sw-netlab
```

### 6. Godot 4.7.2 e o mapa de teste (opcional para compilar; necessário para medir)
1. Baixe o **Godot 4.7.2-stable** (versão padrão, não .NET) da página oficial de versões do Godot e confira o SHA-512 com o arquivo `SHA512-SUMS.txt` da mesma versão. Descompacte fora do repositório (os `.exe` são ignorados pelo git).
2. Os assets (≈ 2,8 GB, CC0, Poly Haven) **não** estão no git: `python sw-godot/tools/download_assets.py --resolution 8k` baixa e valida cada MD5.
3. Rota do robô (abre uma janela; rode no console do Godot):
```bash
godot --path sw-godot/project -- --robot --measure --route-seconds 8
```
O fim da rota imprime `ROUTE_DONE`. Baseline completo: `python sw-godot/tools/baseline.py --godot <godot_console> --route-seconds 60` (relatório em [docs/medicoes/baseline.md](docs/medicoes/baseline.md)).

### 7. Vetores de teste do formato (`sw-pack/testdata`)
Opcional para compilar; leva ~2 min. Precisa de `openssl` 3.x no PATH (no Windows, o Git for Windows traz em `<pasta do Git>\usr\bin`) e de um decodificador Zstd: **Python 3.14+** ou `python -m pip install zstandard`.
```bash
python sw-pack/testdata/verify_vectors.py              # confere vectors-v0.json com implementação independente
python sw-pack/testdata/verify_vectors.py --skip-zstd  # sem decodificador Zstd (Zstd fica só parcialmente verificado)
```
Detalhes e como regenerar: [sw-pack/testdata/README.md](sw-pack/testdata/README.md).

### Problemas conhecidos
| Sintoma | Causa | Saída |
| --- | --- | --- |
| `GetLastError() = 225` ao compilar com MinGW | Defender pôs o `g++.exe` em quarentena | passo 2 (MinGW) |
| Teste sai com `0xc0000139` | DLL do `libstdc++` de outro MinGW no PATH | já tratado no `CMakeLists.txt`; reconfigure em pasta nova |
| `cl` não encontrado | terminal sem o ambiente do Visual Studio | abra o "x64 Native Tools Command Prompt" |

## Fluxo de trabalho
1. Crie um branch a partir de `main`: `tipo/ID-descricao-curta` (ex.: `feat/F1-14-remocao-clock`).
2. Commits pequenos, mensagem no formato `F1-14: implementa marcas d'água da remoção`.
3. Abra o PR cedo (rascunho) e marque o dono do módulo (CODEOWNERS).
4. O PR precisa de: CI verde, testes para o que mudou, doc atualizado se o comportamento mudou.
5. Código marcado `critico` precisa de aprovação de um sênior ou do arquiteto.

## Regras de código
- Nenhuma falha silenciosa: todo erro é retornado ou registrado. **Confira todo valor de retorno de I/O.**
- Nada de alocação no caminho quente de leitura.
- Dado vindo da rede ou de peer é não confiável até ser verificado.
- Toda funcionalidade nova publica as métricas que a provam ([docs/08-metricas-e-testes.md](docs/08-metricas-e-testes.md)).

## Decisões
Mudança de arquitetura ou de contrato = **ADR novo** em [docs/adr/](docs/adr/README.md), revisado antes do código.

## Licença, DCO e cabeçalhos
O núcleo do Swarmstream é open source sob a **Apache-2.0** ([LICENSE](LICENSE), [ADR-0008](docs/adr/0008-licenca-open-source.md)). Ao contribuir, você envia seu código sob essa licença (seção 5 da Apache-2.0).

### DCO: assine cada commit
Usamos o **Developer Certificate of Origin 1.1** ([ADR-0015](docs/adr/0015-politica-de-contribuicao-dco.md)); não há CLA. Assinar um commit é declarar que você tem o direito de enviar aquele código sob a licença do projeto (texto completo em <https://developercertificate.org/>).

```bash
git commit -s -m "F1-14: implementa marcas d'água da remoção"
```

Isso acrescenta `Signed-off-by: Seu Nome <seu@email>` à mensagem, com o mesmo nome e e-mail do autor do commit. O CI recusa PR com commit sem essa linha. Esqueceu? Corrija o último commit com `git commit --amend -s --no-edit`, ou todos os commits do branch com `git rebase --signoff main`, e envie de novo com `git push --force-with-lease`.

### Cabeçalho de arquivo
Todo arquivo de código novo (C, C++, Go, Rust, Python, CMake, scripts) começa com:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
```

Use o comentário da linguagem (`#` em Python, CMake e shell). Não é preciso listar seu nome: a autoria fica no histórico do git.

### Código e material de terceiros
- Não copie código de terceiros sem registrar origem e licença no [NOTICE](NOTICE), no mesmo PR.
- Só entram licenças compatíveis com Apache-2.0 (MIT, BSD, BSL-1.0, Apache-2.0, zlib). GPL/AGPL/LGPL em código do núcleo: não. Na dúvida, pergunte ao arquiteto antes de escrever código.
- As skills em `.claude/skills/` mantêm as licenças de origem (MIT e CC BY-SA 4.0, ver [NOTICE](NOTICE)). Não copie trechos delas para o código ou para a documentação do projeto.

## Segurança
Falhas de segurança: **nunca** em issue pública. Reporte em privado, conforme o [SECURITY.md](SECURITY.md).
