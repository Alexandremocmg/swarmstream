# Origem das skills

Skills de terceiros instaladas neste projeto em 2026-10-17 (31 no total). Cada pasta é uma cópia da pasta original; a única alteração está indicada abaixo da tabela. As licenças completas estão em [_licencas/](_licencas/).

| Repositório | Commit | Licença | Skills copiadas |
| --- | --- | --- | --- |
| [obra/superpowers](https://github.com/obra/superpowers) | `bb92a77` (2026-10-09) | MIT | test-driven-development, systematic-debugging, verification-before-completion, writing-plans, requesting-code-review, receiving-code-review |
| [jame581/GodotPrompter](https://github.com/jame581/GodotPrompter) | `bd9d180` (2026-10-14) | MIT | gdextension, godot-testing, godot-optimization, godot-debugging, godot-project-setup, multithreading, assets-pipeline, export-pipeline, addon-development |
| [jeffallan/claude-skills](https://github.com/jeffallan/claude-skills) | `36f3fe8` (2026-10-16) | MIT | cpp-pro, architecture-designer, python-pro |
| [mohitmishra786/low-level-dev-skills](https://github.com/mohitmishra786/low-level-dev-skills) | `bdc5847` (2026-06-27) | MIT | cmake, conan-vcpkg, static-analysis, concurrency-debugging, memory-model, flamegraphs, sanitizers |
| [trailofbits/skills](https://github.com/trailofbits/skills) | `5c99c03` (2026-10-15) | CC BY-SA 4.0 | modern-cpp, property-based-testing, harness-writing, libfuzzer |
| [alirezarezvani/claude-skills](https://github.com/alirezarezvani/claude-skills) | `19392f7` (2026-08-26) | MIT | boost-asio-pro (pasta `engineering/boost-asio-pro`) |
| [intense-visions/harness-engineering](https://github.com/intense-visions/harness-engineering) | `cb05e60` (2026-10-16) | MIT | db-acid-in-practice (pasta `agents/skills/claude-code/db-acid-in-practice`) |

**Alteração em relação à origem:** a `db-acid-in-practice` veio sem o cabeçalho `name`/`description` que o Claude Code usa para carregar a skill (os dados estavam no `skill.yaml`). Adicionei esse cabeçalho no topo do `SKILL.md`; o restante é idêntico. As demais skills estão sem alterações.

## Revisão feita antes de instalar
- Busca por instruções suspeitas (pedir para ignorar regras, enviar dados, baixar e executar código, mexer em configurações): nada encontrado. Os `curl` que aparecem são exemplos de documentação (baixar libpng para um exemplo de fuzzing, instalar o Butler do itch.io).
- Únicos arquivos executáveis: `systematic-debugging/find-polluter.sh` (procura qual teste deixa arquivos sobrando) e um exemplo `.ts`. Nenhum roda sozinho.
- **Não instaladas de propósito:** `brainstorming` e `executing-plans` do superpowers (trazem scripts e servidor local); `c-review` da Trail of Bits (depende do plugin completo, com agentes e workflows; instalar como plugin se for usar).

## Para atualizar
Clonar o repositório de novo, comparar a pasta com `diff -r`, revisar as mudanças e copiar. Atualizar a tabela acima com o commit novo.
