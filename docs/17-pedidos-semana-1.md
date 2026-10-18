# Pedidos da semana 1 (Fase 1a) para sessões do Claude Code

Pedidos prontos para copiar e colar, um por sessão. Contexto geral: [16-briefing-fase-1a.md](16-briefing-fase-1a.md). As regras que toda sessão segue estão no [CLAUDE.md](../CLAUDE.md) da raiz.

## Resumo

| Sessão | Tarefa | Modelo | Esforço | Pode rodar junto das outras? |
| --- | --- | --- | --- | --- |
| A | **F1-20, portão do Godot** (ADR-0016) | Opus 5.5 | muito alto | Sim, mas usa a GPU e abre janelas: não rode benchmarks em paralelo |
| B | **F1-36**, checagem vazia do baseline | Sonnet 5.5 | médio | Sim |
| C | **F1-01**, chunker em C++ | Sonnet 5.5 | alto (suba para Opus 5.5 se travar) | Sim |
| D (quando A, B ou C terminar) | **F1-33**, esqueleto do relatório da rodada | Sonnet 5.5 | médio | Sim |
| Revisão | Revisor independente do F1-01 e do portão do F1-20 | Opus 5.5, **sessão nova** | alto | Depois dos PRs |

No máximo **3 sessões escrevendo ao mesmo tempo**.

## Antes de abrir as sessões

1. **Crie cada sessão em um worktree próprio** (opção de worktree ao criar a sessão no aplicativo). Cada uma tem o seu branch e não mexe na pasta compartilhada.
2. **Aceite o PR que traz o `CLAUDE.md`** (este). Sem ele na `main`, as sessões novas não recebem as regras.
3. **Para a sessão A (Godot):** o worktree não tem os assets de 2,8 GB nem o `godot-cpp` (ficam fora do git). Crie uma junção para os assets que já estão na sua pasta (no PowerShell, trocando `<worktree>` pelo caminho da sessão):
   ```powershell
   cmd /c mklink /J "<worktree>\sw-godot\project\assets" "C:\Users\alexa\Downloads\p2p_game\sw-godot\project\assets"
   ```
   O Godot 4.7.2 está em `C:\Users\alexa\AppData\Local\Temp\swarmstream-godot-4.7.2\Godot_v4.7.2-stable_win64_console.exe`. **Essa pasta é temporária e o Windows pode apagá-la.** Vale mover o Godot para um lugar fixo (por exemplo `C:\Tools\godot-4.7.2\`) e usar esse caminho nos pedidos.
4. **Pasta compartilhada:** volte-a para a `main` e atualize, quando ninguém a estiver usando.
5. **Pessoa para o teste de 1 hora no Windows** (tarefa humana desta semana): escolha quem não montou o ambiente. Protocolo em [medicoes/guia-1h.md](medicoes/guia-1h.md).

---

## Sessão A: F1-20, portão do Godot (Opus 5.5, esforço muito alto)

```
Tarefa F1-20 da Fase 1a, SOMENTE o "portão obrigatório" do ADR-0016.
Leia antes, nesta ordem: CLAUDE.md, docs/adr/0016-ponto-de-intercepcao-godot.md,
docs/spikes/F0-spike-fileaccess-godot.md, docs/06-integracao-engine-sdk.md
(seção do SDK Godot), sw-core/include/swarmstream.h e a pasta spike-fileaccess/.
Skills: gdextension, multithreading, memory-model, sanitizers.

OBJETIVO
Provar que, em Godot 4.7.2 estoque, um ResourceFormatLoader registrado por uma
GDExtension consegue materializar, a partir de BYTES servidos pelo mock do
sw-core (MemoryContentSource), sem usar FileAccess:
 1) uma textura BCn 4K com mips,
 2) uma malha com 2 LODs,
 3) um áudio OGG,
e TROCAR o mip/LOD no lugar (sem recarregar o recurso), na thread principal,
quando o dado melhor chega (sw_ensure_async). Tudo numa cena do mapa de teste
(sw-godot/project).

