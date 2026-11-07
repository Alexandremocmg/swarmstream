# Pedido ao R1: análise só de leitura do caminho mínimo do F1-67

**Autorizado pelo dono em 2026-11-06 (opção B da [proposta do Portão 1](portao-1-caminho-minimo.md)).** O Planejador não abre sessões: o dono abre a sessão do R1 e cola este pedido. **A análise não autoriza implementar, mudar cartão, ordem, critério ou dependência.** Qualquer corte ou reordenação que ela sugira volta ao dono como proposta.

## Pergunta
O [F1-67](../../tarefas/cartoes/F1-67.md) hoje "precisa de F1-69, F1-70, F1-71 e F1-72 FEITAS antes do início" (decisão do dono, 2026-11-02). **Para cada um desses cartões (e para F1-73 e F1-74, que precedem F1-71/F1-72 na ordem de 2026-11-05), o que o caminho mínimo do F1-67 efetivamente usa?** O objetivo é separar, com fatos:
- o que o `sw_init`/`sw_shutdown`/`sw_open` precisam **para funcionar e serem medidos** (leitura real pelo cache, agendador e fonte HTTP, com telemetria);
- o que é **robustez contra falta de memória** (não terminar o processo no MSVC Debug, `OutOfMemory`/`Internal`, janela pós-`rename`) e que não muda o resultado funcional de uma leitura medida.

## Entregável (um documento curto, sem código)
Uma tabela por cartão: **o que o F1-67 chama**, **fronteira ou contrato envolvido** (com arquivo e linha), **o que quebra no F1-67 se faltar**, **o que só é garantia de OOM**, **retrabalho previsível em `sw_shutdown`/`sw_init` se o cartão vier depois**. Mais: a lista dos **contratos que cada opção de adiamento afetaria** (`swarmstream.h` v0 não muda; ADR-0017; ADR-0018 em Proposta; `CacheErrorCode`; F1-60 §1.1) e **o que não foi possível concluir por leitura**.

## Regras
- Só leitura. Não abrir branch de código, não rodar a varredura de falta de memória, não tocar no #37.
- Ler: [F1-60](../../tarefas/cartoes/F1-60.md) (§1.1, §5, §6, §8), F1-67, F1-68, F1-69 a F1-74, ADR-0017, ADR-0018 (Proposta), o código atual de `CacheOwner::start/stop`, `Impl::checkpoint` e `platform_file.hpp`.
- Separar **o que você leu** do **que é suposição**. Sem previsão de prazo.
- Não reduzir critério do Portão 1 ([docs/16](../16-briefing-fase-1a.md)).

## Depois
O Planejador junta a resposta à proposta e o dono decide entre as opções A, C ou outra. Nada muda antes disso.
