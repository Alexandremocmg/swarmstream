# Parecer do portão da Fase 0 e plano para 3 recursos

**Status: plano autorizado pelo dono do produto em 2026-10-17** (cronograma para 3 recursos, adiamento do backend hospedado para depois do Portão 2 e regra de revisão com 3 pessoas). **Recursos confirmados:** R1 líder técnico sênior, R2 pleno de engenharia e qualidade, R3 júnior de conteúdo e apoio. O fechamento da Fase 0 (lista de 8 itens) continua sendo a condição para entrar na Fase 1.

Data: 2026-10-17. Base: as quatro respostas da equipe (mapa e rede simulada, interfaces e mocks, baseline e métricas, spec do manifesto e spike do Godot, contratos e ADRs) **mais a verificação feita no disco** (arquivos, build e testes refeitos do zero).

## Veredito

**Ainda não passar para a Fase 1. Aprovar com condições: fechar uma lista curta (≈ 1 semana com 3 recursos) e então passar.**

O trabalho técnico da Fase 0 está, em grande parte, entregue e de boa qualidade. O que falta é principalmente **fechamento de processo**, mais **um problema de método na medição** que, se levado adiante, deixaria o Portão 1 sem valor. Nada do que falta é caro, mas o critério de saída da fase não está cumprido.

