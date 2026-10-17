# ADR-0005: Cache em slots pré-alocados, leases e thread dona

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
Pergunta da pauta: como limpar o cache em tempo real sem conflito com a thread principal. Apagar e criar arquivos por chunk fragmenta o disco e gera contenção; remover algo que está sendo lido corrompe dados.

## Decisão
- Arquivos grandes pré-alocados, divididos em slots do tamanho de uma peça.
- Uma **thread de I/O dona** do índice; as outras threads só enviam pedidos.
- **Leases** com contagem de referência: peça com lease nunca é removida.
- Ordem de escrita: dados → flush → journal → flush. Todo retorno de escrita é conferido.
- Remoção CLOCK em background com marcas d'água 95%/85%; conjunto fixo para o `core`.

## Consequências
- (+) Remover é uma operação de metadados; sem fragmentação; o jogo nunca espera limpeza.
- (+) Recuperação após queda pelo journal.
- (−) O teto do cache é reservado no disco de uma vez (o jogador vê o espaço ocupado). Comunicar no launcher.
