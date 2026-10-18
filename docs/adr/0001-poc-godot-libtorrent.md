# ADR-0001: PoC com Godot 4 + libtorrent-rasterbar

**Status:** Aceita (2026-10-17, pelo dono do produto); justificativa da engine corrigida pelo [ADR-0016](0016-ponto-de-intercepcao-godot.md) · **Data:** 2026-10-17

## Contexto
Precisamos validar rápido o risco maior (engine + cache + faltas) e depois o P2P híbrido. Unreal é a engine de AAA, mas tem ciclo de build lento e código grande. WebTorrent é para navegador; o alvo inicial é PC nativo.

## Decisão
- Engine da PoC: **Godot 4** via GDExtension (código aberto, build rápido, leitura de recursos interceptável por um `ResourceFormatLoader` registrado na frente; ver a correção abaixo).
- Rede da PoC: **libtorrent-rasterbar 2.x** (torrent v2, `set_piece_deadline`, web seeds BEP 19, uTP, UPnP).
- A Fase 1 roda **sem P2P**, contra um servidor HTTP com rede simulada.

## Consequências
- (+) Iteração rápida; o libtorrent já resolve integridade, prazos e CDN como seed.
- (+) A interface `IContentSource` deixa trocar o libtorrent depois.
- (−) Resultados no Godot não provam o comportamento no Unreal; a Fase 6 precisa de nova validação.
- (−) Precisamos escrever um storage customizado para o libtorrent gravar no nosso cache.

## Correção (2026-10-17, pelo ADR-0016)
A justificativa original dizia "`FileAccess` interceptável". O [spike F0](../spikes/F0-spike-fileaccess-godot.md) mediu no Godot 4.7.2 que isso é falso para uma GDExtension: `FileAccess` e `DirAccess` não têm métodos virtuais nem classe `…Extension`, e o ponto real de intercepção (`PackedData`/`PackSource`) é interno ao núcleo C++. O que uma GDExtension consegue é registrar um `ResourceFormatLoader` na frente, que atende `load()`, dependências de cena e carga em thread, mas não `FileAccess.open`, `ConfigFile` nem listagem de diretório.

A **decisão continua de pé** (Godot 4 + GDExtension + libtorrent; Fase 1 sem P2P): Godot segue sendo a engine mais barata para validar o risco do lado do jogo, e o loader na frente basta para o PoC. O que muda está no [ADR-0016](0016-ponto-de-intercepcao-godot.md): o ponto de intercepção, a regra de que leituras por `FileAccess` ficam no `core` e o portão de materialização na F1-20. Por isso este ADR não foi substituído, só corrigido.
