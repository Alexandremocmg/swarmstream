# Registro de decisões de arquitetura (ADR)

Cada decisão técnica importante vira um arquivo curto: contexto, decisão, consequências. Decisões não são apagadas; quando mudam, um ADR novo **substitui** o antigo.

| # | Decisão | Status |
| --- | --- | --- |
| [0001](0001-poc-godot-libtorrent.md) | PoC com Godot 4 + libtorrent-rasterbar | Aceita |
| [0002](0002-chunk-em-dois-niveis.md) | Chunk em dois níveis: bloco 128 KB, peça 1 MB, Merkle 16 KB | Aceita |
| [0003](0003-agendador-por-prazo.md) | Agendador por prazo escolhe entre peers e CDN | Aceita |
| [0004](0004-sem-turn-cdn-como-reserva.md) | Sem TURN para assets; CDN é a reserva | Aceita |
| [0005](0005-cache-em-slots-com-leases.md) | Cache em slots pré-alocados, leases e thread dona | Aceita |
| [0006](0006-cpp20-nucleo-com-api-c.md) | Núcleo em C++20 com API pública em C | Aceita |
| [0007](0007-enderecamento-por-conteudo.md) | Endereçamento por conteúdo (FastCDC) em vez de swarm por versão | Aceita |
| [0008](0008-licenca-open-source.md) | Núcleo open source sob Apache-2.0 | Aceita |
| [0009](0009-abertura-de-backend-e-launcher.md) | O que fica aberto ou fechado no backend e no launcher (formato B) | Aceita |
| [0010](0010-linguagem-dos-servicos-de-backend.md) | Serviços de backend em Go | Aceita |
| [0011](0011-tecnologia-do-launcher.md) | Launcher em Tauri 2; um processo dono por pasta de cache | Aceita |
| [0012](0012-serializacao-do-manifesto.md) | Manifesto em FlatBuffers dentro de envelope assinado | Aceita |
| [0013](0013-hash-dos-blocos.md) | SHA-256 em todo hash criptográfico; XXH3 só no checksum local | Aceita |
| [0014](0014-contrato-swarmstream-h-v0.md) | Contrato `swarmstream.h` v0 e regras de compatibilidade da API | Aceita (header congelado) |
| [0015](0015-politica-de-contribuicao-dco.md) | Contribuições com DCO, sem CLA | Aceita |
| [0016](0016-ponto-de-intercepcao-godot.md) | Ponto de intercepção de leitura no Godot: loader na frente + dicas; `.pck` como reserva | Aceita |
| [0017](0017-conflito-arquivo-diretorio-no-manifesto.md) | Conflito arquivo/diretório no manifesto v0: recusa com `SW_E_MANIFEST_INVALID` | Proposta |

Status possíveis: **Proposta** → **Aceita** → (**Substituída por ADR-XXXX** | **Descartada**).

Os ADRs 0001–0009 foram aceitos pelo dono do produto em 2026-10-17. Daqui em diante vale a **regra de revisão com 3 pessoas** ([15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)): R1 escreve ou revisa, outro recurso lê e comenta, o dono do produto aprova. Achados e respostas ficam registrados em [revisoes/](../revisoes/). Use o [modelo](../templates/adr-template.md).

Os ADRs pendentes da Fase 0 (F0-05) são 0010–0016. Todos aceitos em 2026-10-17; revisão registrada em [revisoes/F0-revisao-adrs-0010-0016.md](../revisoes/F0-revisao-adrs-0010-0016.md).
