# cmake

Utilitários CMake que não pertencem ao build normal.

## RedProbe.cmake

Existe só para provar o critério de aceite do F0-03 — *PR com teste falhando
fica vermelho* — sem precisar quebrar um teste de verdade:

```bash
ctest --test-dir build -S cmake/RedProbe.cmake --output-on-failure
echo $?   # precisa ser != 0
```

O script registra um teste que falha de propósito (`cmake -E false`). O CI
(`.github/workflows/ci.yml`) roda o `ctest` normal; essa sonda é ferramenta de
verificação manual/local.