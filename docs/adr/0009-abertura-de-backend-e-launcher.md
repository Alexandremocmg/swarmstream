# ADR-0009: O que fica aberto ou fechado no backend e no launcher

**Status:** Aceita (2026-10-17, pelo dono do produto: formato B) · **Data:** 2026-10-17

## Contexto
O núcleo (`sw-core`, `sw-pack`, SDK Godot) é open source sob Apache-2.0 ([ADR-0008](0008-licenca-open-source.md)). O produto tem outras duas partes:
- **Serviços de backend** (`sw-services`): tracker, serviço de manifestos, coleta de telemetria, STUN.
- **Launcher** (`sw-launcher`): programa do jogador, com instalação, modos compacto e completo e configuração de upload.

Esta decisão é, na prática, o **modelo de negócio**: define o que o projeto vende e o que a comunidade pode usar de graça. Primeiro cliente-piloto decidido: um estúdio indie com jogo grande em Godot, o que pesa na escolha.

Nenhuma fase antes da F2 depende desta decisão: a Fase 0 e a Fase 1 usam servidores de teste na bancada `sw-netlab`.

## Os três formatos

### A — Tudo aberto
Núcleo, backend e launcher sob licença aberta. A receita vem de suporte, consultoria, integração e, se houver, um serviço hospedado opcional.

| Prós | Contras |
| --- | --- |
| Máxima confiança: o estúdio e o jogador podem auditar tudo o que usa a rede e o disco | Receita incerta; quem não precisa de suporte não paga |
| Adoção rápida por indies, que não querem depender de fornecedor | Um concorrente grande pode hospedar o mesmo código e vender como serviço |
| Contribuições externas em todas as partes | O backend precisa ser fácil de rodar por terceiros (documentação, Docker, configuração): trabalho extra |
| Simples de explicar | Suporte a instalações que não controlamos |

### B — Núcleo aberto + backend hospedado por nós, pago (recomendado)
Núcleo aberto. O backend (tracker, manifestos, telemetria) é um serviço que operamos e cobramos, por uso ou por plano. O launcher é aberto ou gratuito. Os protocolos de comunicação são documentados, de modo que o estúdio possa trocar de fornecedor.

| Prós | Contras |
| --- | --- |
| Modelo de receita claro: quem usa a rede paga | Precisa de operação profissional: disponibilidade, custo de infraestrutura, plantão |
| Estúdio indie começa sem montar infraestrutura, o que combina com o piloto | Backend multi-cliente (isolamento entre estúdios, autenticação, cobrança): mais engenharia |
| Núcleo aberto mantém a confiança no que roda no PC do jogador | Alguns estúdios vão querer hospedar o próprio backend e pedir uma versão aberta |
| Nós controlamos a qualidade da rede (tracker, localidade) | Se o serviço cair, todos os clientes sentem, mas a CDN continua servindo (ADR-0003) |

### C — Núcleo aberto + launcher fechado
Núcleo e backend abertos; o launcher é o produto comercial, vendido ou licenciado a publishers.

| Prós | Contras |
| --- | --- |
| O launcher é o ponto de contato com o jogador, e é fácil de explicar a um publisher | O launcher controla o upload do jogador: um programa fechado que usa a rede de quem joga gera desconfiança, justamente o risco R4 em [12-riscos.md](../12-riscos.md) |
| Menos infraestrutura para operar | Concorre com Steam, Epic e outros launchers que os jogadores já têm; baixa adoção provável |
| Receita por licença | Estúdio indie raramente paga por um launcher próprio |

## Comparação lado a lado

| Critério | A — Tudo aberto | B — Backend hospedado | C — Launcher fechado |
| --- | --- | --- | --- |
| Receita | Incerta (suporte, serviços) | Recorrente, por uso | Por licença |
| Confiança do jogador | Máxima | Alta (cliente aberto) | Menor |
| Encaixe com o piloto indie em Godot | Bom | **Muito bom** | Fraco |
| Encaixe com publishers (Fase 6) | Médio | Bom (SLA, cobrança) | Bom |
| Risco de copiarem o serviço | Alto | Médio | Médio |
| Carga operacional | Baixa (suporte) | **Alta** | Média |
| Engenharia extra frente ao plano atual | Média (facilitar self-host) | **Alta** (multi-cliente, cobrança) | Baixa |
| Reversível depois? | Difícil fechar o que abriu | Fácil abrir o backend depois | Fácil abrir o launcher depois |

