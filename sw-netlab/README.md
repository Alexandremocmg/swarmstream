# sw-netlab

Servidor HTTP/1.1 para fixtures de CDN, com suporte a `Range: bytes=...`, latência, banda e perda configuráveis.

```powershell
python server.py --root fixtures --profile cabo_50 --port 8080
python server.py --root fixtures --profile custom --bandwidth-mbps 20 --latency-ms 100 --loss-percent 2
python benchmark.py --size-mib 64
```

Perfis iniciais: `fibra_300` (300 Mbps, 10 ms, 0%), `cabo_50` (50 Mbps, 25 ms, 0,2%) e `adsl_10` (10 Mbps, 60 ms, 1%). Banda é aplicada por resposta HTTP concorrente. Latência é adicionada uma vez por requisição. Perda é uma probabilidade de abortar uma requisição antes do corpo; não representa perda real de pacotes TCP, que exigiria shaping abaixo da camada HTTP (por exemplo, `tc netem`). O benchmark local verifica um intervalo parcial, baixa o arquivo completo com retomada de conexão, valida o SHA-256, mede banda pelo corpo e latência pela mediana de probes `HEAD`; ambas devem ficar dentro de ±10%.

Código sem dependências de runtime. Black, Ruff e mypy estão configurados em `pyproject.toml`; CI executa esses três e clang-format/clang-tidy do `sw-core`.
