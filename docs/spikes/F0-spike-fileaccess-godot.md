# Spike F0: interceptar a leitura de arquivos no Godot 4 via GDExtension

**Status: executado em Godot 4.7.2 estável (Windows, build debug da extensão, `godot-cpp` tag 4.5).** O hello world e a tabela de cobertura (§3.1) são resultados **medidos**. Não foram feitos: export (faltam os templates de exportação), recarga a quente da extensão, Linux/macOS e as abordagens B, C e E. Código e logs: [spike-fileaccess/](../../spike-fileaccess/).

## 1. Resposta curta

**Em Godot 4 estoque, uma GDExtension não consegue registrar um `FileAccess` próprio para `res://`.** O ponto de intercepção real do engine é interno ao núcleo C++ e não é exposto à API de extensões. **Confirmado no `--dump-extension-api` do 4.7.2:** `FileAccess` e `DirAccess` têm 0 métodos virtuais e não existe `FileAccessExtension`; já `ResourceFormatLoader` tem 11 virtuais (`_load`, `_recognize_path`, `_exists`...). Isso contradiz a premissa de [06](../06-integracao-engine-sdk.md) ("GDExtension implementa `FileAccess`/`DirAccess` customizados") e do F1-20, e precisa de decisão (ADR-0016 (proposta): [ponto de intercepção do Godot](../adr/0016-ponto-de-intercepcao-godot.md)).

## 2. O que foi verificado no código do engine

