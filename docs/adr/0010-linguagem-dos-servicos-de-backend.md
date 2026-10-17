# ADR-0010: Serviços de backend em Go

**Status:** Proposta · **Data:** 2026-10-17 · **Autor:** arquiteto/tech lead · **Revisor:** sênior de redes/sistemas (prazo: 2026-10-23)

## Contexto
O [ADR-0006](0006-cpp20-nucleo-com-api-c.md) deixou a linguagem dos serviços entre Go e Rust. O [ADR-0009](0009-abertura-de-backend-e-launcher.md) (formato B) mudou o peso da decisão: o backend passa a ser um **serviço pago, multi-cliente, que nós operamos**, com protocolos abertos para que o estúdio possa hospedar o próprio.

Serviços que entram em `sw-services`:

| Serviço | Carga típica | Ponto sensível |
| --- | --- | --- |
| Tracker | Muitas conexões curtas, pouco CPU | Localidade (IP → ASN), limite de taxa, retenção curta de IP |
| Serviço de manifestos | Leitura de arquivos assinados, cache HTTP | Validar o manifesto antes de publicar |
| Coletor de telemetria | Escrita em lote | Volume, anonimização |
| STUN | — | Fica no **coturn** (ADR-0004); não escrevemos |

Todos são limitados por I/O e rede, não por CPU. Nenhum está no caminho de uma falta de cache: se o backend cair, a CDN continua servindo ([ADR-0003](0003-agendador-por-prazo.md)). Até a Fase 3 a bancada usa arquivos estáticos e um tracker open source; código próprio começa no fim da F2 e cresce na F4 (contas, isolamento, medição de uso, cobrança).

Quem vai escrever e operar: o sênior de backend/operação contratado na F2 e plenos. O núcleo continua em C++20; o backend **não compartilha código** com ele, só formatos e protocolos.

## Opções consideradas

| Critério | **Go** | Rust |
| --- | --- | --- |
| Segurança de memória ao ler dados não confiáveis | Sim (GC) | Sim (sem GC) |
| Produtividade para serviços HTTP/UDP simples | Alta: `net/http`, `crypto/ed25519`, `crypto/sha256` na biblioteca padrão | Média: async (tokio) tem curva maior |
| Latência de cauda | Pausas de GC na casa de sub-ms; irrelevante aqui (nada no caminho da falta) | Melhor, mas não precisamos |
| Operação (deploy, observabilidade, profiling) | Binário estático, `pprof`, ecossistema Prometheus/OpenTelemetry maduro | Bom, menos padronizado |
| Contratação (sênior de backend/operação na F2) | Perfil comum | Perfil mais raro e mais caro |
| Leitura do manifesto | FlatBuffers com suporte oficial ([ADR-0012](0012-serializacao-do-manifesto.md)) | Suporte oficial |
| Facilidade de self-host por estúdios (exigência do ADR-0009) | Binário único, imagem Docker pequena | Igual |

Também descartados: **C++** (repetir no servidor o risco de memória que já aceitamos no cliente, sem o motivo de integração com engines) e **Python/Node** (bons para a bancada, fracos em operação multi-cliente de longa duração).

## Decisão
- Serviços próprios de `sw-services` em **Go** (versão estável fixada no `go.mod`; atualização junto com a revisão de dependências do projeto).
- STUN continua com **coturn**; tracker open source até a F4, como já previsto em [05](../05-rede-e-agendador.md#serviços-de-backend).
- Cada serviço expõe um **protocolo documentado** (ADR-0009); a implementação em Go é uma implementação, não a definição.
- Formatos compartilhados com o núcleo (manifesto, hashes) são testados nos dois lados com os **mesmos vetores de teste** (arquivos em `sw-pack/testdata/`), para que C++ e Go nunca divirjam em silêncio.

## Consequências
- (+) Serviços simples de escrever e operar; binário único facilita o self-host prometido no ADR-0009.
- (+) Biblioteca padrão cobre Ed25519 e SHA-256 ([ADR-0013](0013-hash-dos-blocos.md)) sem dependência extra.
- (−) Uma segunda implementação dos formatos (C++ no cliente, Go no servidor). Mitigação: vetores de teste compartilhados e fuzzing do leitor Go do manifesto (`go test -fuzz`).
- (−) Mais uma linguagem no repositório (C++, Go, Python da bancada). O CI ganha um job Go a partir da F2.
- Reavaliar se algum serviço passar a ficar no caminho de uma falta de cache (latência de cauda passaria a importar).