## Impacto no cronograma
As estimativas abaixo são uma primeira avaliação sobre o [roadmap](../09-roadmap-fases.md) e precisam ser revisadas pelos sêniores de redes e de engine antes de entrar no plano.

| Formato | Fases 0 a 3 | Fase 4 (MVP) | Equipe |
| --- | --- | --- | --- |
| **A** | Sem mudança | +3 a 4 semanas: empacotar o backend para terceiros (Docker, documentação, configuração) e preparar o projeto para contribuições | Sem mudança |
| **B** | Sem mudança | +6 a 8 semanas: contas de estúdio, isolamento entre clientes, medição de uso e cobrança, painel, monitoramento | Um sênior de backend/operação já na F2 (hoje só entra na F4) |
| **C** | Sem mudança | +0 a 2 semanas: o launcher já está no plano; só aumenta o acabamento (instalador, assinatura, atualização automática) | Sem mudança |

Em todos os formatos, as Fases 0 a 3 não mudam: a decisão só precisa estar tomada até o fim da Fase 2, antes dos primeiros serviços próprios.

## Recomendação
**Formato B**, com duas salvaguardas:
1. **Protocolos abertos e documentados** (manifesto, tracker, telemetria), para que o estúdio possa hospedar o próprio backend. Isso mantém a confiança do modelo A sem abrir mão da receita.
2. **Launcher aberto ou gratuito** (não o formato C), porque ele controla o upload do jogador, e a confiança é parte do produto.

Por quê: B é o único formato com receita recorrente clara que também combina com o piloto indie em Godot e com a promessa de um núcleo aberto. O custo é operar um serviço, e por isso a decisão deve vir junto da contratação do sênior de backend (ver [10-equipe-e-niveis.md](../10-equipe-e-niveis.md)).

Se o objetivo principal for ser um projeto comunitário sem receita direta, A é mais coerente. C só faz sentido se o público-alvo mudar para publishers grandes com launcher próprio.

## Decisão
**Formato B**, escolhido pelo dono do produto em 2026-10-17:
- **Núcleo aberto** (Apache-2.0, [ADR-0008](0008-licenca-open-source.md)).
- **Backend hospedado por nós e pago** (tracker, serviço de manifestos, telemetria).
- **Launcher aberto ou gratuito.**
- **Protocolos abertos e documentados** (manifesto, tracker, telemetria), para que o estúdio possa hospedar o próprio backend ou trocar de fornecedor. Isso é uma obrigação do projeto, não uma opção: a documentação dos protocolos entra como entregável da Fase 4.

Ainda em aberto, dentro deste formato: se o launcher será código aberto ou apenas gratuito (a decisão pode esperar a Fase 4) e o plano de preços do backend.

## Ajuste de 2026-10-17 (equipe de 3 recursos)
O formato B continua sendo a decisão de negócio. Com a equipe reduzida a 3 recursos, o dono do produto autorizou **adiar o backend hospedado multi-cliente (contas, cobrança, painel) para depois do Portão 2**. Nas Fases 1 a 3 valem os serviços de teste da bancada. Por isso, a consequência "sênior de backend/operação já na Fase 2" e a duração da Fase 4 abaixo ficam **suspensas** até o Portão 2, quando serão reestimadas com a equipe que existir ([15](../15-portao-fase-0-e-plano-3-recursos.md)).

## Consequências
- Escrever os ADRs de linguagem do backend e de tecnologia do launcher (agora desbloqueados).
- Fase 4 passa de ≈ 16 para ≈ 22–24 semanas (contas de estúdio, isolamento entre clientes, medição de uso e cobrança, painel, monitoramento). Estimativa inicial a revisar pelos sêniores.
- Contratar um **sênior de backend/operação já na Fase 2** (antes planejado só para a F4).
- Novo risco R13 em [12-riscos.md](../12-riscos.md): operar um serviço pago exige disponibilidade, plantão e controle de custo.
- Definir o plano de preços e os limites de um plano gratuito para o piloto (decisão de produto, fora deste ADR).
- Os protocolos abertos exigem versionamento e documentação desde o primeiro serviço.
