# ADR-0014: Contrato `swarmstream.h` v0 e regras de compatibilidade da API

**Status:** Proposta — em revisão pelos dois sêniores · **Data:** 2026-10-17 · **Autor:** arquiteto/tech lead · **Revisores:** sênior de engine/streaming **e** sênior de redes/sistemas

## Contexto
A API pública em C ([ADR-0006](0006-cpp20-nucleo-com-api-c.md)) é a única dependência dura entre trilhas na Fase 1: o `sw-godot` (T3) programa contra ela enquanto o `sw-core` (T2/T4) é escrito. O rascunho em [06](../06-integracao-engine-sdk.md#api-do-núcleo-rascunho-do-swarmstreamh) tinha lacunas que viram retrabalho se ficarem para depois:
- sem prazo nem limite de espera na leitura, que são o centro do agendador ([ADR-0003](0003-agendador-por-prazo.md)) e da política de falta;
- sem tamanho de arquivo, `stat` nem listagem de diretório, que o `FileAccess`/`DirAccess` do Godot exigem;
- sem cancelamento de leitura assíncrona nem garantia de quantas vezes o callback é chamado;
- sem forma de pedir "deixe este arquivo local e me avise" sem copiar bytes, que é o que um `ResourceFormatLoader` precisa (abordagem A do [spike de engine](../spikes/F0-spike-fileaccess-godot.md));
- funções `void`, o que contraria "nenhuma falha silenciosa";
- sem forma de estender structs sem quebrar binários já compilados.

## Decisão
1. O contrato v0 é o arquivo [`sw-core/include/swarmstream.h`](../../sw-core/include/swarmstream.h). Ele substitui o rascunho de [06](../06-integracao-engine-sdk.md).
2. **Compatibilidade binária:**
   - handles opacos; structs de entrada e saída começam por `struct_size`; campos novos só no fim;
   - tipos de largura fixa; enums expostos como `int32_t` (o tamanho de `enum` em C varia entre compiladores);
   - códigos de erro nunca reaproveitados; código negativo desconhecido = erro genérico;
   - `SW_API_VERSION` = MAJOR << 16 | MINOR. `sw_init` recusa MAJOR diferente com `SW_E_VERSION_MISMATCH`.
3. **Mudança depois do congelamento exige ADR novo**, mesmo se compatível:
   - adição compatível (função nova, campo no fim de struct, erro novo, flag nova) → ADR curto + MINOR + 1;
   - mudança incompatível (assinatura, semântica, layout, remoção) → ADR com justificativa medida + MAJOR + 1.
4. **Layout travado por teste:** `sw-core/tests/api_v0_smoke.c` e `api_v0_cxx.cpp` verificam os tamanhos das structs. Teste que quebra = mudança de contrato.
5. **Sem modo de manifesto sem assinatura** ([ADR-0012](0012-serializacao-do-manifesto.md)): a F1 usa a chave de teste em `trusted_keys`.
6. **Fora do v0, de propósito** (entram por ADR quando a fase precisar): instalação e modo completo (pin de grupos, sem remoção) para o launcher (F4); cifra por pacote e chave de licença (F4); troca de manifesto em execução (patch com o jogo aberto); eventos de telemetria por callback.

## Processo de revisão e congelamento

| Quando | O quê | Quem |
| --- | --- | --- |
| 2026-10-19 (seg., semana 1) | Header, stub e teste enviados para revisão no PR do F0-06 | Arquiteto |
| até 2026-10-21 | Aprovação ou lista de mudanças, nos comentários do PR (2 dias, como no briefing) | Os dois sêniores |
| até 2026-10-27 | Mudanças aplicadas; segunda rodada, se necessária | Arquiteto + sêniores |
| **2026-10-28 (qua., semana 2)** | **Congelamento:** este ADR passa a "Aceita", o header ganha a marca "CONGELADO" e o CODEOWNERS de `/sw-core/include/` exige o arquiteto e um sênior | Arquiteto |

Se a revisão não fechar até 2026-10-28, a Fase 1 não começa ([14](../14-briefing-fase-0.md#se-alguém-atrasar)); o dono do produto é avisado no mesmo dia.

### Lista de verificação para os revisores
- [ ] **Engine:** a abordagem A do spike (`ResourceFormatLoader` na frente + `sw_ensure_async` + `sw_read`) e a reserva B (`.pck` por grupo) funcionam só com esta API? Se o ADR de intercepção (0016) escolher outra abordagem, o que falta?
- [ ] **Engine:** `timeout_ms = 0` + `SW_E_NOT_LOCAL` + pedido feito com prazo atende a política de falta (mip menor agora, mip alto depois)?
- [ ] **Engine:** `sw_close` bloqueia até os callbacks do arquivo terminarem. Isso é aceitável dentro de `_load()` e na destruição de recursos, ou precisamos de fechamento sem espera?
- [ ] **Redes:** `deadline_ms` relativo, em milissegundos e 32 bits basta para o agendador (máximo ≈ 49 dias)?
- [ ] **Redes:** o callback "exatamente uma vez, possivelmente antes de `sw_read_async` voltar" é implementável com a thread dona do cache e a fila MPSC ([ADR-0005](0005-cache-em-slots-com-leases.md)) sem corrida no `out_id`?
- [ ] **Redes:** `sw_stats` cobre as métricas de [08](../08-metricas-e-testes.md) que o overlay e a bancada precisam na F1?
- [ ] **Ambos:** a regra de um processo dono por pasta de cache (`SW_E_CACHE_LOCKED`, [ADR-0011](0011-tecnologia-do-launcher.md)) funciona para a bancada com N peers headless (uma pasta por peer)?
- [ ] **Ambos:** alguma função do v0 obriga o `sw-core` a alocar no caminho quente?

## Consequências
- (+) T2, T3 e T4 trabalham em paralelo na F1 contra um contrato estável e um stub que já linka.
- (+) Engines e bindings (C#, GDExtension) compilados contra o v0 continuam funcionando com bibliotecas v0.x.
- (−) Toda mudança, mesmo pequena, custa um ADR. É intencional: o custo é o aviso para quem depende.
- (−) `struct_size` exige disciplina no `sw-core` (ler só até o tamanho informado). Teste dedicado na F1.
- Verificação: no fim da F1, contar quantos ADRs de mudança o contrato precisou. Mais de 3 incompatíveis = o processo de revisão do contrato precisa melhorar antes da F2.
