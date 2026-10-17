# ADR-0016: Ponto de intercepção de leitura no Godot

**Status:** Proposta · **Data:** 2026-10-17 · **Autor:** @senior-engine (revisão pendente: 1 sênior + arquiteto)

## Contexto
[06](../06-integracao-engine-sdk.md) assumia que uma GDExtension implementaria `FileAccess`/`DirAccess` para que `res://` lesse pelo `sw-core` (F1-20). O [spike F0](../spikes/F0-spike-fileaccess-godot.md), executado no Godot 4.7.2, mostrou:
- `FileAccess` e `DirAccess` não têm métodos virtuais nem classe `…Extension` na API de extensões; o ponto real de intercepção (`PackedData`/`PackSource` e a tabela `FileAccess::create_func`) é interno ao núcleo C++.
- Um `ResourceFormatLoader` registrado na frente por GDExtension **funciona**: atende `load()`, cenas com `ext_resource`, carga em thread, e serve arquivos que não existem no disco. **Não** vê `FileAccess.open`, `ConfigFile` nem listagem de diretório.
- Cargas em thread usam tarefas de baixa prioridade do `WorkerThreadPool` (padrão 30% dos núcleos); um `_load` que espera rede derruba a vazão.

## Opções consideradas
1. **A. `ResourceFormatLoader` na frente (GDExtension)** + dicas e `ensure` assíncrono. Godot estoque, granularidade de recurso; não cobre leituras diretas.
2. **B. Mini-pacotes `.pck` por grupo**, montados com `load_resource_pack` quando o grupo está completo. Godot estoque, cobre tudo que lê `res://`, sem streaming por bloco (*não verificado no spike*).
3. **C. Módulo C++ / engine próprio** com `PackSource` do `sw-core`. Camada certa, granularidade de bloco; exige templates de export próprios.
4. **D. Contribuir upstream** a exposição de `FileAccess`/`PackSource` a extensões (ver godot-proposals#6307). Longo prazo, fora do nosso controle.
5. **E. Virtualização no SO** (ProjFS no Windows, FUSE no Linux). Serve qualquer engine; exige recurso opcional/privilégio e lê de forma bloqueante.
6. Descartadas: hook de `CreateFileW`/`ReadFile` (anti-cheat, só Windows) e alterar `create_func` por dentro da DLL (símbolo não exportado, quebra por versão).

## Decisão
1. **Caminho do PoC e do F1-20: A, com dicas.** O `sw-godot` registra o loader na frente para os tipos pesados. O `_load` **nunca espera a rede**: o `sw-core` garante os dados antes (`sw_hint_*`/`ensure` assíncrono); o `_load` entrega ou aplica a política de falta. A lista de dependências de cada cena vem do `sw-pack` (plugin de export).
2. **Regra de projeto para o estúdio:** tudo que o jogo lê por `FileAccess`, `ConfigFile` ou `DirAccess` fica no grupo `core`. O plugin de editor sinaliza essas leituras quando detectar.
3. **B é a reserva**, a ser verificada antes do fim da F1: se A mostrar buracos que o `core` não resolve, o conteúdo de um grupo vira `.pck` montado em runtime.
4. **Protótipo C de ~1 semana na F1** (módulo com `PackSource` sobre o mock do `sw-core`), só para **medir o teto** de latência e de cobertura. Não é caminho de produto. Critério: se C reduzir a falta em ≥ 2× sobre A no mapa da F0-09, abrir conversa upstream (D) e avaliar templates próprios; caso contrário, A é definitivo.
5. **E entra na avaliação antes da Fase 6** como trilha independente de engine.
6. Fixar a versão do Godot no F0-13 e **repetir o spike nessa versão** (inclui export e Linux) antes de congelar o `sw-godot`.

## Consequências
- (+) Funciona hoje em Godot estoque; o estúdio instala só uma extensão.
- (+) Valida cedo o contrato `ensure` assíncrono, que serve às outras engines.
- (−) Cobertura parcial: leituras diretas não passam pelo sistema, e a disciplina do `core` vira requisito do estúdio.
- (−) O limite de threads de carga exige o padrão `ensure` assíncrono; mudar `low_priority_thread_ratio` é só paliativo.
- (−) Pendências do spike: export com `template_release`, Linux/macOS, recarga a quente, B, C e E.
- **Verificação:** F1-20 entrega o mapa da F0-09 carregando pelo loader; medir faltas por minuto e p95 de espera ([08](../08-metricas-e-testes.md)). Revisitar este ADR no portão da Fase 1.
