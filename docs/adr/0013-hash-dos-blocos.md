# ADR-0013: SHA-256 em todo hash criptográfico; XXH3 só para checksum local

**Status:** Proposta · **Data:** 2026-10-17 · **Autor:** arquiteto/tech lead · **Revisor:** sênior de redes/sistemas (prazo: 2026-10-23)

## Contexto
A pergunta em aberto em [03](../03-formato-de-conteudo.md#questões-em-aberto): BLAKE3 para os blocos (mais rápido) ou SHA-256 em tudo (uma só primitiva)?

O que já está fixado por decisões aceitas:
- As **peças** são verificadas por árvore Merkle **SHA-256** com folhas de 16 KB, porque o torrent v2 (BEP 52) exige isso e usamos o libtorrent ([ADR-0001](0001-poc-godot-libtorrent.md), [ADR-0002](0002-chunk-em-dois-niveis.md)). Ou seja: SHA-256 está no projeto de qualquer jeito.
- O **identificador do bloco** é o hash do conteúdo ([ADR-0007](0007-enderecamento-por-conteudo.md)).
- Na leitura do cache, um checksum rápido (XXH3) por bloco detecta corrupção do disco ([04](../04-cache-local.md#estrutura-em-disco)).

Onde o hash do bloco é calculado de fato:

| Lugar | Quando | Volume | Observação |
| --- | --- | --- | --- |
| `sw-pack` (build) | Uma vez por build, em paralelo | Jogo inteiro | O gargalo é a compressão Zstd nível 15–19, ordens de grandeza mais lenta que qualquer hash |
| `sw-core`, recebimento de peça | Toda peça vinda da rede | Bytes baixados | Merkle **SHA-256** obrigatória (BEP 52); não há escolha |
| `sw-core`, primeira entrada do bloco no cache de RAM | Depois de descomprimir, uma vez por bloco ([03 §4](../03-formato-de-conteudo.md#4-frame-de-bloco-dentro-da-peça)) | Bytes descomprimidos lidos pelo jogo | **Único ponto em que o hash do bloco fica no caminho de leitura.** Em software, SHA-256 de 128 KB custa da mesma ordem que descomprimir o bloco com Zstd; com instruções SHA, bem menos |
| `sw-core`, patch | Compara identificadores dos manifestos antigo e novo; não recalcula | — | — |
| Serviço de manifestos (Go) | Valida o manifesto; não recalcula blocos | — | — |

Com instruções SHA (x86 desde ~2019, ARMv8), o SHA-256 passa de 1 GB/s por núcleo (ordem de grandeza publicada; medir na F1). O BLAKE3 seria mais rápido sobretudo em máquinas sem essas instruções.

## Opções consideradas
1. **SHA-256 em tudo que é criptográfico** — prós: uma primitiva só; exigida pelo BEP 52 de qualquer forma; aceleração em hardware; biblioteca padrão do Go ([ADR-0010](0010-linguagem-dos-servicos-de-backend.md)); aceita em auditorias de publishers (FIPS 180-4). Contras: mais lento que BLAKE3 em software sem instruções SHA.
2. **BLAKE3 nos blocos + SHA-256 nas peças** — prós: hash de bloco 2–5× mais rápido em software, o que importa na verificação pós-descompressão. Contras: duas primitivas para manter, testar e documentar no protocolo aberto; sem biblioteca padrão no Go; não é padrão FIPS; a vantagem encolhe muito em CPUs com instruções SHA.
3. **BLAKE3 em tudo** — inviável: quebra a compatibilidade com o torrent v2.

## Decisão
| Uso | Algoritmo | Calculado sobre | Onde fica |
| --- | --- | --- | --- |
| Identificador do bloco | **SHA-256**, 32 bytes no v0 | Bytes **descomprimidos** do bloco (o identificador não muda se o codec mudar) | Manifesto, coluna própria ([ADR-0012](0012-serializacao-do-manifesto.md)) |
| Raiz da peça | **Merkle SHA-256**, folhas de 16 KB (BEP 52) | Bytes **gravados** da peça (comprimidos e, se houver, cifrados), para que peers validem sem a chave ([03](../03-formato-de-conteudo.md#sigilo-opcional-por-publisher)) | Manifesto e torrent v2 |
| Assinatura do manifesto | **Ed25519 puro** (RFC 8032), sem pré-hash | Bytes do envelope ([ADR-0012](0012-serializacao-do-manifesto.md)) | Fim do envelope |
| `key_id` da chave pública | SHA-256, 8 primeiros bytes | Chave pública | Envelope; só seleciona a chave, não dá segurança |
| Checksum local na leitura | **XXH3-64** (não criptográfico) | Bytes gravados do bloco | Índice do cache, calculado pelo cliente **depois** de a peça passar na verificação Merkle; nunca vem da rede |

- O envelope declara o algoritmo (`hash_alg = 1`, SHA-256; [03 §2](../03-formato-de-conteudo.md#2-primitivas-criptográficas-v0)). O valor 2 fica reservado para BLAKE3 **só no identificador do bloco**, se a medição abaixo pedir. Trocar exige ADR novo; o cliente recusa algoritmo que não conhece.
- Truncar o identificador para 16 bytes (saída (b) de [03 §7.3](../03-formato-de-conteudo.md#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo)) não está decidido aqui: exige ADR com a medição da F1-04.
- Nenhum dado vindo de peer é aceito por XXH3: o XXH3 protege contra o disco, não contra atacante.

## Consequências
- (+) Uma primitiva criptográfica em todo o sistema, igual em C++ e Go, simples de documentar no protocolo aberto.
- (+) Identificador sobre bytes descomprimidos: recomprimir um asset com outro codec não gera bloco "novo" para o cálculo de patch.
- (−) Em máquinas sem instruções SHA, a verificação pós-descompressão pode custar tanto quanto a própria descompressão. É o principal risco desta decisão e o que a F1 mede.
- (−) Build um pouco mais lento nessas máquinas. Irrelevante frente ao Zstd, a confirmar.
- (−) 32 bytes por bloco no manifesto (≈ 39 MB para 150 GB); por isso a coluna fica separada e fora da memória residente (ADR-0012).
- Verificação, na F1, com números da bancada (máquina de referência e uma sem instruções SHA):
  1. fração do tempo de build do `sw-pack` gasta em hash (relatório da CLI, F1-03);
  2. tempo da verificação SHA-256 pós-descompressão dividido pelo tempo de descompressão, por bloco, e efeito nos quadros > 33 ms da rota.
  Reabrir se (1) passar de 10% do build, ou se (2) passar de 50% **e** a verificação aparecer nos quadros com stutter. Antes de trocar de algoritmo, considerar a saída mais barata: verificar por amostragem em release e sempre em debug e na bancada (mudança em 03 §4, decidida por ADR).
