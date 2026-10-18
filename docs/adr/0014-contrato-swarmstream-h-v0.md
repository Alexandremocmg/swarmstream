# ADR-0014: Contrato `swarmstream.h` v0 e regras de compatibilidade da API

**Status:** Aceita (2026-10-17: revisão R1, leitura independente, aprovação do dono do produto) · **Data:** 2026-10-17 · **Autor:** R1 (líder técnico)

**Revisão (regra de 3 pessoas, [15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)):** R1 revisou · leitura e comentários: revisor independente · aprovação: dono do produto (2026-10-17). Registro: [revisão F0](../revisoes/F0-revisao-adrs-0010-0016.md)

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
4. **Layout travado por teste:** `sw-core/tests/api_v0_smoke.c` e `api_v0_cxx.cpp` verificam `sizeof` **e** `offsetof` das structs. O v0 só suporta plataformas de 64 bits (Windows x64, Linux x86_64/arm64); o teste falha em 32 bits. Teste que quebra = mudança de contrato.
   - `sw_allocator` e `sw_vec3` não têm `struct_size`: o layout delas fica congelado para sempre. Precisar de mais campos = struct nova.
5. **Sem modo de manifesto sem assinatura** ([ADR-0012](0012-serializacao-do-manifesto.md)): a F1 usa a chave de teste em `trusted_keys`.
6. **Fora do v0, de propósito** (entram por ADR quando a fase precisar): instalação e modo completo (pin de grupos, sem remoção) para o launcher (F4); cifra por pacote e chave de licença (F4); troca de manifesto em execução (patch com o jogo aberto); eventos de telemetria por callback.

## Processo de revisão e congelamento

O plano original pedia dois sêniores. Com 3 recursos ([15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)), vale a regra de 3 pessoas, com dois olhos obrigatórios porque a API pública é código crítico:

| Etapa | O quê | Quem |
| --- | --- | --- |
| 1 | Revisão R1 e correções (residência com 0 seguro, `SW_PARTIAL`, regras de callback) | R1 |
| 2 | Leitura e comentários; achados aplicados ou recusados com motivo no [registro da revisão](../revisoes/F0-revisao-adrs-0010-0016.md) | Revisor independente |
| 3 | Aprovação | Dono do produto |
| 4 | **Congelamento:** este ADR passa a "Aceita", o header ganha a marca "CONGELADO", e mudanças em `/sw-core/include/` passam a exigir ADR e aprovação de R1 + R2 no PR | R1 |

Se o congelamento não acontecer, a Fase 1 não começa ([14](../14-briefing-fase-0.md#se-alguém-atrasar)).

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