COMO
- Parta do código do spike (spike-fileaccess/); não apague o spike. Crie o novo
  código em sw-godot/ (extensão e cena de teste), com build por CMake.
- O _load NUNCA espera a rede: lê com timeout_ms = 0 e entrega o que há, ou aplica
  a política de falta (mip/LOD menor) se receber SW_E_NOT_LOCAL.
- Materialize por APIs de buffer: Image.create_from_data + ImageTexture (ou
  PortableCompressedTexture2D), ArrayMesh.add_surface_from_arrays, load_from_buffer
  dos AudioStream*. NÃO delegue ao loader nativo (ele abre o arquivo por FileAccess).
- godot-cpp: use o tag que o spike usou (veja spike-fileaccess/CMakeLists.txt e a
  nota do spike). Ele não está no git; baixe-o fora do repositório.
- Godot: <CAMINHO DO godot_console.exe 4.7.2>. Assets do mapa: já ligados por junção.

NÃO FAÇA
- Não edite sw-core/include/swarmstream.h (congelado, ADR-0014). Se faltar algo no
  contrato, pare e descreva o que falta e por quê.
- Não comece a F1-21 nem a F1-22. Não aceite o próprio PR.

SE ALGUM DOS TRÊS FOR IMPOSSÍVEL em Godot estoque: PARE, não improvise. Escreva o
resultado com a evidência (erro, versão, trecho de código) em
docs/medicoes/f1-20-portao.md e proponha o ADR que muda a decisão 1 do ADR-0016
para o caminho B (.pck por grupo) para aquele tipo. Isso é resultado, não falha.

ENTREGA (num PR, branch feat/F1-20-portao-loader)
- Código da extensão e cena de teste, com um modo headless que sai com código 0
  quando os três tipos materializam e a troca no lugar funciona, e código 1 caso
  contrário (e diz qual falhou).
- docs/medicoes/f1-20-portao.md: o que foi provado, comandos, saída, versão do
  Godot e do godot-cpp, tempos, e o que NÃO foi provado (audível, export, Linux).
- Teste de que _load não bloqueia (medido) e de que a troca ocorre na thread principal.
- CI verde. Marque o PR como CRÍTICO (peço revisão independente).
```

**Para você, no fim (2 minutos):** abra a cena e confirme com os olhos e o ouvido que a textura aparece, a malha troca de LOD e o áudio toca. Uma sessão só consegue verificar que o recurso foi criado e está "tocando", não que o som sai.

---

## Sessão B: F1-36, checagem vazia do baseline (Sonnet 5.5, esforço médio)

```
Tarefa F1-36 da Fase 1a (docs/11-backlog-inicial.md, linha F1-36).
Leia antes: CLAUDE.md, docs/medicoes/baseline.md e sw-godot/tools/baseline.py
(função check_against_limits e a parte do --check em main()).
Skills: test-driven-development, python-pro, systematic-debugging.

PROBLEMA (já reproduzido)
baseline.py --check imprime "within limits" e sai com código 0 quando os
relatórios não têm as métricas esperadas (relatórios vazios {} ou com chaves
erradas). Hoje o código só confere as métricas que existem no relatório
("name in report").

O QUE FAZER
- Escreva PRIMEIRO os testes que falham: 3 relatórios vazios e 3 com chaves
  erradas devem sair com código 1 e dizer QUAL métrica faltou.
- Corrija para falhar quando faltar qualquer uma destas: frames_over_33ms_steady,
  frames_over_100ms, p99_frame_ms (em cada rodada) e load_ms (para a mediana).
  As métricas obrigatórias devem vir do arquivo de limites, não ficar fixas em dois lugares.
- Mantenha o comportamento correto que já funciona: aceita as 5 rodadas reais
  de docs/medicoes/baseline-gpu.json (uma por arquivo), recusa 3 quadros acima
  de 33 ms, 1 acima de 100 ms, p99 de 7 ms, mediana de carga de 3,5 s, e exige
  pelo menos 3 rodadas para a carga.
