# ADR-0018: Validade do contexto e liberação de recursos depois de `sw_shutdown`

**Status:** Proposta (não aceita; o dono do produto escolheu a opção A em 2026-11-02 e pediu o texto para revisão) · **Data:** 2026-11-02 · **Autor:** Planejador (a pedido do dono)

## Contexto
O comentário de `sw_shutdown` em [`swarmstream.h`](../../sw-core/include/swarmstream.h) v0 (contrato congelado, [ADR-0014](0014-contrato-swarmstream-h-v0.md)) diz que a função encerra o contexto e solta o lock, que `SW_E_BUSY` não muda nada e que "depois de `SW_OK`, `ctx` é inválido". Ele **não diz**: (1) se `ctx` continua válido depois de outro erro; (2) se a chamada pode ser repetida; (3) o que acontece com os recursos quando o flush final falha.

O caso que força a pergunta é a falha de I/O ao parar o `CacheOwner` (flush do journal, checkpoint final, fechamento do store) ou ao fechar a telemetria. O `CacheOwner` documenta que, depois de uma falha de sync, só `stop()` e um `start()` novo resolvem, e que um sync que falhou **nunca é repetido** ([docs/04](../04-cache-local.md), [`cache_owner.hpp`](../../sw-core/include/swarmstream/cache_owner.hpp)). Logo não existe "tentar o flush de novo". Além disso o lock da pasta ([ADR-0011](0011-tecnologia-do-launcher.md)) precisa ser solto para o launcher ou o jogo assumirem a pasta; um contexto que continua vivo depois de um erro seguraria esse lock.

## Opções consideradas
1. **A. Desliga sempre e devolve o erro.** Em qualquer resultado que não seja `SW_E_BUSY`/`SW_E_INVALID_ARGUMENT`, todos os recursos foram liberados e `ctx` é inválido; a falha de I/O é devolvida como `SW_E_IO`. Prós: o chamador sabe que houve falha; lock e memória sempre liberados. Contras: exige que o contrato diga que `ctx` é inválido também depois de `SW_E_IO` (complemento do texto do header, daí este ADR).
2. **B. Contexto continua válido depois de `SW_E_IO` e a chamada é repetível.** Prós: leitura literal do header. Contras: o flush não se repete, então a segunda chamada só poderia descartar; o lock e os recursos ficam retidos até o chamador repetir; vazamento se ele não repetir.
3. **C. Desliga sempre, devolve `SW_OK` e registra o erro só no log e na telemetria.** Prós: não muda o header. Contras: o chamador que só olha o retorno não vê a falha. **Rejeitada pelo dono** (2026-11-02): não aprova devolver `SW_OK` depois de falha de I/O.

## Decisão (proposta: opção A)
O comentário de `sw_shutdown` em `swarmstream.h` passa a ter o texto abaixo. **Nenhuma assinatura, enum, struct, valor numérico ou código de erro muda**; só o comentário é complementado, o que pelo ADR-0014 exige este ADR aceito e a aprovação de R1 + R2 no PR do header.

```c
/* Encerra o contexto e solta o lock da pasta de cache.
 * Pré-condições: todos os sw_file e sw_dir fechados (senão SW_E_BUSY e nada
 * muda); nenhuma outra chamada em curso com este ctx; não chamar de dentro
 * de callback (SW_E_BUSY). Pedidos de hint pendentes são descartados.
 * Resultados:
 *   SW_OK          tudo liberado, lock solto. `ctx` é inválido.
 *   SW_E_IO        o flush, o checkpoint final ou o fechamento do cache (ou da
 *                  telemetria) falhou. Mesmo assim TODOS os recursos foram
 *                  liberados, o lock foi solto e `ctx` é inválido, como em
 *                  SW_OK. Os bytes já verificados continuam no disco e o
 *                  próximo sw_init recupera a pasta pelo journal. Não repita
 *                  a chamada.
 *   SW_E_BUSY      há handles abertos, ou chamada vinda de callback. Nada
 *                  muda; `ctx` continua válido.
 *   SW_E_INVALID_ARGUMENT  `ctx` é NULL, desconhecido ou já desligado. Nada
 *                  muda.
 *   SW_E_SHUTTING_DOWN     outra thread já está desligando este ctx (uso
 *                  incorreto). Nada muda por esta chamada.
 * Nenhum outro código é devolvido. Depois de SW_OK ou SW_E_IO, o alocador e
 * o log_fn da configuração não são mais usados. Reusar um `ctx` desligado é
 * indefinido; a biblioteca só o rejeita por melhor esforço. */
```

Regras de implementação que acompanham o texto (verificáveis por teste):
- Em S4 (parar o `CacheOwner`) ou S6 (fechar a telemetria), um erro **não interrompe** a destruição: os passos seguintes executam, o lock é solto por último e o primeiro erro vira `SW_E_IO`. Falhas seguintes são só registradas no `log_fn`.
- `sw_shutdown` não aloca memória (nenhum `SW_E_OUT_OF_MEMORY`).
- `SW_E_BUSY` e `SW_E_INVALID_ARGUMENT` nunca alteram o estado do contexto.

## Consequências
- (+) Lock, threads e memória são sempre liberados; o chamador vê a falha de I/O; a regra de validade é uma só (`SW_OK` ou `SW_E_IO` => `ctx` inválido).
- (+) Nenhum binário existente quebra: o v0 ainda não tem implementação real de `sw_shutdown`; só o comentário muda.
- (−) Quem tratava "erro" como "contexto ainda vivo" erra. Por isso o texto diz isso em palavras e o teste cobre a repetição da chamada (rejeitada por melhor esforço, sem corromper memória).
- (−) Um `SW_E_IO` aqui não distingue qual recurso falhou; o detalhe vai ao `log_fn` e ao resumo da telemetria.
- Como verificar: teste do F1-67 com falha injetada no `stop()` e na telemetria (todos os recursos liberados, lock solto, alocador sem blocos vivos, reabertura com índice consistente), mais o `diff` do header limitado ao comentário.
- **Efeito no processo:** o F1-67 não implementa `sw_shutdown` antes do aceite deste ADR; o PR que complementa o comentário do header só abre depois do aceite.
