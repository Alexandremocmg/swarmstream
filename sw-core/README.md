# sw-core

Biblioteca central do cliente Swarmstream em C++20.

## Componentes

- **`ICacheStore`**: Interface para acesso e gerenciamento do cache local de slots pré-alocados, transações atômicas de commit e leitura protegida por *leases* com contagem de referência.
- **`IContentSource`**: Interface de abstração de fontes de rede (CDN, P2P swarm / libtorrent) consumida pelo agendador por prazos (*deadline scheduler*).
- **`DeadlineScheduler`**: Agendador por prazo v1 (F1-28) sobre uma `IContentSource`: fila por prazo, demanda sempre antes de prefetch, vagas reservadas para demanda, pedidos da mesma peça juntados, prefetch vencido descartado e cancelamento. Regras em `include/swarmstream/deadline_scheduler.hpp`.
- **`MemoryCacheStore`**: Mock em memória thread-safe de `ICacheStore` com simulação de WAL (*Write-Ahead Log*), verificação de integridade e proteção de leases.
- **`MemoryContentSource`**: Mock em memória assíncrono de `IContentSource` com suporte a cancelamento, latência simulada e injeção de erros.

## Compilação e Testes

```bash
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```
