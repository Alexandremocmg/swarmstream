# ADR-0008: Núcleo open source sob Apache-2.0

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
Desenvolvedores e estúdios precisam confiar em software que usa a rede e o disco do jogador. Um núcleo aberto facilita essa confiança, a auditoria de segurança e as contribuições externas. O produto pode ainda ter partes comerciais (serviços hospedados, suporte, integração com publishers).

## Decisão
- O núcleo (`sw-core`, `sw-pack`, SDK Godot) é **open source**.
- **Licença: Apache-2.0**, confirmada pelo dono do produto. Motivos: uso comercial livre, concessão explícita de patentes e compatibilidade com as dependências conhecidas. Uma consulta jurídica rápida antes da primeira publicação pública continua recomendada.
- O que fica aberto ou fechado nos serviços de backend e no launcher é decisão separada (ADR futuro).

## Opções consideradas
| Licença | Prós | Contras |
| --- | --- | --- |
| **Apache-2.0** | Uso comercial livre; concessão explícita de patentes; padrão em infraestrutura; compatível com MIT/BSD | Não obriga a devolver melhorias |
| **MIT** | Mais simples e curta; compatível com Godot (MIT) | Sem cláusula de patentes |
| **MPL-2.0** | Melhorias nos arquivos do núcleo voltam à comunidade; ainda permite uso em jogos fechados | Mais complexa; menos comum em SDKs de jogo |
| **GPL/AGPL** | Obriga abrir derivados | Incompatível com jogos comerciais fechados; afasta publishers |

Licenças das dependências conhecidas (todas compatíveis com Apache-2.0): libtorrent (BSD-3), Godot e godot-cpp (MIT), Zstd (BSD/GPL dupla, usar BSD), LZ4 (BSD-2 na biblioteca), Boost (BSL-1.0).

## Consequências
- (+) Confiança, auditoria externa, contribuições, adoção por estúdios indie.
- (+) Contribuições externas nas tarefas `boa-primeira-tarefa`, com revisão obrigatória de um dono do módulo.
- (−) Concorrentes podem usar o código; o diferencial passa a ser execução, serviços e integração com publishers.
- (−) Exige processo: arquivo `LICENSE`, cabeçalhos, `NOTICE` com as licenças de terceiros, política de contribuição (DCO ou CLA), política de divulgação de vulnerabilidades (`SECURITY.md`).
- Dados de assets de teste (CC0 etc.) mantêm as licenças originais e ficam fora do repositório principal ou em LFS com a lista de licenças.
