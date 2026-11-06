# Estudo documental limitado: risco R12 (caso Blizzard) e premissas comerciais

**Status: ESTUDO do Planejador (2026-11-06), autorizado pelo dono em 2026-11-06 como estudo documental limitado.** Sem contato externo, sem contratação, sem infraestrutura. Não é prova de demanda e não altera nenhum critério ou decisão.

**Limite honesto:** a pesquisa foi feita em um dia, com busca na web e leitura de páginas públicas. **Não consegui nenhuma fonte primária da Blizzard sobre P2P**: os artigos de suporte pedem login (redirecionam para a tela de entrada do Battle.net). Por isso a parte do caso Blizzard é, hoje, **majoritariamente hipótese e relato secundário**. Cada item abaixo diz de onde veio e quem leu.

Legenda de leitura: **[lido]** página aberta e lida nesta pesquisa (pelo subagente de leitura, em 2026-11-06; o Planejador não reabriu as páginas); **[resultado de busca]** só vi o trecho que o buscador devolveu, a página não foi aberta; **[doc do projeto]** afirmação já escrita nos documentos do projeto, sem fonte anexada.

## 1. Fatos (com a fonte e o nível de confiança)
| # | Afirmação | Fonte | Nível |
| --- | --- | --- | --- |
| F1 | O projeto afirma que o P2P do Blizzard Downloader/Battle.net foi "abandonado ~2016" | [docs/01](../01-visao-e-escopo.md) linha 51 | **[doc do projeto], sem fonte; não verificado** |
| F2 | O cliente de download da Blizzard na era World of Warcraft usava BitTorrent e tinha opção de desligar o P2P, caindo para HTTP | Comentários em lista de operadoras de rede (NANOG, 2010) e fóruns | **[resultado de busca]**, fonte secundária (relato de usuário) |
| F3 | Usuários relataram que o downloader saturava o upload e prejudicava a própria conexão; houve objeção a upload forçado em conexões com franquia | Fóruns e comentários de 2008–2012 | **[resultado de busca]**, anedótico |
| F4 | Em 2011 a operadora canadense Rogers limitou tráfego de World of Warcraft por causa do P2P do cliente e recomendou desligar a opção | Techdirt e Slashdot, 2011 | **[resultado de busca]**, imprensa; a página não foi aberta |
| F5 | Em 2021 a Blizzard removeu LAN/P2P **de partida** no Diablo II Resurrected, citando riscos de segurança | win.gg | **[resultado de busca]**; trata de **jogo em rede, não de distribuição**. Não serve de evidência para o R12 |
| F6 | Preços de entrega de dados nas páginas oficiais dos provedores em 2026-11-06 (tabela abaixo) | Páginas dos provedores | **[lido]** |
| F7 | Wikipedia (Battle.net) não menciona P2P de download; as referências não trazem fonte primária sobre isso | en.wikipedia.org/wiki/Battle.net | **[lido]**, secundária |

**Não encontrado:** qualquer texto oficial da Blizzard com data ou motivo do abandono do P2P de download (se houve); declaração de funcionário (blue post); a taxa da loja Steam e se a Steam cobra banda de download (as páginas de Steamworks lidas não trazem; o contrato de distribuição não foi lido).

### Preços de entrega, lidos em páginas oficiais (2026-11-06)
| Provedor | Preço por GB (entrega para a internet) | Observação |
| --- | --- | --- |
| Bunny CDN, rede Standard | US$ 0,01 (Europa e América do Norte); 0,045 (América do Sul); 0,03 (Ásia e Oceania); 0,06 (Oriente Médio e África) | Sem franquia gratuita informada |
| Bunny CDN, rede Volume | US$ 0,005 (primeiros 500 TB); 0,004 (500 TB–1 PB); 0,002 (1–2 PB) | 10 PoPs |
| Amazon CloudFront | US$ 0,085 (próximos 9 TB, EUA/Canadá/México e Europa) até 0,020 (acima de 5 PB); 1 TB/mês grátis | Descontos por compromisso de ~10 TB/mês |
| Cloudflare R2 | Entrega direta do R2: sem cobrança de egress; armazenamento US$ 0,015/GB-mês | "Se conectar outros serviços medidos, eles podem cobrar" |
| Google Cloud CDN | **não lido** (a página veio vazia) | — |

Os **termos de uso** de cada provedor (por exemplo, se aceitam distribuição de instaladores de jogos) **não foram lidos**. Isso precisa ser conferido antes de qualquer cálculo que dependa deles.

