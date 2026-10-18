# Teste do critério de 1 hora (F0-13, item 4 do portão da Fase 0)

**Critério:** pessoa nova compila e roda os testes em **< 1 h** seguindo só a seção "Ambiente de desenvolvimento" do [CONTRIBUTING](../../CONTRIBUTING.md).

## Estado: NÃO EXECUTADO com pessoa externa
Falta a parte que prova o critério: alguém que **não montou o ambiente** seguindo só o guia, com cronômetro. O pré-teste abaixo foi feito pelo autor do guia e **não vale** como resultado do critério.

## Protocolo (quem aplica o teste)
1. Máquina: Windows 11 ou Ubuntu 22.04 **sem** CMake, compilador, vcpkg, Godot ou Python do projeto instalados (VM limpa ou usuário novo). Anote SO, CPU, RAM, disco e velocidade da rede.
2. Participante: nunca montou este ambiente; não vê este arquivo nem o histórico do guia. Recebe só a URL do repositório e o link do CONTRIBUTING.
3. Cronômetro começa ao abrir o guia e **para** quando `ctest` imprime `100% tests passed` (meta do critério). Os passos opcionais (Godot, assets de 2,8 GB, baseline) são cronometrados à parte e **não** entram na meta.
4. O observador **não ajuda**. Se o participante travar por mais de 5 min, anote o passo e o que ele tentou; depois, dê a dica mínima e marque o tempo como "com ajuda".
5. Registre cada passo que falhou, confundiu ou exigiu algo que o guia não diz. Corrija o guia no mesmo PR.

## Registro (preencher)
| Campo | Valor |
| --- | --- |
| Data | |
| Participante (função, experiência com C++/CMake) | |
| Observador | |
| Máquina / SO / rede | |
| Início → `ctest` verde | |
| **Tempo total até compilar e testar** | ___ min (meta < 60) |
| Resultado | ☐ passou (< 60 min, sem ajuda) · ☐ passou com ajuda · ☐ falhou |

| Passo do guia | Tempo | Falhou/confundiu? | O que aconteceu | Correção no guia |
| --- | --- | --- | --- | --- |
| 1 Clonar | | | | |
| 2 Compilador, CMake, Ninja | | | | |
| 3 vcpkg (opcional) | | | | |
| 4 Compilar e testar | | | | |
| 5 Python e lint | | | | |
| 6 Godot (opcional) | | | | |
| 7 Vetores | | | | |

## Pré-teste do autor (2026-10-17, Windows 11, MinGW/WinLibs) — não é o teste do critério
Executado pelo autor, em máquina que já tinha tudo instalado, então **nenhum tempo é válido**.

| Passo | Executado? | Resultado |
| --- | --- | --- |
| 4 Compilar e testar (`cmake -G Ninja`, build, `ctest`) | sim, com MinGW | passou (100%) depois de corrigir o Defender e o `0xc0000139` — ver abaixo |
| 5 Lint (black, ruff, mypy) e `benchmark.py --size-mib 64` | sim | passou; o benchmark leva ~70 s |
| 6 Rota do Godot 4.7.2 com `--robot --measure` | sim (via `baseline.py`) | passou. Download e conferência do SHA-512 do Godot: **não executados** |
| 7 `verify_vectors.py` | sim, Python 3.14 e 3.12 | passou (111 checks; 105 com `--skip-zstd`) |
| 2 MSVC, Ubuntu; 3 vcpkg; `winget install` de CMake/Ninja; `download_assets.py` do zero | **não** | sem evidência: o CI (Windows/MSVC e Linux) é o único que exercita MSVC e Linux, e ainda não rodou |

### Passos do guia que falharam no pré-teste
1. **MinGW, erro 225 ao compilar:** Defender pôs `g++.exe`/`c++.exe` em quarentena (`Trojan:Win32/Bearfoos.A!ml`). Resolvido com `winget upgrade` do WinLibs (r2). Já no guia (passo 2).
2. **MinGW, teste sai com `0xc0000139`:** `libstdc++-6.dll` de outro MinGW (MSYS2) no PATH. Corrigido no `CMakeLists.txt` (link estático). Já no guia.
3. **Passo 7 em Python 3.11/3.12 falhava com traceback** (sem decodificador Zstd). Agora o verificador diz o que fazer (`pip install zstandard`, Python 3.14+ ou `--skip-zstd`). **Corrigido no passo 7 do CONTRIBUTING** (Python 3.14+ ou `zstandard`).
4. **Passo 7 exige `openssl` no PATH**, que no Windows não existe por padrão (Git for Windows traz em `usr\bin`). O verificador avisa e o passo 7 do guia agora menciona.
5. **`verify_vectors.py` leva ~2 min**; o passo 7 agora avisa.
