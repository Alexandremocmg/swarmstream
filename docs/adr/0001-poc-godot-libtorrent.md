# ADR-0001: PoC com Godot 4 + libtorrent-rasterbar

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
Precisamos validar rápido o risco maior (engine + cache + faltas) e depois o P2P híbrido. Unreal é a engine de AAA, mas tem ciclo de build lento e código grande. WebTorrent é para navegador; o alvo inicial é PC nativo.

## Decisão
- Engine da PoC: **Godot 4** via GDExtension (código aberto, build rápido, `FileAccess` interceptável).
- Rede da PoC: **libtorrent-rasterbar 2.x** (torrent v2, `set_piece_deadline`, web seeds BEP 19, uTP, UPnP).
- A Fase 1 roda **sem P2P**, contra um servidor HTTP com rede simulada.

## Consequências
- (+) Iteração rápida; o libtorrent já resolve integridade, prazos e CDN como seed.
- (+) A interface `IContentSource` deixa trocar o libtorrent depois.
- (−) Resultados no Godot não provam o comportamento no Unreal; a Fase 6 precisa de nova validação.
- (−) Precisamos escrever um storage customizado para o libtorrent gravar no nosso cache.
