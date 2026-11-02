# ADR-0018: Validade do contexto e liberação de recursos depois de `sw_shutdown`

**Status:** Proposta (não aceita; o dono do produto escolheu a opção A em 2026-11-02, pediu correções em 2 rodadas e decide o aceite à parte) · **Data:** 2026-11-02 · **Autor:** Planejador (a pedido do dono)

## Contexto
O comentário de `sw_shutdown` em [`swarmstream.h`](../../sw-core/include/swarmstream.h) v0 (contrato congelado, [ADR-0014](0014-contrato-swarmstream-h-v0.md)) diz que a função encerra o contexto e solta o lock, que `SW_E_BUSY` não muda nada e que "depois de `SW_OK`, `ctx` é inválido". Ele **não diz**: (1) se `ctx` continua válido depois de outro erro; (2) se a chamada pode ser repetida; (3) o que acontece com os recursos quando uma etapa do encerramento falha.

**Fatos do código atual (2026-11-02), que condicionam o texto:**
- `CacheOwner::stop()` é `noexcept` e chama `Impl::checkpoint()`, que **não** é `noexcept` e **aloca** (`entries.reserve`, cópia das listas de checksums, `std::filesystem::path`, `unique_ptr`). Uma falta de memória ali é `std::bad_alloc` dentro de uma função `noexcept`: hoje isso chama `std::terminate`. Um `catch` na API externa (`sw_shutdown`) **não** impede esse `terminate`; a captura tem de existir **dentro** de `stop()`, antes de a exceção cruzar a fronteira `noexcept`.
- O `CacheOwner` documenta que, depois de uma falha de sync, só `stop()` e um `start()` novo resolvem, e que um sync que falhou **nunca é repetido** ([docs/04](../04-cache-local.md), `cache_owner.hpp`). Não existe "tentar o flush de novo".
- `~TelemetryLog()` não devolve resultado; o resumo e o fechamento da telemetria não têm código de retorno.
- O lock da pasta ([ADR-0011](0011-tecnologia-do-launcher.md)) precisa ser solto para o launcher ou o jogo assumirem a pasta; um contexto que sobrevive a um erro seguraria esse lock.

## Opções consideradas
1. **A. Leva o encerramento até o fim, libera tudo e informa a falha.** Depois de passada a validação, o resultado é sempre "contexto inválido e recursos liberados"; a falha é devolvida no código. Prós: sem vazamento, sem lock preso, o chamador sabe. Contras: complementa o texto do header (daí este ADR) e **exige** que o caminho de parada nunca termine o processo (requisitos abaixo).
2. **B. Contexto continua válido depois de erro e a chamada é repetível.** Contras: o flush não se repete; recursos e lock retidos; vazamento se o chamador não repetir.
3. **C. Desliga sempre, devolve `SW_OK` e só registra o erro.** **Rejeitada pelo dono** (2026-11-02).

## Decisão (proposta: opção A)
**Não há promessa de desligamento sem alocações.** O encerramento **pode alocar** (o checkpoint final do cache, caminhos, resumo da telemetria). O que se promete é: falta de memória ou exceção **não termina o processo, não impede a liberação e é informada**. Para isso há requisitos internos explícitos (abaixo), com escopo próprio ([F1-71](../../tarefas/cartoes/F1-71.md)).

### Texto proposto para o comentário de `sw_shutdown` (o header NÃO é alterado por este ADR; só depois do aceite, em PR próprio com R1 + R2)
```c
/* Encerra o contexto e solta o lock da pasta de cache.
 * Pré-condições: todos os sw_file e sw_dir fechados (senão SW_E_BUSY e nada
 * muda); nenhuma outra chamada em curso com este ctx; não chamar de dentro
 * de callback (SW_E_BUSY). Pedidos de hint pendentes são descartados.
 * Passada a validação, o encerramento SEMPRE vai até o fim; o resultado é:
 *   SW_OK                  tudo liberado, lock solto. `ctx` é inválido.
 *   SW_E_IO, SW_E_DISK_FULL, SW_E_OUT_OF_MEMORY, SW_E_INTERNAL
 *                          uma etapa do encerramento (disco, memória ou erro
 *                          interno) falhou. Mesmo assim TODOS os recursos
 *                          foram liberados, o lock foi solto e `ctx` é
 *                          inválido, como em SW_OK. A biblioteca não termina
 *                          o processo. Se mais de uma etapa falhou, devolve
 *                          o código da primeira na ordem do encerramento; as
 *                          outras vão ao log_fn. Não repita a chamada.
 *   SW_E_BUSY              há handles abertos, ou chamada vinda de callback.
 *                          Nada muda; `ctx` continua válido.
 *   SW_E_INVALID_ARGUMENT  `ctx` é NULL, desconhecido ou já desligado. Nada
 *                          muda.
 *   SW_E_SHUTTING_DOWN     outra thread já está desligando este ctx (uso
 *                          incorreto). Nada muda por esta chamada.
 * Nenhum outro código é devolvido. O encerramento pode alocar memória; sem
 * memória, etapas opcionais (como o checkpoint final do cache) são puladas,
 * o código é SW_E_OUT_OF_MEMORY e a liberação prossegue. Depois de qualquer
 * resultado que não seja SW_E_BUSY, SW_E_INVALID_ARGUMENT ou
 * SW_E_SHUTTING_DOWN, o alocador e o log_fn da configuração não são mais
 * usados. Depois de uma falha, o próximo sw_init aplica o protocolo de
 * recuperação da pasta, que pode descartar ou reconstruir parte da
 * indexação, ou falhar com diagnóstico; nenhum dado é entregue sem
 * verificação, e não há garantia de que o próximo sw_init tenha sucesso.
 * Reusar um `ctx` desligado é indefinido; a biblioteca só o rejeita por
 * melhor esforço. */
```

