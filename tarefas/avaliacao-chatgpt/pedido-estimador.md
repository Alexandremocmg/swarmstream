# Pedido ao agente externo: estimador de banda e RTT (avaliação do ChatGPT)

Este texto é o pedido completo. Cole-o inteiro para o agente e anexe os 3 arquivos listados na seção "Arquivos que você recebe". O agente não precisa de mais nada do projeto.

## Contexto
Swarmstream é uma biblioteca em C++20 (licença Apache-2.0) que entrega assets de jogos por cache local, P2P e CDN. Você vai escrever **uma peça pequena e isolada**: o estimador de banda e de RTT (latência de ida e volta) de uma fonte de dados. O agendador do projeto usará esses dois números para decidir se uma peça chega a tempo. Nada mais do projeto é necessário: o estimador é só aritmética, sem rede, sem threads, sem alocação.

## O que entregar (2 arquivos novos, e só eles)
1. `sw-core/include/swarmstream/source_estimator.hpp` (apenas cabeçalho)
2. `sw-core/tests/test_source_estimator.cpp` (teste com `main`)

Entregue o **texto completo dos dois arquivos**, em dois blocos de código separados, sem "..." nem trechos omitidos. Depois dos blocos, escreva uma lista curta com: decisões que você tomou onde o pedido deixou margem, o que você **não** conseguiu verificar e o que testou (se puder rodar código, o comando e a saída; se não puder, diga que não rodou).

## Regras do projeto (obrigatórias)
- Português do Brasil nos comentários. Código e identificadores em inglês, como nos arquivos anexos.
- Cada arquivo começa com exatamente estas duas linhas:
  ```
  // SPDX-License-Identifier: Apache-2.0
  // Copyright 2026 Alexandre Aparecido Rodrigues Peres
  ```
- C++20, `namespace swarmstream`, `#pragma once`, sem dependências além da biblioteca padrão.
- **Sem alocação dinâmica, sem exceções, sem `std::mutex`, sem `static` mutável.** O estimador é chamado no caminho quente.
- Imite o estilo, a densidade de comentários e os nomes dos arquivos anexos (`mpsc_queue.hpp` para o cabeçalho, `test_xxh3.cpp` e `check.hpp` para o teste).
- No teste, use **`SW_CHECK(...)`** (de `check.hpp`), nunca `assert`: o `assert` some em build Release e o teste passaria sem verificar nada.
- Teste que passa sem testar nada é defeito. Todo teste deve ter pelo menos uma verificação que falharia se o código estivesse errado.
- Não invente API do projeto além do que está nos arquivos anexos. Se precisar de algo que não existe, escreva você mesmo dentro do cabeçalho.

## Interface pedida
```cpp
namespace swarmstream {

class SourceEstimator
{
public:
    // Registra uma medida de RTT (tempo entre enviar o pedido e receber o primeiro byte).
    void record_rtt(std::chrono::microseconds rtt) noexcept;

    // Registra uma transferência concluída.
    //   bytes: tamanho do corpo recebido.
    //   duration: do primeiro byte ao último byte do corpo (não inclui o RTT).
    //   concurrent: quantas transferências desta fonte estavam ativas ao mesmo tempo (>= 1,
    //               contando esta). O enlace é dividido entre elas.
    void record_transfer(std::uint64_t bytes, std::chrono::microseconds duration,
                         unsigned concurrent) noexcept;

    // Vazios até haver a primeira amostra válida.
    [[nodiscard]] std::optional<std::chrono::microseconds> rtt() const noexcept;
    [[nodiscard]] std::optional<double> bandwidth_bytes_per_sec() const noexcept;

    // Quanto tempo uma peça de `bytes` leva até o último byte: rtt + bytes / banda.
    // Vazio se faltar RTT ou banda.
    [[nodiscard]] std::optional<std::chrono::microseconds>
    estimate_fetch_time(std::uint64_t bytes) const noexcept;
};

} // namespace swarmstream
```
Classe não é thread-safe (quem chama sincroniza). Não precisa de construtor além do padrão.

## Regras do cálculo (definidas aqui para o resultado ser verificável)
**RTT.** Média móvel exponencial: a primeira amostra vira o valor; as seguintes fazem `valor = valor + (amostra - valor) / 8`. Amostra `<= 0` é ignorada. Em microssegundos, com aritmética em `double` ou `int64`, à sua escolha, mas sem estouro.

