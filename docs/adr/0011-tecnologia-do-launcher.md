# ADR-0011: Launcher em Tauri 2, com o `sw-core` ligado pela API C

**Status:** Proposta · **Data:** 2026-10-17 · **Autor:** arquiteto/tech lead · **Revisor:** sênior de engine/streaming (prazo: 2026-10-23)

## Contexto
O `sw-launcher` é o programa do jogador: instalação (modos compacto, completo e personalizado), configuração de upload, consentimento P2P e seed em segundo plano. Pelo [ADR-0009](0009-abertura-de-backend-e-launcher.md) ele é **aberto ou gratuito**, porque controla o upload de quem joga e a confiança é parte do produto (risco R4).

Requisitos que pesam na escolha:
1. **Leve.** Fica aberto na bandeja do sistema servindo peças; jogador desinstala launcher pesado.
2. **Liga o `sw-core`** pela API C ([ADR-0006](0006-cpp20-nucleo-com-api-c.md)), sem reescrever nada do núcleo.
3. **Windows e Linux**, com instalador, assinatura de código e atualização automática **assinada** ([07](../07-seguranca-e-privacidade.md#software-do-jogador)).
4. **Licença compatível** com um projeto Apache-2.0, sem obrigações extras de redistribuição.
5. Telas e textos feitos por plenos e juniores ([10](../10-equipe-e-niveis.md), trilha T7).

O launcher só é construído na Fase 4. A decisão sai agora para que nenhuma escolha das fases 1–3 (API, cache) a contrarie.

## Opções consideradas

| Opção | Prós | Contras |
| --- | --- | --- |
| **Tauri 2** (Rust + webview do sistema) | Binário pequeno (~10 MB); bandeja, instalador e atualizador com assinatura já prontos; MIT/Apache-2.0; UI em HTML/TS, fácil para plenos e juniores; FFI de Rust para C é direta | Adiciona Rust (fina camada de cola); depende do WebView2 no Windows (já presente no Windows 11; instalador o inclui no Windows 10) e do WebKitGTK no Linux, de qualidade variável por distribuição |
| Qt 6 (C++/QML) | Mesma linguagem do núcleo; link direto; nativo e leve | LGPL exige link dinâmico e permitir troca da biblioteca, ou licença comercial; atualizador e assinatura por nossa conta; SDK grande |
| Electron | Ecossistema enorme; UI web | 100+ MB e muita RAM para um programa que fica aberto sempre: contradiz o requisito 1 e a confiança |
| Wails (Go + webview) | Mesma linguagem do backend ([ADR-0010](0010-linguagem-dos-servicos-de-backend.md)) | Comunidade menor; atualizador não incluso; cgo complica o build cruzado |
| Godot como UI | Equipe já conhece; reaproveita o `sw-godot` | Renderiza o tempo todo; sem bandeja do sistema; acessibilidade fraca |

## Decisão
- Launcher em **Tauri 2.x** (versão fixada no `Cargo.lock`): UI em HTML/TypeScript, processo em Rust.
- O processo Rust **liga o `sw-core` como biblioteca dinâmica** pela API de `swarmstream.h`. Nenhuma lógica de cache, rede ou verificação no Rust; ele só chama a API e mostra o estado.
- Atualizações do launcher usam o atualizador assinado do Tauri; a chave de assinatura fica fora do repositório.
- **Regra de posse do cache (vale para o launcher e para o jogo):** só **um processo por vez** é dono de uma pasta de cache, e a posse é garantida por um *lock* de arquivo na pasta. `sw_init` devolve `SW_E_CACHE_LOCKED` se outro processo já é dono. Quando o jogo abre, o launcher solta o cache e o jogo assume o seed (com a QoS do jogo); quando o jogo fecha, o launcher retoma. O protocolo de passagem entre os dois é tarefa da Fase 4.

## Consequências
- (+) Launcher leve, aberto, com atualização assinada pronta, e telas acessíveis a plenos e juniores.
- (+) A regra de um só dono preserva a thread dona do cache ([ADR-0005](0005-cache-em-slots-com-leases.md)) entre processos, e entra já no `swarmstream.h` v0 ([ADR-0014](0014-contrato-swarmstream-h-v0.md)).
- Chaves confiáveis do manifesto: quem chama `sw_init` (jogo ou launcher) passa a lista em `sw_config.trusted_keys`; o envelope escolhe pelo `key_id` ([03 §7.1](../03-formato-de-conteudo.md#71-envelope-cabeçalho-fixo--payload)). Rotação: publicar primeiro um build que confia na chave nova e só depois assinar com ela. Revogação de chave fica para o ADR de segurança da F4.
- (−) Rust entra no projeto, ainda que só como cola. O pleno do launcher (F4) precisa de noções de Rust e de FFI.
- (−) Testar a UI em WebKitGTK de várias distribuições Linux. Mitigação: suportar oficialmente só Ubuntu LTS e SteamOS no piloto.
- (−) A API pública precisa servir o launcher (limite de upload, P2P ligado/desligado, estatísticas). O que faltar para instalação e modo completo entra por ADR na F4, como mudança compatível.
- Reavaliar no Portão 2 se o piloto mudar de plataforma (por exemplo, macOS) ou se o tamanho medido do launcher passar de 30 MB instalado.