## 2. Hipóteses (o que parece plausível, mas não está comprovado)
| # | Hipótese | Como se verificaria |
| --- | --- | --- |
| H1 | O P2P da Blizzard saiu por uma combinação de: custo de CDN em queda, reclamação sobre upload, problema com operadoras e custo de suporte | Fonte oficial ou relato de ex-funcionário com data; hoje **não há** |
| H2 | O custo da entrega por GB já é baixo o bastante para que a economia de banda seja pequena para clientes indie e AA (o próprio [docs/01](../01-visao-e-escopo.md) linha 17 já levanta isto) | Cálculo da seção 4, com os preços lidos, e dados do cliente |
| H3 | O argumento comercial mais forte é **tempo até jogar** e **retenção** (jogo cabe no SSD), não a economia de banda | Dados de abandono por tempo de instalação (não temos) |
| H4 | O primeiro cliente é um estúdio indie com jogo grande em Godot ([docs/01](../01-visao-e-escopo.md) linha 61) que **distribui fora de uma loja que já oferece entrega** (loja própria, itch.io, launcher próprio) | Entrevistas (fora do escopo autorizado); hoje é suposição |
| H5 | Para jogos vendidos em loja que já entrega os arquivos, o produto resolve pouco do problema de **entrega** e mais o de **cache em disco/streaming** | Idem |
| H6 | Upload de jogadores em conexão com franquia ou ruim é uma objeção real de adoção (o [docs/09](../09-roadmap-fases.md) já prevê detecção de franquia e QoS) | Dados da F2/F3, não simulação |

## 3. Perfil do primeiro cliente e o problema que ele pagaria para resolver
Hoje o projeto **não tem** esta resposta com evidência. O que há:
- **Perfil declarado (doc do projeto):** estúdio indie com jogo grande em Godot; piloto na Fase 6 para publishers.
- **Problemas candidatos**, em ordem de plausibilidade e **todos hipóteses**:
  1. *Jogo grande demais para instalar inteiro* (tempo de instalação e espaço em disco) → o benefício é o streaming por demanda, que o Portão 1 testa.
  2. *Custo de entrega de patches em dia de lançamento* → depende de preço por GB (seção 4) e de onde o jogo é distribuído.
  3. *Download lento em conexões ruins* → benefício do P2P, a medir na F2/F3.
- **O que falta saber para escolher o perfil** (dados necessários): onde o cliente-alvo distribui hoje; tamanho típico do jogo e dos patches; número de instalações e de atualizações por mês; o que faz hoje (ele mesmo hospeda? usa loja?); quanto gasta com entrega; o que perde por abandono durante a instalação.

## 4. Condições econômicas mínimas (ilustrativas, não previsão)
Conta ilustrativa com preços lidos, **sem** dados de cliente:
- Se o jogo e seus patches geram **100 TB/mês** de entrega, a US$ 0,01/GB (Bunny Standard, EUA/Europa) custam US$ 1.000/mês; a US$ 0,005/GB (Volume), US$ 500; no R2, sem cobrança de egress (só armazenamento e operações).
- Se o P2P absorvesse **60–80%** desse tráfego (a promessa da proposta, risco R2, **não medida**), a economia seria de US$ 600–800/mês na tarifa Bunny Standard (US$ 300–400/mês na Volume). É um **teto**, e ainda exclui custo de tracker, suporte e integração.
- No CloudFront, a US$ 0,085/GB nos primeiros 9 TB, a mesma conta seria muito maior, mas isso muda com volume e compromisso.

**Condições mínimas para o produto fazer sentido comercial** (a validar; formuladas como perguntas):
1. A economia de entrega, ou o ganho de conversão/retenção, supera o custo de integrar e manter o SDK **para o cliente** — o valor não pode depender só de banda barata.
2. O cliente distribui em um canal em que **ele paga a entrega** (ou onde o tempo de instalação importa).
3. O produto não cria um custo oculto para o jogador (upload, franquia, queixas ao suporte, bloqueio por operadora — cf. F3/F4).
4. A cobrança do produto cabe dentro da economia gerada (backlog de preços só depois do Portão 2, [doc 15](../15-portao-fase-0-e-plano-3-recursos.md)).

## 5. Possíveis objeções decisivas (a verificar sem esperar a F3)
| Objeção | Já dá para verificar sem a F3? | Como |
| --- | --- | --- |
| "O custo de CDN já é baixo demais" | **Parcialmente** (preços acima) | Completar com termos de uso dos provedores e volume típico de cliente |
| "Quem distribui por loja não paga entrega" | **Parcialmente** | Ler o contrato de distribuição da Steam (não lido) e termos de outras lojas |
| "O que a Blizzard fez e por quê" | **Não, sem fonte** | Fonte oficial (post, entrevista, apresentação técnica); ver pedido abaixo |
| "Jogadores não aceitam upload" | **Não** | Só com dado real de adoção; simulação não prova demanda |
| "Operadoras limitam P2P" | **Parcialmente** (caso Rogers 2011) | Verificar se há casos recentes |

## 6. Pedido ao dono (sem custo e sem contato externo)
O que impede este estudo de ir além é o acesso. Duas opções, escolha do dono:
1. **Leitura pelo dono** dos artigos de suporte da Blizzard que exigem login (`us.support.blizzard.com/en/article/28110` e `…/26021`), colando aqui o trecho sobre P2P, se houver.
2. **Autorizar buscas adicionais** em arquivos públicos (Internet Archive, apresentações em eventos de desenvolvedores) para achar o motivo oficial. Só leitura de páginas públicas.

O estudo **não** concluiu que o risco R12 está resolvido nem que se confirma. Até haver fonte, ele continua com probabilidade "desconhecida" no [doc 12](../12-riscos.md).