> Critério de saída (de [09](09-roadmap-fases.md#f0--fundação--3-semanas)): *"um desenvolvedor novo clona, compila e roda o mapa de teste em menos de 1 hora seguindo o CONTRIBUTING."* Hoje isso **não é verificável**: não há repositório git, o CI nunca rodou, e o guia de ambiente ainda diz "a completar".

## O que foi verificado no disco

| Item | Resultado |
| --- | --- |
| Build do zero (CMake + Ninja + GCC 16 MinGW, Debug) | Configura e compila sem erro |
| Testes do `sw-core` | `sw-core-tests` passa (1 de 1). Confere com o relatório da equipe |
| Arquivos de projeto aberto (`LICENSE`, `NOTICE`, `SECURITY.md`) | Existem |
| `.clang-format`, `.clang-tidy`, `CODEOWNERS`, modelo de PR, workflows `ci.yml` e `lint.yml` | Existem |
| Repositório git | **Não existe** (`fatal: not a git repository`) |
| CI em Windows e Linux | **Nunca rodou**: sem git e sem GitHub não há execução. Só foi testado localmente, com MinGW, não com MSVC |
| `CODEOWNERS` | Usa apelidos de função (`@senior-engine`), não usuários reais |
| `.gitattributes` | Não existe (arquivos misturam CRLF e LF) |
| `CONTRIBUTING.md` | Seção "Ambiente" ainda diz "a completar na tarefa F0-13" |
| `REA-Estudo/` (JDK, Ghidra, node_modules) | Está na raiz do projeto e **não está no `.gitignore`**: entraria no primeiro commit |

## Tarefa por tarefa

| Tarefa | Estado | Observação |
| --- | --- | --- |
| F0-01 Repositório | **Parcial** | Pastas prontas; falta `git init`, `.gitattributes`, ignorar `REA-Estudo/` |
| F0-02 Build | **Feito (local)** | Falta provar com MSVC e no Linux |
| F0-03 CI | **Escrito, não executado** | Não dá para dizer que "PR quebrado fica vermelho" |
| F0-04 Lint e padrões | **Feito** | Black, Ruff, mypy estrito passam; clang-format/tidy configurados |
| F0-05 ADRs pendentes | **Escritos, não aceitos** | 0010–0016 em "Proposta"; revisão marcada para 23/10 |
| F0-06 `swarmstream.h` v0 | **Feito, não congelado** | Compila em C99 e C++20, exporta 23 funções; congelamento previsto para 28/10 |
| F0-07 Interfaces e mocks | **Feito** | Testes passam, incluindo o de integração pedido → fonte → cache → leitura com lease |
| F0-08 Spec do manifesto | **Feito (v0)** | Vetores só para hash de bloco e Merkle; faltam envelope assinado, codecs e chunker |
| F0-09 Mapa de teste | **Feito, com ressalva** | Ver "Problema de método" abaixo |
| F0-10 Rota reprodutível | **Feito** | Duas execuções com tempos e coordenadas iguais; `route.log` idêntico nas 5 rodadas |
| F0-11 Servidor de rede | **Feito** | Os 3 perfis ficam dentro de ±3% (limite era ±10%) |
| F0-12 Baseline | **Parcial** | Carga e FPS confiáveis; stutter e carga a frio **não provados** |
| F0-13 Guia de ambiente | **Não feito** | Bloqueia o critério de saída |
| Spike do `FileAccess` | **Feito** | Resultado importante, abaixo |

## Três achados que mudam o plano

### 1. A premissa do `FileAccess` estava errada (ADR-0001 e doc 06)
O spike mostra que, no Godot estoque, uma GDExtension **não consegue** implementar o `FileAccess`. O que funciona é um `ResourceFormatLoader` na frente: atende `load()`, cenas com `ext_resource` e cargas em thread, mas **não** vê `FileAccess.open`, `ConfigFile` nem listagem de diretório.

Consequência: a tarefa F1-20 ("`FileAccess` customizado") não pode ser feita como foi escrita. O [ADR-0016](adr/0016-ponto-de-intercepcao-godot.md) propõe o caminho certo (loader na frente + dicas, `.pck` por grupo como reserva, e um protótipo de módulo C++ de cerca de 1 semana só para medir o teto). Minha avaliação: a proposta é sensata e deve ser aceita. O ADR-0001 continua válido (Godot + libtorrent), mas o trecho "`FileAccess` interceptável" precisa ser corrigido.

Ressalvas do próprio spike: a leitura do código do engine veio de resumos, não de um checkout do tag 4.7.2; export, Linux e macOS não foram testados; a explicação para as cargas em thread usarem só ~30% dos núcleos (e a melhora com `low_priority_thread_ratio = 0,9`) é provável, não confirmada.

### 2. A medição não separa leitura de disco de decodificação (problema de método)
A carga do mapa leva ~21,5 s e **não muda** com cache frio (21,45 e 21,52 s, dentro da faixa das quentes). Ou a expulsão do cache do Windows falhou, ou a carga é limitada pela **decodificação de JPEG 8K na CPU**, e não pelo disco. Se for a segunda, o baseline "lendo do SSD" **não mede I/O**, e o Portão 1 não poderia concluir nada sobre streaming.

Além disso, o critério da Fase 1 "stutter ± 5% do baseline" **não é verificável**: com 3 quadros lentos por rodada, 5% não significa nada. O tempo acima de 33 ms varia 51% entre rodadas.

Correções propostas (baratas):
- Converter as texturas para um formato GPU comprimido (BC/ETC ou o formato importado do Godot) antes de medir, para que a carga dependa de leitura e não de decodificação de JPEG.
- Trocar o critério por limites absolutos (ex.: quadros lentos ≤ baseline + N e p99 ≤ baseline + 2 ms), como o próprio relatório sugere. Quem define é o responsável técnico.
- Repetir a carga a frio com reinício do Windows ou limpeza de cache com administrador, **uma vez**.

### 3. O manifesto estoura a meta de tamanho (spec §7.3)
Com blocos médios de 128 KiB, um jogo de 150 GB gera cerca de 54 MB de manifesto, acima da meta de 50 MB. A spec já lista saídas (blocos médios de 192 KiB, hash cortado, etc.). **Não bloqueia a Fase 0**, mas precisa de decisão por ADR no início da Fase 1, porque muda o ADR-0002 (aceito).

## Lista para fechar a Fase 0 (≈ 1 semana)

| # | Item | Quem (ver abaixo) | Pronto quando |
| --- | --- | --- | --- |
| 1 | `git init`, `.gitignore` para `REA-Estudo/`, `build*/`, `.venv`, `.godot-version` mantido; `.gitattributes` (fim de linha); primeiro commit com `LICENSE`, `NOTICE`, `SECURITY.md` **(feito e enviado)** | Pleno | Árvore limpa; nada de JDK, Ghidra ou node_modules no commit |
| 2 | Repositório no GitHub (criado), `CODEOWNERS` com usuários reais, e e-mail de segurança no `SECURITY.md` (feito) | Dono do produto + Pleno | `CODEOWNERS` sem apelidos de função |
| 3 | CI rodando em Windows e Linux, **incluindo um PR de teste que quebra e fica vermelho** | Pleno | Captura do PR vermelho e do PR verde |
| 4 | Guia de ambiente (F0-13) e **teste do critério de 1 hora** com alguém que não participou da montagem | Júnior + um revisor externo ao guia | Cronômetro registrado em `docs/medicoes/` |
| 5 | Corrigir o método do baseline: formato GPU comprimido e limites absolutos de stutter; uma carga fria com reinício | Pleno (com o sênior) | `baseline.md` com carga fria claramente maior que a quente, ou conclusão documentada de que a cena não mede I/O |
| 6 | Revisar e aceitar ADRs 0010–0016 (ver regra de revisão abaixo) e congelar `swarmstream.h` v0 | Arquiteto/sênior | Nenhum ADR da F0 em "Proposta" |
| 7 | Corrigir ADR-0001, docs 06 e o backlog F1-20 conforme o ADR-0016 | Arquiteto/sênior | Textos coerentes com o spike |
| 8 | Completar os vetores de teste da spec (envelope assinado, Zstd/LZ4, chunker) | Júnior (com o sênior) | Implementação independente reproduz os valores |

Itens **não** bloqueantes, que podem ir para a Fase 1: export do Godot com `.pck`, Linux/macOS no spike, abordagens B, C e E do spike, a decisão sobre o tamanho do manifesto.

## Plano para 3 recursos

O plano anterior supunha 7 pessoas ([10](10-equipe-e-niveis.md)). Com 3, a capacidade cai a menos da metade, e o gargalo não é a soma das horas, é o **sênior**: cache em disco, agendador, GDExtension, libtorrent, assinatura e fuzzing dependem dele.

### Os 3 recursos (confirmados pelo dono do produto em 2026-10-17)
| Papel | Nível | Foco |
| --- | --- | --- |
| **R1 — Líder técnico** | Arquiteto/sênior | Contratos, ADRs, cache em disco, integração com o Godot, agendador, revisão de tudo que é crítico |
| **R2 — Engenharia e qualidade** | Pleno | Build, CI, bancada, `sw-pack`, fonte HTTP, testes de queda, fuzzing |
| **R3 — Conteúdo e apoio** | Júnior ou pleno | Mapa e assets, scripts da bancada, documentação, vetores de teste, telemetria, painéis |


### Regra de revisão com 3 pessoas
A regra "revisão de um sênior antes de aceitar" deixa de funcionar com um só sênior. Proposta: ADR é aceito quando **R1 escreve ou revisa**, **outro recurso lê e comenta**, e **o dono do produto aprova** (como já foi feito com os ADRs 0001–0009). Código crítico (verificação de hash, parsing de rede, journal, API pública) exige **dois olhos** sempre, e se o autor for R1, o segundo revisor é R2.

### Novo cronograma (estimativa inicial, a revisar ao fim de cada fase)
| Fase | Plano para 7 | Plano para 3 | Observação |
| --- | --- | --- | --- |
| F0 fechamento | 3 sem | **+1 sem** | Lista acima |
| F1 Engine + cache + rede simulada | 8 sem | **14–16 sem** | Dividir em F1a e F1b (abaixo) |
| F2 Híbrido P2P | 8 sem | **12 sem** | libtorrent e o storage customizado ficam com R1 |
| F3 Swarm em escala | 6 sem | **8–10 sem** | Bancada com poucos peers reais; o resto simulado |
| F4 MVP | 22–24 sem | **não estimar agora** | Reestimar no Portão 2, com a equipe que existir então |

### F1 dividida em duas
- **F1a (≈ 8–9 semanas): provar o Portão 1.** Cache em disco com journal (F1-10 a F1-14), VFS, fonte HTTP e agendador v1, `sw-pack` mínimo, loader do Godot com política de falta (ADR-0016), medição em 3 perfis de rede. Sem prefetch espacial.
- **F1b (≈ 5–6 semanas): refinamento.** Traces e índice espacial (F1-25/26), política de falta completa, experimento de tamanho de bloco (F1-35), decisão do tamanho do manifesto.

**Portão 1** passa a ficar **no fim da F1a**, com os critérios absolutos de stutter. Se falhar aqui, a F1b e todo o resto mudam, e o custo de ter descoberto isso foi de ≈ 9 semanas, não de 16.

### O que adiar ou cortar
| Item | Decisão |
| --- | --- |
| Backend hospedado multi-cliente, cobrança e painel ([ADR-0009](adr/0009-abertura-de-backend-e-launcher.md), formato B) | **Adiar para depois do Portão 2.** O formato B continua sendo a decisão de negócio, mas com 3 recursos não há sênior de backend na F2 (a consequência prevista no ADR-0009 não se sustenta). Nas F1 a F3, os serviços são os de teste da bancada |
| Launcher (ADR-0011) | Fora do caminho até a F4 |
| Fuzzing contínuo | Começar só quando existir parser de rede real (F2); na F1, testes de propriedade e de queda |
| Linux em CI | Manter (já está escrito), mas macOS e Unreal/Unity ficam para a F6 |
| Unreal e Unity | F6, como já previsto |

### Distribuição proposta para a F1a
| Pessoa | Tarefas (IDs do [backlog](11-backlog-inicial.md)) |
| --- | --- |
| **R1** | F1-10 a F1-13 (slots, journal, thread dona, leases), F1-28 (agendador v1), loader do Godot e política de falta (ADR-0016, substitui F1-20/21), revisões críticas |
| **R2** | F1-01, F1-03 a F1-05 (chunker, compressão, manifesto, classificação), F1-14 a F1-17 (remoção, RAM, checksum, VFS), F1-27 (fonte HTTP), F1-31/32 (testes de queda e disco cheio), rodadas da bancada |
| **R3** | F1-02 e F1-08 (testes e relatório do chunker), F1-18 (inspetor do cache), F1-24 (overlay), F1-30 e F1-33/34 (telemetria e relatórios), vetores de teste, documentação |

### Riscos novos (também em [12-riscos.md](12-riscos.md))
- **R14: dependência de uma pessoa (fator ônibus = 1).** R1 acumula cache, agendador e Godot. Mitigação: contratos escritos, mocks que permitem trabalhar sem ele, e documentar cada decisão em ADR.
- **R15: medição que não mede o que se pretende** (achado 2). Mitigação: validar a bancada com uma mudança conhecida (por exemplo, rede mais lenta deve piorar a carga) antes de confiar nos números.

## Decisões
| # | Decisão | Estado |
| --- | --- | --- |
| 1 | Quem são os 3 recursos (níveis) e uso da distribuição acima | **Confirmado** em 2026-10-17: R1 sênior, R2 pleno, R3 júnior |
| 2 | Adiar o backend hospedado para depois do Portão 2 (ajusta uma consequência do [ADR-0009](adr/0009-abertura-de-backend-e-launcher.md)) | **Autorizado** em 2026-10-17 |
| 3 | Regra de revisão com 3 pessoas | **Autorizada** em 2026-10-17 |
| 4 | Criar o repositório no GitHub; informar o e-mail de segurança e o titular do copyright | **Feito em 2026-10-17**: repositório privado e vazio em <https://github.com/Alexandremocmg/swarmstream>; e-mail de segurança `autokabem@gmail.com`; titular do copyright "Alexandre Aparecido Rodrigues Peres" (já aplicado em `NOTICE`, `SECURITY.md` e nos cabeçalhos dos arquivos) |

## Pedido de fechamento da Fase 0, por recurso
Itens numerados conforme a lista "Lista para fechar a Fase 0". Cada pessoa entrega a evidência no PR (saída de comando, captura ou número medido).

### R1 — Líder técnico sênior
> Os ADRs 0010–0016 estão escritos, mas em "Proposta". Revise cada um, aceite, ajuste ou devolva com motivo, usando a regra de revisão para 3 pessoas (você revisa, outro recurso lê e comenta, o dono do produto aprova). Congele o `swarmstream.h` v0 depois da leitura do R2. Corrija o ADR-0001, o doc 06 e a tarefa F1-20 conforme o ADR-0016 (o spike mostrou que uma GDExtension não implementa `FileAccess`). Pronto quando nenhum ADR da Fase 0 estiver em "Proposta". (itens 6 e 7). Skills: `architecture-designer`, `gdextension`.

### R2 — Engenharia e qualidade (pleno)
> Você coloca o projeto de pé no GitHub. O repositório já existe, privado e vazio: <https://github.com/Alexandremocmg/swarmstream> (você precisa receber acesso de escrita do dono do produto antes do primeiro `git push`). Faça o `git init` com `.gitignore` que exclua `REA-Estudo/`, `build*/` e `.venv`, mais `.gitattributes` para fim de linha; primeiro commit com `LICENSE`, `NOTICE` e `SECURITY.md`. Depois deixe o CI rodando em Windows e Linux e prove com dois PRs: um que quebra e fica vermelho, outro verde. Por último, corrija o método do baseline: texturas em formato GPU comprimido, limites absolutos de stutter e uma carga a frio com reinício do Windows. Se a carga fria continuar igual à quente, documente que a cena não mede I/O. (itens 1, 3 e 5). Skills: `cmake`, `sanitizers`, `static-analysis`, `systematic-debugging`.

### R3 — Conteúdo e apoio (júnior)
> Escreva o guia de ambiente no CONTRIBUTING (compilador, CMake, vcpkg, Godot 4.7.2, Python) e depois o teste do critério de 1 hora: peça a alguém que não montou o ambiente para seguir só o guia, com cronômetro, e registre o resultado em `docs/medicoes/`. Complete os vetores de teste da spec: envelope assinado, Zstd/LZ4 e chunker, gerados por um script e conferidos por uma implementação independente. Registre qualquer passo do guia que falhou. (itens 4 e 8). Skills: `python-pro`, `property-based-testing`.

### Dono do produto
- ~~Criar o repositório no GitHub e passar o endereço ao R2.~~ Feito: <https://github.com/Alexandremocmg/swarmstream> (privado, vazio).
- ~~Definir o e-mail de segurança e o titular do copyright.~~ Feito: `autokabem@gmail.com` e "Alexandre Aparecido Rodrigues Peres".
- ~~Decidir quando o repositório passa de privado para público.~~ Decidido em 2026-10-17: **permanece privado até o fechamento da Fase 0**. A publicação depois disso é uma nova decisão (recomendado antes: consulta jurídica rápida e revisão do primeiro commit).
- **Adiado a pedido do dono do produto:** acesso de escrita do R2 (e depois R1 e R3) ao repositório. Quando informar o usuário do GitHub de cada um, o acesso é adicionado. Enquanto isso o trabalho segue sem colaboradores.
- **Feito em 2026-10-17 (item 1 da lista de fechamento, parte local):** `git init`, `.gitignore` ampliado (exclui `REA-Estudo/`, builds, dependências do spike e assets), `.gitattributes` (LF) e primeiro commit local assinado (`-s`) com 252 arquivos e ~1,8 MB. **Enviado ao GitHub em 2026-10-17 (branch `main`, repositório privado).**
- **Primeira execução do CI (2026-10-17):** `lint` e `ci` terminaram em sucesso: build e teste em Ubuntu e Windows, e ASan+UBSan em Ubuntu. Isso prova que o CI funciona no verde. **Falta provar o vermelho** (PR de teste que quebra) e o MSVC/Linux fora do MinGW local já está coberto pelos runners do GitHub.
- ~~Aprovar os ADRs depois da revisão do R1 (item 6).~~ Feito em 2026-10-17: ADRs 0010–0016 aceitos e `swarmstream.h` v0 congelado ([registro](revisoes/F0-revisao-adrs-0010-0016.md)).
- Escolher quem fará o teste de 1 hora do item 4 (alguém que não participou da montagem do ambiente).

### Ordem e prazo (≈ 1 semana)
1. Dias 1–2: R2 faz o repositório e o CI; R1 revisa os ADRs; R3 escreve o guia.
2. Dias 3–4: R2 corrige o baseline; R1 congela o header; R3 completa os vetores.
3. Dia 5: teste de 1 hora com a pessoa externa; reunião de fechamento (1 h) e decisão de passar para a F1a.

## Situação do fechamento (atualizada em 2026-10-17, à noite)

Verificada no GitHub (PRs, execuções do CI) e no disco, não só nos relatos.

| # | Item | Estado | Evidência / o que falta |
| --- | --- | --- | --- |
| 1 | Git e primeiro commit | **Feito** | Commits na `main`; `REA-Estudo/` e builds fora |
| 2 | Repositório, `CODEOWNERS`, e-mail | **Parcial** | Repositório e e-mail feitos. `CODEOWNERS` ainda com apelidos de função: depende dos usuários do GitHub de R1, R2, R3 |
| 3 | CI com PR vermelho e PR verde | **Feito (provado)** | [PR #2](https://github.com/Alexandremocmg/swarmstream/pull/2) verde em Ubuntu, Windows (MSVC), ASan+UBSan e lint; [PR #1](https://github.com/Alexandremocmg/swarmstream/pull/1) vermelho em Ubuntu, Windows e ASan por uma asserção quebrada. A primeira tentativa revelou 3 defeitos reais do CI (travamento no Windows, `.clang-format` inválido para o clang-format 18, C++ existente fora do formato) |
| 4 | Guia de ambiente e teste de 1 hora | **Guia escrito, teste não executado** | Falta máquina limpa e uma pessoa que não montou o ambiente ([protocolo](medicoes/guia-1h.md)) |
| 5 | Método do baseline | **Quase feito** | [PR #3](https://github.com/Alexandremocmg/swarmstream/pull/3) verde: texturas GPU (carga de ~30 s para ~2,5 s), limites absolutos, carga fria por esvaziamento de cache (leitura pura ~2× a frio). Reinício do Windows não executado (ver decisão abaixo) |
| 6 | ADRs 0010–0016 e `swarmstream.h` v0 | **Aceitos em `main`** (commit 8799c3c) | O registro de revisão diz que o dono do produto aprovou. **Confirmar** (abaixo) |
| 7 | Correção de ADR-0001, doc 06, F1-20 | **Feito** | No mesmo commit |
| 8 | Vetores de teste da spec | **Em andamento** | `sw-pack/testdata/` existe, ainda sem commit. Pendências: R1 aprovar a definição do chunker; o payload do envelope é sintético até existir `flatc` ou `sw-pack` |

### Problemas encontrados na verificação
- **`main` está vermelha no lint** (commit 8799c3c). Causa: o código C++ existente não segue o `.clang-format`, e a configuração corrigida só está no PR #2, ainda não aceito.
- **PRs #1, #2 e #3 abertos.** O #1 é só prova e não deve ser aceito. O #2 deve entrar antes do #3.
- **Formatação do C++ existente:** um PR mecânico de formatação fica para depois do congelamento do header (28/10), senão qualquer PR que toque esses arquivos fica vermelho no lint.
- **Limites do baseline** (+3 quadros, +1, +2 ms, 500 ms) são escolhas do R2, não derivadas de dados. Precisam de aprovação do R1.
- **Compilador local:** o Windows Defender bloqueou o MinGW na máquina do R2. Não foi alterado; a correção foi validada só pelo CI.

### Conclusão
A Fase 0 está **perto de fechar**. Faltam 4 coisas: o teste de 1 hora (item 4), aceitar os PRs #2 e #3 (itens 3 e 5), os vetores da spec (item 8) e os usuários do GitHub para o `CODEOWNERS` (item 2). Nenhuma depende de novo desenvolvimento grande.
