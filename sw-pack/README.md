# sw-pack

Ferramenta de empacotamento de conteúdo do Swarmstream — trilha **T1
(Conteúdo/build)**. Divide o build do jogo em blocos comprimidos, identificados
por hash, e gera o manifesto assinado que a publisher distribui.

> **Estado:** Fase 0. O CMake e o vcpkg estão ligados ([F0-02](../docs/11-backlog-inicial.md)),
> mas ainda não há fonte: a biblioteca é `INTERFACE` (build vazia). O conteúdo
> real depende da especificação v0 do manifesto ([F0-08](../docs/11-backlog-inicial.md)).

## O que vai morar aqui

- Chunker FastCDC com tamanhos mín/médio/máx configuráveis (F1-01)
- Compressão por bloco Zstd/LZ4 por tipo de arquivo (F1-03)
- Geração e leitura do manifesto v0 (F1-04), classificação core/grupo/opcional (F1-05)
- Ordenação de blocos em peças por localidade (F1-06)
- Relatório de tamanho por grupo, nº de peças e taxa de compressão (F1-08)
- Torrent v2 por pacote com web seed (F2-03)

## Contrato

Só conversa com o resto do sistema pelos contratos de
[docs/02-arquitetura.md](../docs/02-arquitetura.md); não inclui cabeçalhos de
`sw-core` diretamente sem passar por esses contratos.

## Compilação

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Referências

- [docs/03-formato-de-conteudo.md](../docs/03-formato-de-conteudo.md)
- [docs/09-roadmap-fases.md](../docs/09-roadmap-fases.md)
