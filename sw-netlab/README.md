# sw-netlab

Servidor HTTP/1.1 para fixtures de CDN, com suporte a `Range: bytes=...`, latência, banda e perda configuráveis.

```powershell
python server.py --root fixtures --profile cabo_50 --port 8080
python server.py --root fixtures --profile custom --bandwidth-mbps 20 --latency-ms 100 --loss-percent 2
python benchmark.py --size-mib 64
```

Perfis iniciais: `fibra_300` (300 Mbps, 10 ms, 0%), `cabo_50` (50 Mbps, 25 ms, 0,2%) e `adsl_10` (10 Mbps, 60 ms, 1%). **A banda do perfil é do enlace inteiro, não de cada conexão** (decisão do F1-53): um perfil simula a linha de uma casa, então 4 ou 8 conexões simultâneas dividem os 10 Mbps do `adsl_10`. O servidor tem um único `LinkShaper` por processo e cada fatia de 64 KiB espera a sua vez no enlace **antes** de ser escrita; uma peça de 1 MiB não chega inteira antes do atraso. Tempo teórico = latência + bytes×8/banda (8 MiB em `adsl_10`: cerca de 6,7 s). `test_shaping.py` confere 1 peça e 8 em paralelo nos 3 perfis (±10%; em `fibra_300` há folga mínima de 30 ms, porque o custo fixo de Python e loopback já passa de 10% de 38 ms). Rode com `python -m unittest test_shaping` dentro de `sw-netlab`. Latência é adicionada uma vez por requisição. Perda é uma probabilidade de abortar uma requisição antes do corpo; não representa perda real de pacotes TCP, que exigiria shaping abaixo da camada HTTP (por exemplo, `tc netem`). O benchmark local verifica um intervalo parcial, baixa o arquivo completo com retomada de conexão, valida o SHA-256, mede banda pelo corpo e latência pela mediana de probes `HEAD`; ambas devem ficar dentro de ±10%.

Código sem dependências de runtime. Black, Ruff e mypy estão configurados em `pyproject.toml`; CI executa esses três e clang-format/clang-tidy do `sw-core`.
