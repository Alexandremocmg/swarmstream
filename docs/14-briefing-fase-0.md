# Briefing da Fase 0 — o que pedir a cada pessoa

Como passar a Fase 0 ([09-roadmap-fases.md](09-roadmap-fases.md#f0--fundação--3-semanas)) para a equipe base (1 arquiteto, 2 sêniores, 2 plenos, 2 juniores). Cada briefing já traz: objetivo, tarefas do [backlog](11-backlog-inicial.md), o que entregar, como saber que acabou, de quem depende e quais skills do Claude usar ([13](13-skills-claude.md)).

## Regras para todos
1. **Leitura obrigatória antes de começar (1–2 h):** [01](01-visao-e-escopo.md), [02](02-arquitetura.md), [glossário](glossario.md), [CONTRIBUTING](../CONTRIBUTING.md) e os docs da sua trilha.
2. **Uma tarefa por vez, PR pequeno** (até ~5 dias), com o ID no título: `F0-04: ...`.
3. **Sem critério de aceite cumprido, não está pronto.** Cole no PR a evidência (saída do comando, captura, número medido).
4. **Travou por mais de meio dia? Pergunte.** Perguntar cedo é esperado, principalmente para júnior.
5. **Dúvida de arquitetura vira pergunta no ADR**, não decisão silenciosa.
6. **Ao fim da tarefa:** atualize o doc afetado e diga no PR o que ficou de fora.

## Ordem e dependências

```
Semana 1:  F0-01 repo ─► F0-02 build ─► F0-03 CI          F0-05 ADRs (arquiteto)
           F0-09 mapa Godot (júnior 1)   F0-11 servidor de rede (júnior 2)
Semana 2:  F0-06 header ─► F0-07 interfaces+mocks         F0-08 manifesto (sênior)
           F0-10 rota de câmera ─► F0-12 baseline          F0-04 lint (júnior)
Semana 3:  F0-13 guia de ambiente ─► teste "clona e roda em < 1 h"
```

Quem não tem tarefa liberada na semana 1 (sênior de engine, pleno 2): faz leitura dirigida e a preparação indicada no briefing.

---

## Arquiteto / tech lead

**Objetivo:** deixar as decisões e os contratos prontos para a equipe trabalhar em paralelo.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-05** Escrever os ADRs pendentes (os 0001–0008 já foram aceitos pelo dono do produto) | Novos ADRs: linguagem do backend, tecnologia do launcher, serialização do manifesto, hash dos blocos (BLAKE3 vs SHA-256). Preparar `LICENSE` (Apache-2.0, [ADR-0008](adr/0008-licenca-open-source.md)), `NOTICE` e `SECURITY.md` | Nenhum ADR "Proposta" restante; cada um revisado por 1 sênior |
| **F0-06** `swarmstream.h` v0 | Header C compilando; todas as funções devolvem "não implementado" | 2 sêniores aprovaram; congelado para a F1 |
| **Licença open source** | `LICENSE`, `NOTICE` com as licenças de terceiros, `SECURITY.md`, política de contribuição (DCO ou CLA) no repositório | Presentes no primeiro commit do F0-01 |
| Cerimônia da fase | Planejamento de 1 h; definir CODEOWNERS | Todos com tarefa atribuída no dia 2 |

**Como pedir (exemplo):**
> Você é dono dos contratos da Fase 0. Os ADRs 0001–0008 já estão aceitos e valem como base: não os reabra sem motivo medido. Até o fim da semana 1, escreva os ADRs pendentes (backend, launcher, manifesto, hash) e prepare os arquivos de projeto aberto (`LICENSE`, `NOTICE`, `SECURITY.md`, política de contribuição) para o Pleno 1 incluir no primeiro commit. Até o meio da semana 2, congele o `swarmstream.h` v0 depois de revisão dos dois sêniores. Qualquer mudança de contrato depois disso exige ADR novo. Skill útil: `architecture-designer`.

---

## Sênior de redes/sistemas

**Objetivo:** interfaces do cache e da rede prontas, com versões falsas (mocks) para todos usarem.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-07** `IContentSource` e `ICacheStore` + mocks em memória | Headers + 2 implementações falsas + testes | Um teste usa os dois mocks juntos (pedido → fonte falsa → cache falso → leitura) |
| Revisar **F0-06** | Comentários no header | Aprovação ou lista de mudanças em 2 dias |
| Apoio ao **F0-02/03** | Revisão do CMake/CI com foco em sanitizers | CI roda ASan/UBSan e TSan |
| Preparação para a F1 | Esboço do formato dos arquivos do cache (slots, índice, journal) em [04](04-cache-local.md) | Esboço revisado pelo arquiteto |

**Como pedir:**
> Defina e implemente as interfaces `ICacheStore` e `IContentSource` (veja [02](02-arquitetura.md#contratos-entre-módulos)). Entregue também mocks em memória, porque os outros times só conseguem avançar se tiverem algo contra o que testar. Interface pequena, sem detalhes de libtorrent nem de disco. Skills: `cpp-pro`, `modern-cpp`, `memory-model`, `db-acid-in-practice` (para o esboço do journal).

---

## Sênior de engine/streaming

**Objetivo:** especificação do formato de conteúdo e viabilidade da integração com o Godot.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-08** Especificação v0 do manifesto e do layout bloco/peça | [03](03-formato-de-conteudo.md) atualizado: campos, tamanhos, regras de assinatura, exemplos | O pleno do `sw-pack` consegue implementar só com o documento · `critico` |
| Spike de 2 dias: `FileAccess` customizado no Godot | Nota curta (1 página) com o ponto de intercepção escolhido e riscos | Um "hello world" lê um arquivo por uma classe própria |
| Revisar **F0-09/F0-10** | Checklist: o mapa tem regiões, assets pesados e rota repetível | Aprovado |

**Como pedir:**
> Escreva a especificação v0 do manifesto e do layout de bloco e peça, de modo que outra pessoa implemente sem falar com você. Depois faça um spike de 2 dias provando que dá para interceptar a leitura de arquivos no Godot 4 via GDExtension; o resultado é uma nota com a abordagem e os riscos, não código final. Skills: `gdextension`, `godot-project-setup`, `cpp-pro`.

---

## Pleno 1 (build / conteúdo)

**Objetivo:** infraestrutura de build funcionando para todo mundo.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-01** Monorepo | Estrutura de pastas com README em cada uma e os arquivos de projeto aberto (`LICENSE`, `NOTICE`, `SECURITY.md`) preparados pelo arquiteto | Estrutura revisada pelo arquiteto |
| **F0-02** CMake + vcpkg | Build vazio de `sw-core` e `sw-pack` no Windows e Linux | `cmake --build` funciona nos dois |
| **F0-03** CI | Build + testes a cada PR, ASan/UBSan no Linux | PR com teste quebrado fica vermelho |
| **F0-13** (com o júnior) | Guia de ambiente | Pessoa nova compila em < 1 h |

**Como pedir:**
> Você abre a Fase 0: sem o repositório e o CI, ninguém começa. Prioridade máxima nos 3 primeiros dias: F0-01, F0-02 e F0-03, nessa ordem. Um build que quebra no Windows ou no Linux é bloqueio para o time inteiro, então avise no canal assim que sair o primeiro build verde. Skills: `cmake`, `conan-vcpkg`, `sanitizers`, `static-analysis`.

---

## Pleno 2 (bancada / qualidade)

**Objetivo:** o jeito de medir está pronto antes de existir algo para medir.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-12** Baseline | Relatório `docs/medicoes/baseline.md`: FPS, quadros > 33 ms, tempo de carga lendo do SSD | Mesma rota gera números parecidos em 3 execuções |
| Revisar **F0-11** (servidor de rede) | Validação dos perfis `fibra_300`, `cabo_50`, `adsl_10` | Banda e latência medidas batem com o configurado (± 10%) |
| Definir o formato do relatório de rodada | Modelo em [08](08-metricas-e-testes.md) | Júnior 2 consegue gerar o relatório seguindo o modelo |
| Preparação para a F1 | Esboço dos testes de queda e de disco cheio | Lista de cenários revisada pelo sênior de redes |

**Como pedir:**
> Seu trabalho é garantir que, quando a Fase 1 começar, qualquer mudança possa ser medida do mesmo jeito. Faça o baseline com a rota gravada, três vezes, e anote a variação. Se o número varia mais de 10% entre execuções, a bancada não está confiável e isso é o primeiro problema a resolver. Skills: `python-pro`, `godot-testing`, `systematic-debugging`.

---

## Júnior 1 (conteúdo do Godot)

**Objetivo:** o mapa de teste que vai sustentar todas as medições.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-09** Projeto Godot com 2–5 GB de assets | Projeto com 4+ regiões, assets pesados (texturas 4K, malhas), **lista de licenças (CC0)** | Abre e roda no Godot 4; nenhum asset sem licença registrada |
| **F0-10** Rota de câmera reprodutível | Script de rota + modo "robô" | Duas execuções passam pelos mesmos pontos (verificado pelo log de posição) |
| Apoio ao **F0-13** | Anotar tudo que atrapalhou ao montar o ambiente | Notas entregues ao pleno 1 |

**Como pedir:**
> Monte o mapa de teste: precisa ser pesado de verdade (muitas texturas grandes) e dividido em regiões separadas, porque vamos medir o que acontece ao atravessar de uma para outra. Use só assets de licença livre (Poly Haven, por exemplo) e anote cada um numa planilha com a licença. Depois grave uma rota de câmera que dê para repetir exatamente. Se algo não ficar claro, pergunte ao sênior de engine antes de gastar dias. Skills: `godot-project-setup`, `assets-pipeline`.

**Armadilhas comuns:** o repositório não deve guardar 5 GB de assets no git comum (usar Git LFS ou baixar por script); não esquecer de fixar a versão do Godot.

---

## Júnior 2 (rede simulada e lint)

**Objetivo:** servidor que imita internet ruim, e padrões de código automáticos.

| Tarefa | O que entregar | Pronto quando |
| --- | --- | --- |
| **F0-11** Servidor HTTP Range com latência/banda/perda | Script Python com 3 perfis prontos e opção de perfil próprio | Pleno 2 valida as medidas (± 10%) |
| **F0-04** clang-format, clang-tidy, modelo de PR, CODEOWNERS | Arquivos de configuração no repositório | CI recusa PR mal formatado |
| Texto para **F0-13** | Rascunho do guia de ambiente | Revisado pelo pleno 1 |

**Como pedir:**
> Construa um servidor que serve arquivos com HTTP Range e finge uma conexão ruim: você escolhe a banda, a latência e a perda, e ele se comporta assim. Comece simples, com `fibra_300`, `cabo_50` e `adsl_10`, e meça com um download de teste que o resultado bate com o configurado. Depois configure o formatador e o lint (as regras são as do pleno 1). Skill: `python-pro`.

---

## Dono do produto (você)

| O que fazer na Fase 0 | Quando |
| --- | --- |
| Nome (Swarmstream), ADRs 0001–0009 (inclui licença Apache-2.0 e o modelo de negócio B) e o perfil do piloto (estúdio indie em Godot) já estão decididos | — |
| Começar a procurar estúdios indie candidatos ao piloto (jogos grandes em Godot) | Fases 0 a 2 |
| Definir o plano de preços do backend hospedado e aprovar a contratação do sênior de backend/operação (já na Fase 2) | Fases 1 e 2 |
| Aprovar o orçamento e a composição da equipe | Semana 1 |
| Pedir à equipe uma demonstração curta no fim da semana 3 | Semana 3 |

## Reunião de fim da Fase 0 (1 h)
1. Demonstração: clonar, compilar e rodar o mapa de teste com o relatório de baseline.
2. Revisão dos contratos congelados e dos ADRs.
3. Conferir o critério de saída: **pessoa nova compila e roda em menos de 1 hora**.
4. Planejamento da Fase 1: distribuição de tarefas conforme [11](11-backlog-inicial.md#distribuição-sugerida-na-f1-equipe-base).

## Se alguém atrasar
| Situação | O que fazer |
| --- | --- |
| Build/CI (F0-01 a F0-03) atrasa | Todos os outros continuam nas tarefas independentes (F0-05, F0-08, F0-09, F0-11); o arquiteto ajuda o pleno 1. |
| Mapa de teste (F0-09) atrasa | Usar um mapa mínimo de 500 MB só para destravar o baseline; completar depois. |
| Contratos (F0-06/07) atrasam | A F1 não começa: é a única dependência dura da fase. |