- Os testes devem rodar sem o Godot e entrar no CI (lint.yml já roda black,
  ruff e mypy --strict nos arquivos de sw-godot/tools; mantenha-os passando).

NÃO FAÇA
- Não mude os valores dos limites (aprovados pelo R1 e pelo dono do produto).
- Não aceite o próprio PR.

ENTREGA (branch fix/F1-36-checagem-vazia)
PR com os testes, a correção, a saída dos testes antes (falhando) e depois
(passando), e black/ruff/mypy verdes. CI verde.
```

---

## Sessão C: F1-01, chunker em C++ (Sonnet 5.5, esforço alto)

```
Tarefa F1-01 da Fase 1a: implementar em C++20 o chunker "swarmstream-fastcdc-v0"
no sw-pack.
Leia SOMENTE estes arquivos como especificação: CLAUDE.md,
docs/03-formato-de-conteudo.md (seção 11.1), sw-pack/testdata/README.md (seção
"Definição do chunker usada nos vetores") e sw-pack/testdata/vectors-v0.json
(seção "chunker").
Skills: modern-cpp, cpp-pro, cmake, test-driven-development, sanitizers.

REGRA DE INDEPENDÊNCIA (condição aprovada pelo dono do produto)
Escreva a partir da spec. NÃO ABRA nem leia gen_vectors.py, verify_vectors.py,
verify_chunker_r1.py nem test_properties.py. O objetivo é uma implementação
independente das versões em Python. Se a spec for ambígua, NÃO resolva olhando
o Python: pare e descreva a ambiguidade.

O QUE FAZER
- Biblioteca em sw-pack (cabeçalho em sw-pack/include/swarmstream/, fonte em
  sw-pack/src/), ligada ao CMake raiz (hoje só o sw-core é adicionado). API simples:
  dado um buffer/fluxo e os parâmetros (min 32 KiB, avg 128 KiB, max 256 KiB),
  devolve os cortes.
- Teste (CTest) que carrega sw-pack/testdata/vectors-v0.json e REPRODUZ todos os
  casos do chunker e a tabela gear. Se precisar ler JSON, escreva um leitor mínimo
  dentro do teste: NÃO adicione dependências (vcpkg.json) sem pedir ao dono do produto.
- Se a tabela gear exigir SHA-256, implemente-o pequeno e teste com vetores
  conhecidos, ou embuta a tabela de 256 valores conferida contra o JSON. Diga o que escolheu.
- Teste de estabilidade: inserir 1 byte no meio de dados pseudoaleatórios altera
  no máximo 3 blocos. Para 1 GiB, deixe o teste pesado atrás de um rótulo/opção
  do CTest, para não deixar o CI lento (o teste padrão deve levar poucos segundos).
- Sem alocação desnecessária no laço principal; sem comportamento indefinido (o CI
  roda ASan+UBSan).

NÃO FAÇA
- Não mude a definição do algoritmo nem o nome. Não edite swarmstream.h.
- Não aceite o próprio PR.

ENTREGA (branch feat/F1-01-chunker-cpp)
PR com o código, os testes, a lista de vetores reproduzidos (n de n), a saída do
ctest e do teste de estabilidade, e a lista de ambiguidades encontradas na spec. CI verde.
```

---

## Sessão D: F1-33, esqueleto do relatório da rodada (Sonnet 5.5, esforço médio)

Abra quando uma das três anteriores terminar.

```
Tarefa F1-33 da Fase 1a, ESQUELETO (docs/11-backlog-inicial.md, linha F1-33).
Leia antes: CLAUDE.md, docs/08-metricas-e-testes.md, docs/16-briefing-fase-1a.md
(critério do Portão 1), docs/medicoes/baseline.md e docs/medicoes/baseline-gpu.json.
Skills: python-pro, test-driven-development.