**Banda.** Cada transferência dá uma amostra do **enlace inteiro**: `amostra = bytes * concurrent / duration_em_segundos`. (Se 4 transferências dividem o enlace igualmente, cada uma vê 1/4 dele; multiplicar por `concurrent` recupera o enlace.) Regras:
- Ignore a transferência se `bytes < 65536` (64 KiB; transferências pequenas medem mais o RTT que a banda), se `duration <= 0` ou se `concurrent == 0`.
- A primeira amostra válida vira o valor; as seguintes fazem `valor = valor + (amostra - valor) / 4`.
- Unidade: bytes por segundo, `double`.

**Tempo estimado.** `rtt + bytes / banda`, arredondado para cima em microssegundos. Sem RTT ou sem banda, devolve vazio.

## O que o teste deve cobrir
Simule, sem rede e sem relógio real, três perfis de enlace e gere as amostras de forma determinística (use `std::mt19937` com semente fixa e variação uniforme de ±10% em torno do valor verdadeiro):

| perfil | banda | RTT |
| --- | --- | --- |
| `fibra_300` | 300 Mbps (37 500 000 B/s) | 10 ms |
| `cabo_50` | 50 Mbps (6 250 000 B/s) | 25 ms |
| `adsl_10` | 10 Mbps (1 250 000 B/s) | 60 ms |

Casos mínimos:
1. **Erro < 20%.** Para cada perfil, alimente 50 medidas de RTT e 50 transferências de 1 MiB (cada uma com duração calculada pelo enlace dividido por `concurrent`, que varia de 1 a 4) e verifique que `rtt()` e `bandwidth_bytes_per_sec()` ficam a menos de 20% do valor verdadeiro. Este é o critério de aceite da tarefa.
2. **Sem amostra, sem valor.** Estimador novo devolve vazio nos três getters.
3. **Ignora o que deve ignorar.** Transferência de 1 byte, de 65535 bytes, com duração zero e com `concurrent == 0` não mudam nada (e o estimador continua vazio se for a única).
4. **Primeira amostra vira o valor** (RTT e banda), e a segunda amostra aplica exatamente 1/8 (RTT) e 1/4 (banda). Verifique os números exatos.
5. **`concurrent` multiplica.** Duas transferências idênticas, uma com `concurrent = 1` e outra com `concurrent = 4` em estimadores separados: a segunda dá banda 4 vezes maior.
6. **`estimate_fetch_time`** confere com a fórmula num caso com números redondos (ex.: RTT 10 ms, banda 1 000 000 B/s, peça de 500 000 B dá 510 000 µs) e devolve vazio quando falta RTT ou banda.
7. **Mutação mental:** se alguém trocar `/ 8` por `/ 4` no RTT, ou esquecer o `* concurrent`, algum `SW_CHECK` tem de falhar. Diga no resumo qual verificação pega cada uma.

O `main` do teste imprime uma linha por caso e retorna 0 só se tudo passar (`SW_CHECK` já termina o processo com código 1 na falha).

## Como vamos conferir (não precisa ser feito por você)
```
g++ -std=c++20 -Wall -Wextra -Werror -O2 -I sw-core/include -I sw-core/tests \
    sw-core/tests/test_source_estimator.cpp -o test_source_estimator && ./test_source_estimator
```
O mesmo teste também será compilado em Release (com `-DNDEBUG`), com `-fsanitize=address,undefined` e com MSVC. Por isso: nada de `assert`, nada de comportamento indefinido (cuidado com divisão por zero, conversão de `double` para inteiro fora da faixa e estouro de `int64`) e nada específico de compilador.

## Arquivos que você recebe (anexe estes 3, nesta ordem)
1. `sw-core/tests/check.hpp`: a macro `SW_CHECK`, que o teste usa.
2. `sw-core/include/swarmstream/mpsc_queue.hpp`: exemplo do estilo de cabeçalho, comentários e nomes.
3. `sw-core/tests/test_xxh3.cpp`: exemplo do estilo de teste (`main`, `SW_CHECK`, saída).

## O que NÃO fazer
- Não altere nem proponha alterar outros arquivos (`CMakeLists.txt`, `swarmstream.h`, docs): a integração ao build é nossa.
- Não adicione dependências, nem bibliotecas de terceiros, nem `#include` fora da biblioteca padrão e de `check.hpp` (só no teste).
- Não copie código de outros projetos.