| Fato | Evidência | Consequência |
| --- | --- | --- |
| Todo `FileAccess::open(path)` tenta primeiro `PackedData::get_singleton()->try_open_path(path)` e só depois `create_for_path()` | `file_access.cpp` (master) | O gargalo único do engine é o **`PackedData`/`PackSource`** (os leitores de `.pck`) |
| `create_for_path` escolhe o backend pelo prefixo: `res://` e `uid://` → `ACCESS_RESOURCES`, `user://` → `ACCESS_USERDATA`, `pipe://` → `ACCESS_PIPE`, resto → `ACCESS_FILESYSTEM` | idem | Não há prefixo configurável por extensão |
| Backends são registrados em `FileAccess::create_func[ACCESS_MAX]` via `make_default<T>()`, função **estática C++** | `file_access.h` (master) | Só código compilado dentro do engine (ou com acesso ao símbolo) troca isso |
| `file_access.h` não tem `GDVIRTUAL` nem existe `FileAccessExtension` | `file_access.h` (master) | Nada para uma GDExtension sobrescrever; os ~20 métodos virtuais puros são C++ comuns |
| Existe proposta aberta de "custom resource path" com classes `FileAccess`/`DirAccess` registráveis | [godot-proposals#6307](https://github.com/godotengine/godot-proposals/issues/6307) | Demanda conhecida; **não** encontrei PR de GDExtension para isso (busca limitada; rechecar na versão fixada) |

Ressalvas: (1) as leituras vieram resumidas por uma ferramenta de busca, não de um checkout; (2) a versão do Godot do projeto ainda não foi fixada (F0-13) — repetir a checagem de `file_access.h` no tag exato; (3) o que a GDExtension pode **usar** de `FileAccess` (abrir e ler arquivos) é diferente do que ela pode **implementar**.

## 3. Abordagens avaliadas

| # | Abordagem | Cobre | Stock Godot? | Granularidade | Veredito |
| --- | --- | --- | --- | --- | --- |
| A | **`ResourceFormatLoader` registrado na frente** (`add_resource_format_loader(loader, true)`) numa GDExtension | `load()`, `ResourceLoader.load_threaded_*`, `ext_resource` de cenas | Sim | Recurso inteiro | Viável para o PoC. **Não** cobre `FileAccess.open` direto, `ConfigFile`, nem o que o importador/engine lê por conta própria |
| B | **Mini-pacotes `.pck` por grupo** montados com `ProjectSettings.load_resource_pack()` quando o grupo está completo (modelo de DLC) *(não verificado nesta rodada)* | Tudo que o engine lê de `res://` | Sim | Grupo/região (MBs a GBs) | Plano B robusto, mas perde o streaming por bloco e exige remontar um arquivo contíguo no disco |
| C | **Módulo C++ / fork do engine** com `PackedData::add_pack_source(new SwarmPackSource)` | Tudo, na camada certa | **Não** (templates de export próprios) | Bloco (ideal) | É a solução limpa tecnicamente e a que mede o teto do que o produto consegue; custo: o estúdio precisa usar nosso build do engine |
| D | **Contribuir upstream** a exposição de `FileAccess`/`PackSource` a GDExtension (ligada a #6307) | Tudo | Sim, no futuro | Bloco | Longo prazo, fora do nosso controle; vale abrir a conversa cedo |
| E | **Virtualização no nível do SO** (Windows ProjFS ou Cloud Files API, Linux FUSE) na pasta do jogo *(não verificado)* | Qualquer engine, até `FileAccess` e o próprio `.pck` | Sim | Arquivo/faixa de bytes | Alternativa forte e independente da engine (serve Unreal/Unity na Fase 6). Riscos: ProjFS é recurso opcional do Windows (pede habilitação), FUSE no Linux/macOS, leitura bloqueante na thread que lê, interação com antivírus/anti-cheat |
| F | Hook de `CreateFileW/ReadFile` por injeção (MinHook) | Tudo | Sim | Faixa de bytes | Descartar: frágil, só Windows, anti-cheat trata como malware |
| G | Alterar o ponteiro `FileAccess::create_func` por dentro da DLL | Tudo | Sim | Arquivo | Descartar: depende de símbolo não exportado e de layout por versão |

### 3.1 Resultado medido da abordagem A (loader na frente)

Extensão em C++ (`SwarmFormatLoader`, `add_resource_format_loader(loader, true)`) que reivindica `swarm://` e `res://claimed/**` e devolve um `SwarmBlob` com bytes de um mock do `sw-core`. Roteiro: [spike_test.gd](../../spike-fileaccess/project/scripts/spike_test.gd); saída: [run_final.log](../../spike-fileaccess/out/run_final.log).

| Teste | Resultado | Leitura |
| --- | --- | --- |
| `load("swarm://hello.txt")` | `SwarmBlob`, texto do mock | **Hello world de classe própria funciona** |
| `load("res://claimed/a.txt")` (existe no disco) | Loader chamado; devolve o mock, não o arquivo | O loader na frente tem precedência sobre o arquivo real |
| `load("res://claimed/<não existe no disco>")` | Loader chamado, devolve o recurso; `ResourceLoader.exists` = true | **Serve arquivo que não está no disco**, caso de uso do streaming |
| Cena `.tscn` com `ext_resource` para `claimed/` (existente ou só virtual) | O loader é chamado e o recurso entra na cena | Dependências de cena passam pelo loader |
| `load_threaded_request` (virtual e existente) | `_load` roda em **thread de worker**; status `LOADED` | Funciona com carregamento em thread |
| `FileAccess.open("res://claimed/a.txt")` | Lê o arquivo **real**; loader não é chamado | **Não intercepta** `FileAccess` |
| `ConfigFile.load("res://claimed/c.cfg")` | Lê o arquivo real; loader não é chamado | Não intercepta |
| `FileAccess.file_exists` em caminho só-virtual | `false` | Código que testa existência por `FileAccess` não vê o arquivo |
| `FileAccess.open("swarm://x")` / `DirAccess.open("swarm://")` | `null` (erro 7) / `null` | `swarm://` só existe para `ResourceLoader`; não há listagem de diretório |
| `load("res://plain/tex.png")` (fora de `claimed/`) | `CompressedTexture2D`, loader não chamado | Importador normal intacto fora dos caminhos reivindicados |
| `load("res://claimed/tex.png")` | Loader chamado, devolve `SwarmBlob` | Dentro do prefixo reivindicado, o loader substitui o importador (precisaria delegar) |

**Achado extra com impacto direto no desenho:** as cargas em thread rodam em *tarefas de baixa prioridade* do `WorkerThreadPool`, limitadas por `threading/worker_pool/low_priority_thread_ratio` (padrão **0,3**). Com 8 núcleos, 16 pedidos de 200 ms de espera levaram **1 715 ms** (concorrência efetiva 2); com a razão em 0,9 levaram **627 ms** ([parallelism_default.log](../../spike-fileaccess/out/parallelism_default.log); o de 0,9 foi medido na hora e não ficou em log). Um `_load` que bloqueia na rede sufoca o carregamento do jogo. Por isso o `_load` não deve esperar rede: o `sw-core` garante os dados antes (dica/`ensure` assíncrono) e o `_load` só entrega, ou aplica a política de falta. A ligação entre o 0,3 e o limite de 2 é a explicação mais provável (o número bate com 8 × 0,3), mas não li o código do `WorkerThreadPool` para confirmar.

Limites do teste: a extensão foi compilada contra o `godot-cpp` 4.5 e carregou no editor 4.7.2 (compatibilidade para frente, como a skill descreve); um único Windows, build debug; mock em memória em vez do `sw-core`.

## 4. Recomendação

1. **PoC e F1-20: A + dicas explícitas**, com **B como reserva**. Desenho da A: o `sw-godot` registra um `ResourceFormatLoader` na frente para os tipos pesados (texturas importadas, malhas, áudio); em `_load()` ele chama `sw_read`/`sw_ensure` sobre o `sw-core` e entrega o recurso ou aplica a política de falta ([06](../06-integracao-engine-sdk.md#política-de-falta)). O teste mostrou que as dependências de uma cena **passam** pelo loader, mas o `_load` não pode esperar a rede (limite de threads, §3.1); a lista de dependências para o `ensure` antecipado vem do `sw-pack` no export. Leituras diretas por `FileAccess`/`ConfigFile` **não** passam pelo loader: tudo que o jogo ler assim precisa estar no `core`.
2. **Em paralelo, um protótipo C** (módulo com `PackSource`) por ~1 semana na F1 para **medir o teto**: se o ganho de latência de C sobre A for pequeno, o produto fica em stock Godot; se for grande, vira argumento para D (upstream) ou para distribuir templates próprios.
3. **Avaliar E** como trilha engine-agnóstica antes da Fase 6; é o que realmente resolve Unreal e Unity.
4. **Responder a pergunta aberta de [06](../06-integracao-engine-sdk.md#questões-em-aberto)** ("`FileAccess` customizado ou `.pck` virtuais?") com: *`.pck`/`PackSource` é o ponto real; via GDExtension só dá para ficar acima dele (A, B).*

## 5. O que foi feito dos 2 dias e o que falta

| Item | Estado |
| --- | --- |
| Godot 4.7.2 (SHA-512 conferido) e `godot-cpp` tag 4.5 baixados em `spike-fileaccess/` | Feito |
| Extensão compilada (CMake + MinGW) e carregada | Feito |
| Hello world: `load("swarm://hello.txt")` por classe própria | Feito |
| Tabela do que o loader intercepta / não intercepta | Feito (§3.1) |
| Cargas em thread e limite de concorrência | Feito (§3.1) |
| Export com `template_release` (a extensão existe no jogo exportado?) | **Falta** (templates de exportação não baixados) |
| Recarga a quente (`reloadable`), Linux, macOS, MSVC | **Falta** |
| Abordagens B (`.pck` por grupo), C (`PackSource` em módulo) e E (ProjFS/FUSE) | **Falta**; C é o protótipo de ~1 semana da §4 |
| Repetir a leitura de `file_access.h/.cpp` no tag 4.7.2 (a de `master` veio de resumo web; o dump da API confirma só a superfície exposta) | **Falta** |

Reproduzir: `cmake -S . -B build -G Ninja && cmake --build build`, depois `Godot_v4.7.2-stable_win64_console.exe --headless --path project --import --quit` e `--script res://scripts/spike_test.gd`. Em `spike-fileaccess/`, `godot-cpp/` e `tools/` são baixados, não versionados.

## 6. Riscos

| Risco | Prob. | Impacto | Mitigação |
| --- | --- | --- | --- |
| A não cobre `FileAccess`/`ConfigFile` (medido), e o jogo lê dado ausente ou arquivo "inexistente" | Alta | Alto | Dicas + `sw-pack` marcando o `core`; B como reserva; testes que leem tudo no mapa |
| Quebra de ABI/API entre versões do Godot (`compatibility_minimum`, mudanças em 4.7: `is_class`, funções de cast) | Média | Médio | Fixar a versão e o branch do `godot-cpp`; CI com a versão fixada |
| `_load` bloqueante em carga em thread: só ~30% dos núcleos executam cargas (medido) | **Alta** | Alto | Padrão "ensure assíncrono, `_load` só entrega"; subir `low_priority_thread_ratio` no projeto do estúdio é paliativo |
| Fork do engine (C) afasta o piloto indie | Alta | Alto | Só usar C como medidor ou como opção explícita |
| Windows: `mmap` e antivírus diferem do Linux | Média | Médio | Medir no Windows desde o dia 1 |
| ProjFS/FUSE (E) exigem privilégio ou recurso opcional | Média | Médio | Fase 4–6, não bloqueia o PoC |
| A leitura de `file_access.cpp` veio de resumos web; vale conferir no tag 4.7.2 | Baixa | Médio | Checar o código no tag antes de fechar o ADR |

## 7. Fontes
- Godot `core/io/file_access.h` e `file_access.cpp` (branch `master`), lidos por busca em 2026-10-17.
- [godot-proposals#6307 — custom resource path support](https://github.com/godotengine/godot-proposals/issues/6307).
- [godot-proposals#13727 — FileAccess instances em métodos de IO](https://github.com/godotengine/godot-proposals/issues/13727).
- Documentação de `ResourceFormatLoader`/`ResourceLoader` (godot docs, 4.x).
- `--dump-extension-api` do Godot 4.7.2 e execução local descritos na §5.
- Skill `gdextension` do projeto (estrutura, `.gdextension`, regras de compatibilidade).