OBJETIVO
Um script Python (sw-netlab/ ou sw-godot/tools/, onde fizer mais sentido) que lê
os relatórios de rodada e gera uma tabela em Markdown por perfil de rede
(fibra_300, cabo_50, adsl_10) com: taxa de falta, espera p50/p95/p99 numa falta,
quadros acima de 33 ms e de 100 ms, p99 do quadro, carga, e o veredito contra os
critérios do Portão 1 e os limites de docs/medicoes/baseline-limits.json.

AINDA NÃO EXISTEM os eventos de telemetria reais (F1-30). Por isso:
- Defina o formato de entrada (JSON) e documente-o; use os campos que o baseline.py
  já grava e deixe os campos de falta/espera como opcionais, marcados "n/d" no relatório.
- Métrica ausente = "n/d" visível e veredito "incompleto", NUNCA "passou" (a lição do F1-36).

ENTREGA (branch feat/F1-33-esqueleto-relatorio)
PR com o script, testes (inclua relatório incompleto e perfil faltando), uma saída de
exemplo gerada a partir do baseline-gpu.json e black/ruff/mypy --strict verdes. CI verde.
```

---

## Sessão de revisão (Opus 5.5, **sessão nova**, esforço alto)

Abra depois que o PR existir, uma por PR crítico (F1-20 e F1-01). A sessão **não** pode ser a que escreveu o código.

```
Revise o PR <NÚMERO> como revisor independente. Você não escreveu esse código.
Leia apenas o PR, o CLAUDE.md e os documentos de referência da tarefa
(F1-20: ADR-0016 e o spike; F1-01: docs/03 seção 11.1).
Rode você mesmo build, testes e a verificação que o PR descreve; não confie nos
números do texto do PR.
Procure: falhas silenciosas, retorno de I/O ignorado, corrida de dados, comportamento
indefinido, teste que passa sem testar, e qualquer coisa que a spec ou o ADR contradiga.
Para o F1-01, confira também que o PR NÃO copia a lógica de verify_chunker_r1.py nem de
gen_vectors.py (a independência é a condição aprovada).
Responda: aprovo, aprovo com ajustes (liste) ou recuso (motivo), com a evidência.
Não aceite o PR nem faça push.
```

---

## Fim da semana 1: o que verificar (e pedir a mim)

Peça: **"Verifique os PRs da semana 1."** Eu confiro no GitHub, não só no texto dos PRs:
- CI verde nos quatro jobs e PRs sem arquivos indevidos (assets, builds).
- A saída dos testes e dos vetores, rodados do zero.
- Se o F1-36 recusa relatórios vazios e vazios com chaves erradas.
- Se o chunker reproduz todos os vetores e não copia o Python.
- Se o portão do ADR-0016 passou ou produziu o ADR de mudança para o caminho B.

**Quem aceita:** você, depois de ler o resumo.

## O que é seu nesta semana
| O quê | Quando |
| --- | --- |
| Teste de 1 hora no Windows com uma pessoa que não montou o ambiente | Qualquer dia da semana 1 |
| Ver e ouvir o resultado do portão do Godot | Quando a sessão A entregar |
| Manter o computador parado durante benchmarks | Sempre que uma sessão pedir medição |
| Aceitar PRs na ordem: F1-36, F1-01, F1-20 (o mais arriscado por último) | Depois do CI verde e da revisão |

## A validação da bancada ("mudança conhecida") muda de semana
O briefing da F1a previa validar a bancada na semana 1, trocando `cabo_50` por `adsl_10`. **Isso não é possível agora:** ainda não existe caminho de rede no jogo (a fonte HTTP é a F1-27 e o loader do Godot é a F1-20). Sem ele, mudar o perfil de rede não altera nada no baseline. A validação passa para **o primeiro momento em que o jogo lê pela rede** (previsto para a semana 5, depois de F1-27 e F1-20 integrados), e continua sendo condição para confiar nos números do Portão 1. O [briefing](16-briefing-fase-1a.md) foi ajustado.
