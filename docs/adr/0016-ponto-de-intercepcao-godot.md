# ADR-0016: Ponto de intercepção de leitura no Godot

**Status:** Aceita (2026-10-17: revisão R1, leitura independente, aprovação do dono do produto) · **Data:** 2026-10-17 · **Autor:** autor do spike F0 (trilha de engine)

**Revisão (regra de 3 pessoas, [15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)):** R1 revisou e ajustou (marcado como "ajuste da revisão") · leitura e comentários: revisor independente · aprovação: dono do produto (2026-10-17). Registro: [revisão F0](../revisoes/F0-revisao-adrs-0010-0016.md)

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
1. **Caminho do PoC e do F1-20: A, com dicas.** O `sw-godot` registra o loader na frente para os tipos pesados. O `_load` **nunca espera a rede**: o `sw-core` garante os dados antes (`sw_hint_*` para prefetch e `sw_ensure_async`, que prende os bytes no cache até o `sw_close`; ver [`swarmstream.h`](../../sw-core/include/swarmstream.h)); o `_load` lê com `timeout_ms = 0` e entrega, ou aplica a política de falta se receber `SW_E_NOT_LOCAL`. A lista de dependências de cada cena vem do `sw-pack` (plugin de export).
   - **Materialização (ajuste da revisão):** o spike provou que o loader **intercepta**, mas devolveu bytes crus (`SwarmBlob`). Os loaders nativos de `CompressedTexture2D` e de recursos binários (`.res`, `.scn`, `.mesh`) abrem o arquivo por caminho via `FileAccess`, exatamente o que A não intercepta. Por isso o loader **não delega** ao loader nativo: ele constrói o recurso a partir de bytes, com APIs de buffer (`Image.create_from_data` com formatos BCn e `ImageTexture`/`PortableCompressedTexture2D` para texturas, `ArrayMesh.add_surface_from_arrays` para malhas, `load_from_buffer` dos `AudioStream*` para áudio). O `sw-pack` exporta esses tipos num formato que o loader sabe ler. Cenas, scripts e recursos pequenos ficam no `core` e passam pelo caminho normal do Godot.
   - **Troca quando o dado melhor chega (ajuste da revisão):** o `ResourceLoader` guarda o recurso no cache dele, então a política de falta não pode depender de carregar de novo. O loader devolve um recurso que ele mesmo atualiza no lugar (`ImageTexture.update`/`set_image`, troca da superfície da malha) quando o `sw_ensure_async` do nível melhor termina, sempre na thread principal (`call_deferred`).
2. **Regra de projeto para o estúdio:** tudo que o jogo lê por `FileAccess`, `ConfigFile` ou `DirAccess` fica no grupo `core`. O plugin de editor sinaliza essas leituras quando detectar.
3. **B é a reserva**, a ser verificada antes do fim da F1: se A mostrar buracos que o `core` não resolve, o conteúdo de um grupo vira `.pck` montado em runtime.
4. **Protótipo C de ~1 semana na F1** (módulo com `PackSource` sobre o mock do `sw-core`), só para **medir o teto** de latência e de cobertura. Não é caminho de produto. Critério (ajustado na revisão): mesma rota, perfil `cabo_50`, 3 rodadas, comparar **taxa de falta** e **tempo de pop-in** ([08](../08-metricas-e-testes.md)); a espera p95 não serve, porque em A o `_load` nunca espera por construção. Se C melhorar uma das duas em ≥ 2× sobre A no mapa da F0-09, abrir conversa upstream (D) e avaliar templates próprios; caso contrário, A é definitivo.
5. **E entra na avaliação antes da Fase 6** como trilha independente de engine.
6. Fixar a versão do Godot no F0-13 e **repetir o spike nessa versão** (inclui export e Linux) antes de congelar o `sw-godot`.

## Consequências
- (+) Funciona hoje em Godot estoque; o estúdio instala só uma extensão.
- (+) Valida cedo o contrato `ensure` assíncrono, que serve às outras engines.
- (−) Cobertura parcial: leituras diretas não passam pelo sistema, e a disciplina do `core` vira requisito do estúdio.
- (−) O limite de threads de carga exige o padrão `ensure` assíncrono; mudar `low_priority_thread_ratio` é só paliativo.
- (−) Pendências do spike: export com `template_release`, Linux/macOS, recarga a quente, B, C e E.
- (−) O `sw-pack` passa a exportar texturas, malhas e áudio num formato próprio legível por buffer, em vez dos arquivos importados do Godot (`.ctex`, `.mesh`). Mais código no plugin de export.
- **Portão obrigatório (ajuste da revisão), primeira entrega do F1-20, antes de começar F1-21/F1-22:** a partir de bytes servidos pelo mock do `sw-core`, sem `FileAccess`, o loader materializa (1) uma textura BCn 4K com mips, (2) uma malha com 2 LODs e (3) um áudio OGG, todos visíveis e audíveis numa cena do mapa da F0-09, e troca o mip/LOD no lugar sem recarregar. Se algum dos três não for possível em Godot estoque, a decisão 1 cai para B para aquele tipo, por um ADR novo com o resultado.
- **Verificação:** F1-20 entrega o mapa da F0-09 carregando pelo loader; medir faltas por minuto, tempo de pop-in e quadros > 33 ms ([08](../08-metricas-e-testes.md)). Revisitar este ADR no portão da Fase 1.