### Mapeamento das falhas do encerramento (sem converter tudo em `SW_E_IO`)
| Origem | Código de `sw_shutdown` |
|---|---|
| erro de disco no checkpoint, no journal ou no store (`CacheErrorCode::IoError`) | `SW_E_IO` |
| disco cheio ao gravar o checkpoint (`DiskFull`) | `SW_E_DISK_FULL` |
| `std::bad_alloc` em qualquer etapa | `SW_E_OUT_OF_MEMORY` |
| outra exceção, ou condição interna inesperada | `SW_E_INTERNAL` |
| falha de escrita da telemetria (diagnóstico, não dado) | **não muda o retorno**: vai ao `log_fn` (nível ERROR) e ao resumo, se couber |

Encerramento concluído implica contexto inválido e recursos liberados **também** com `SW_E_OUT_OF_MEMORY` e `SW_E_INTERNAL`: esses dois códigos só saem de `sw_shutdown` **depois** da validação; antes dela só saem `BUSY`, `INVALID_ARGUMENT` e `SHUTTING_DOWN`.

### Requisitos internos para cumprir o texto (verificáveis; escopo em [F1-71](../../tarefas/cartoes/F1-71.md) e no F1-67)
1. **Nenhuma exceção cruza uma fronteira `noexcept` no caminho de parada.** Cada função desse caminho ou não aloca, ou captura `std::bad_alloc`, `std::exception` e `...` **dentro dela** e converte em código. Isso inclui `CacheOwner::stop()` (e portanto `~CacheOwner()`), o resumo e o fechamento da telemetria (`~TelemetryLog()`), e, na etapa 2, a parada do agendador e da fonte.
2. **`CacheOwner::stop()`**: `bad_alloc` no checkpoint => o checkpoint é **pulado** (o journal permanece; a recuperação o aplica), `journal.close()` e `store.close()` **ainda são executados**, e `stop()` devolve um código de memória (valor novo do `CacheErrorCode`, interno, não do header). Outra exceção => código interno novo. Nunca `terminate`.
3. **Mensagens de log do encerramento são formatadas sem alocar** (buffer fixo), para que o próprio log não lance exceção.
4. A destruição dos recursos em S5-S8 usa só operações que não lançam (liberar com o `free_fn`, destruir objetos); a ordem de encerramento da [F1-60 §1.1](../../tarefas/cartoes/F1-60.md) continua valendo e o lock é solto por último.

## Consequências
- (+) Lock, threads e memória são sempre liberados; o chamador vê a falha (disco, memória ou interna) sem confusão de códigos; a regra de validade é uma só.
- (+) Nenhum binário existente quebra: o v0 ainda não tem `sw_shutdown` real; só o comentário muda.
- (−) Exige mudança de código **crítica** fora do `sw_shutdown`: `CacheOwner::stop()` (F1-71) e a auditoria do resto do caminho de parada. Não é "só texto".
- (−) Quem tratava "erro" como "contexto ainda vivo" erra; por isso o texto diz isso e o teste cobre a repetição (rejeição por melhor esforço, sem corromper memória).
- (−) Um checkpoint pulado por falta de memória deixa o journal mais longo para o próximo `sw_init`; a recuperação pode custar mais tempo ou descartar indexação.
- **Como verificar:** (a) em `stop()`: varredura de falha de `operator new` em **todos** os pontos do `stop()`, sem excluir pontos (como no F1-63), mais exceção injetada em cada passo: nenhum `terminate`, journal e store fechados, código esperado; (b) em `sw_shutdown` (F1-67): a mesma varredura sobre o `sw_shutdown` inteiro e falhas injetadas no `stop()` e na telemetria: resultado em {`OK`, `IO`, `DISK_FULL`, `OUT_OF_MEMORY`, `INTERNAL`}, contexto inválido, `vivos == 0` no alocador de teste, lock solto (outro processo o obtém), nenhuma chamada ao alocador nem ao `log_fn` depois do retorno; (c) reabertura da pasta depois da falha injetada: o `sw_init` aplica a recuperação e **ou** tem sucesso sem entregar dado não verificado, **ou** falha com diagnóstico (o teste aceita as duas saídas e reprova qualquer dado sem validação); (d) `git diff` do header limitado ao comentário.
- **Efeito no processo:** o F1-67 não implementa a regra de resultado de `sw_shutdown` antes do aceite deste ADR; o F1-71 (fronteira do `stop()`) é pré-requisito do F1-67 e não depende do aceite do ADR para ser planejado, mas só começa com autorização do dono.
