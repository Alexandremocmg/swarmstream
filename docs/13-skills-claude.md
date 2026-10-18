# Skills do Claude por nível e trilha

O projeto tem 31 skills do Claude instaladas em [`.claude/skills/`](../.claude/skills/). Elas valem para **qualquer pessoa** que abrir o Claude Code nesta pasta: o Claude carrega a skill sozinho quando o pedido combina com a descrição dela. Também dá para chamar pelo nome (ex.: `/systematic-debugging`).

As skills não são separadas por nível no disco. A tabela abaixo diz **quais cada nível deve usar como rotina**, de acordo com as tarefas que cada nível assume em [10-equipe-e-niveis.md](10-equipe-e-niveis.md). Origem, versões e licenças: [`.claude/skills/FONTES.md`](../.claude/skills/FONTES.md).

## Por nível

### Todos os níveis (processo de trabalho)
| Skill | Para quê |
| --- | --- |
| `test-driven-development` | Escrever o teste antes do código (ciclo vermelho → verde → refatorar). |
| `systematic-debugging` | Achar a causa de um bug antes de propor correção. |
| `verification-before-completion` | Rodar e mostrar a verificação antes de dizer que terminou. Combina com a regra "medir antes de afirmar". |
| `requesting-code-review` | Preparar o PR para revisão. |
| `receiving-code-review` | Responder a revisões com verificação técnica, não com concordância automática. |

### Júnior
Além das de todos os níveis:
| Skill | Tarefas típicas do backlog |
| --- | --- |
| `godot-project-setup` | F0-09 (projeto Godot de teste) |
| `assets-pipeline` | F0-09 (importar assets CC0 com a compressão certa) |
| `godot-debugging` | F1-24 (overlay), F1-25 (traces) |
| `godot-testing` | Testes do lado Godot |
| `python-pro` | F0-11 (servidor de rede simulada), F1-33 (script de relatório) |
| `property-based-testing` | F1-02 (testes do chunker com entradas geradas) |

### Pleno
Além das de júnior:
| Skill | Tarefas típicas do backlog |
| --- | --- |
| `cpp-pro`, `modern-cpp` | Todo código C++ do `sw-core` e do `sw-pack` |
| `cmake`, `conan-vcpkg` | F0-02 (build), F2-01 (libtorrent no build) |
| `static-analysis` | F0-03, F0-04 (clang-tidy no CI) |
| `sanitizers` | F0-03 (ASan/UBSan no CI), testes de queda |
| `addon-development` | Plugin de editor Godot (F4) |
| `export-pipeline` | Exportar o mapa de teste e builds de demonstração |
| `writing-plans` | Quebrar uma funcionalidade inteira em passos antes de codar |

### Sênior
Além das de pleno:
| Skill | Tarefas típicas do backlog |
| --- | --- |
| `gdextension` | F1-20 (`ResourceFormatLoader` na frente sobre o `sw-core`, ADR-0016) |
| `multithreading` | F1-20, F1-21 (carregamento em thread no Godot sem travar o frame) |
| `godot-optimization` | Medir stutter e pop-in no Portão 1 |
| `memory-model` | F1-12, F1-13 (fila MPSC, leases com atômicos) |
| `concurrency-debugging` | F1-12 (zerar data races no TSan) |
| `flamegraphs` | Achar gargalos no caminho quente de leitura |
| `harness-writing`, `libfuzzer` | F2-07 (fuzzing do manifesto e das mensagens de rede) |
| `db-acid-in-practice` | F1-11 (journal: dados → flush → journal → flush, recuperação após queda). É teoria de banco de dados, aplicada ao nosso cache. |
| `boost-asio-pro` | F1-27, F2-04 (código de rede assíncrono em C++ com Asio, strands e coroutines). Não cobre QUIC. |

### Arquiteto / tech lead
Além das de sênior:
| Skill | Uso |
| --- | --- |
| `architecture-designer` | Escrever e revisar ADRs, avaliar trocas de tecnologia (F0-05) |

## Por trilha

| Trilha | Skills principais |
| --- | --- |
| T1 Conteúdo/build | `cpp-pro`, `modern-cpp`, `property-based-testing`, `harness-writing` |
| T2 Cache/VFS | `db-acid-in-practice`, `memory-model`, `concurrency-debugging`, `sanitizers`, `flamegraphs` |
| T3 Engine | `gdextension`, `multithreading`, `godot-optimization`, `godot-debugging`, `godot-testing`, `assets-pipeline`, `addon-development` |
| T4 Rede | `boost-asio-pro`, `cpp-pro`, `memory-model`, `libfuzzer` |
| T5 Serviços | (aguarda o ADR da linguagem de backend) |
| T6 Bancada/QA | `python-pro`, `cmake`, `static-analysis`, `sanitizers`, `export-pipeline` |
| T7 Launcher | (aguarda o ADR da tecnologia do launcher) |
| T8 Segurança | `libfuzzer`, `harness-writing`, `sanitizers`, `property-based-testing` |

## Lacunas: pesquisei no GitHub e não existe skill pública para
- **libtorrent / BitTorrent** (storage customizado, `set_piece_deadline`, web seeds). Só existe uma de WebTorrent, que é JavaScript.
- **NAT traversal / P2P** (STUN, hole punching, uTP). As skills de libp2p que achei são documentação interna de um projeto em Rust e o NAT lá está como pendente.
- **FastCDC, Merkle e formato de manifesto.** Só há bibliotecas (ex.: `fastcdc` em Rust), úteis como referência de código.
- **Cache em disco com journal.** Coberto só em parte, pela teoria de WAL da `db-acid-in-practice`; falta a parte de slots, leases e remoção.
- **QUIC em C++.** A `boost-asio-pro` cobre TCP/UDP, não QUIC.

Para essas, a sugestão é **escrever skills próprias do projeto** à medida que a equipe aprende (com a skill `skill-creator`, já disponível no Claude), começando pelos docs [03](03-formato-de-conteudo.md), [04](04-cache-local.md) e [05](05-rede-e-agendador.md).

## Instaláveis depois, como plugin
- **Trail of Bits `c-review`** (revisão de segurança de C/C++ com vários agentes): depende do plugin completo. Instalar com `/plugin marketplace add trailofbits/skills` num terminal `claude`, quando houver código para auditar (F2).
- **Skills de backend** (`golang-pro` ou `rust-engineer`, do mesmo repositório do `cpp-pro`): instalar quando o ADR de backend for aceito.

## Regras
- Skill nova entra só depois de revisada (sem instruções suspeitas nem scripts desconhecidos) e registrada em `FONTES.md`.
- Atualizar uma skill = comparar com a versão nova antes de copiar.
