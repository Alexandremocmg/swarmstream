# ADR-0017: Conflito arquivo/diretório no manifesto v0

**Status:** Proposta (aguarda aceite do dono do produto) · **Data:** 2026-10-29 · **Autor:** Planejador (a pedido do dono)

## Contexto
O [03 §8, item 7](../03-formato-de-conteudo.md#8-validações-do-leitor) exige caminhos válidos, estritamente crescentes (bytewise) e sem colisão ignorando maiúsculas. Ele **não** diz nada sobre um caminho que seja, ao mesmo tempo, arquivo e prefixo de diretório: `a` e `a/b` passam em todas as validações atuais do `ManifestView::open` (`sw-format`).

O contrato [`swarmstream.h`](../../sw-core/include/swarmstream.h) v0 expõe diretórios virtuais derivados dos caminhos (`sw_stat`, `sw_dir_open/next`). Com `a` arquivo e `a/b` arquivo, `a` seria arquivo e diretório: `sw_stat("a")`, `sw_open("a")` e a listagem do pai teriam duas respostas ou nome duplicado. A lacuna foi achada pela sessão do F1-17 (VFS) e **não estava prevista** no formato.

## Decisão proposta
1. **Definição.** Há conflito arquivo/diretório se existirem, no manifesto, um caminho `P` e um caminho `Q` com `Q.size() > P.size()`, `Q` começando por `P` e `Q[P.size()] == '/'`. Prefixo sem barra (`a` e `ab`, `a` e `a-1`) **não** é conflito. `a` e `a/` não coexistem (o §6 já proíbe `/` final).
2. **Regra.** Um manifesto com conflito é **inválido**. O leitor recusa com `SW_E_MANIFEST_INVALID` (nada é corrigido em silêncio, como no resto do §8). Acrescentar ao §8 o item "7a" com a definição acima, **depois** do aceite deste ADR.
3. **Onde é aplicada no v0.** No `sw-core`, na construção do índice do VFS (F1-17, `Vfs::build`), que roda no `sw_init` (F1-60). O `ManifestView::open` do `sw-format` **não muda** neste ADR. O mapeamento para a API C é `SW_E_MANIFEST_INVALID` em `sw_init`; não há código público novo.
4. **Fora deste ADR.** Fazer o `sw-format` ou o construtor (`sw-pack`) recusarem/gerarem esse caso é decisão e card separados (a alternativa, mais forte, muda a validação de `ManifestView::open` e exige reexecutar os vetores de teste do formato).

## Alternativas
- **A. Recusar só no VFS (esta proposta).** Nenhuma mudança em `sw-format` nem nos vetores; o erro aparece em `sw_init`. Custo: quem usar só o `sw-format` (ferramentas) aceita o manifesto.
- **B. Apertar `ManifestView::open` no `sw-format`.** Uma fonte só da regra; exige alterar o leitor, os vetores e os testes do formato.
- **C. Aceitar e definir uma regra de precedência** (por exemplo, arquivo vence diretório). Descartada: esconde um erro do produtor do pacote e cria nomes ambíguos.

## Consequências
- (+) Nenhum manifesto ambíguo chega ao `sw_stat`/`sw_dir_*`; o erro é detectado na abertura, não numa leitura.
- (+) `swarmstream.h` v0 não muda.
- (−) Enquanto o construtor não recusar o caso, o `sw-pack` pode produzir um manifesto que o cliente recusa. Mitigação: card de acompanhamento (a criar, se o dono quiser).
- (−) Duas camadas conhecem a regra se um dia for movida para o `sw-format`; por isso a proposta B fica registrada.

## Verificação
Teste do F1-17 (`test_empty_manifest_and_conflict`, `test_file_directory_conflict_definition`) cobre a definição, com exemplos de conflito e de não conflito.
