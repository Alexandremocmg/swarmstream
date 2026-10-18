# ADR-0015: Contribuições com DCO, sem CLA

**Status:** Aceita (2026-10-17: revisão R1, leitura independente, aprovação do dono do produto) · **Data:** 2026-10-17 · **Autor:** R1 (líder técnico)

**Revisão (regra de 3 pessoas, [15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)):** R1 revisou · leitura e comentários: revisor independente · aprovação: dono do produto (2026-10-17). Registro: [revisão F0](../revisoes/F0-revisao-adrs-0010-0016.md)

## Contexto
O [ADR-0008](0008-licenca-open-source.md) deixou em aberto a política de contribuição: **DCO** (*Developer Certificate of Origin*) ou **CLA** (*Contributor License Agreement*). A decisão precisa existir antes do primeiro commit (F0-01), porque trocar de política depois exige reunir aceite retroativo de quem já contribuiu.

Fatos que pesam:
- A Apache-2.0 já diz, na seção 5, que toda contribuição enviada ao projeto entra sob a própria Apache-2.0, com a concessão de patentes da seção 3.
- Pelo [ADR-0009](0009-abertura-de-backend-e-launcher.md) (formato B), a receita vem do backend hospedado, que é código nosso, não de contribuições externas. A Apache-2.0 já permite usar o núcleo, inclusive as contribuições, num produto comercial.
- O público inicial são estúdios indie e contribuidores ocasionais (tarefas `boa-primeira-tarefa`). Burocracia na primeira contribuição afasta exatamente esse público.

## Opções consideradas
1. **DCO** — quem contribui declara, em cada commit (`Signed-off-by:`), que tem o direito de enviar aquele código sob a licença do projeto. Prós: zero cadastro; padrão do kernel Linux, CNCF e GitLab; verificável no CI. Contras: não dá ao projeto direito de **relicenciar** as contribuições.
2. **CLA** — contrato assinado uma vez, normalmente com cessão ampla de direitos. Prós: permite relicenciar no futuro. Contras: atrito na primeira contribuição; precisa de revisão jurídica e de ferramenta de coleta; gera desconfiança em projeto que tem parte comercial ("querem fechar o código depois").

## Decisão
- **DCO 1.1** em todo commit, de qualquer pessoa, incluindo a equipe: `git commit -s`.
- O CI recusa PR com commit sem `Signed-off-by:` que bata com o autor do commit (job a criar na F0-03/F0-04). Ficam isentos os bots configurados no repositório (por exemplo, o Dependabot), que não podem assinar o DCO; commits feitos pela interface web do GitHub precisam da linha, e o GitHub oferece a opção de incluí-la.
- **Sem CLA.** Não planejamos relicenciar o núcleo; se um dia isso for necessário, será um ADR novo com consulta jurídica.
- Todo arquivo de código novo começa com o cabeçalho SPDX descrito no [CONTRIBUTING](../../CONTRIBUTING.md#licença-dco-e-cabeçalhos).

## Consequências
- (+) Primeira contribuição sem cadastro; mensagem clara de que o núcleo continua aberto.
- (+) Origem de cada linha registrada no histórico do git.
- (−) O projeto não pode relicenciar contribuições externas sem a autorização de cada autor. Aceito: a Apache-2.0 já cobre o modelo de negócio do ADR-0009.
- (−) Commits sem `-s` são o erro mais comum de quem chega; o CONTRIBUTING mostra como corrigir.
- Consulta jurídica rápida antes da primeira publicação pública (já recomendada no ADR-0008) confirma esta escolha.
