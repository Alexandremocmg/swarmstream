# Métricas e testes

"Medir antes de afirmar" é princípio do projeto. Toda fase tem métricas de saída ([09-roadmap-fases.md](09-roadmap-fases.md)).

## Métricas principais

| Métrica | Definição | Meta inicial (a validar) |
| --- | --- | --- |
| **Taxa de falta** | Leituras do jogo que não estavam locais / total de leituras | < 1% jogando; 0% em lógica/colisão |
| **Espera do jogo** p50/p95/p99 | Tempo que o jogo esperou numa falta | p99 < 300 ms com rede de 50 Mbps |
| **Quadros com stutter** | Quadros > 33 ms atribuídos a I/O | Limites absolutos sobre o baseline com SSD local ([medicoes/baseline.md §3](medicoes/baseline.md)): > 33 ms ≤ 2 e > 100 ms = 0 por rodada de 60 s, p99 ≤ baseline + 2 ms |
| **Pop-in** | Tempo com mip/LOD rebaixado na tela | Medir; meta definida na Fase 1 |
| **% de bytes de peers** | Bytes de peers / bytes totais baixados | 60–80% em regime (proposta original) — **a provar** na Fase 3 |
| **Tempo até jogar** | Do clique em instalar até o primeiro controle | < 10 min com 100 Mbps para jogo de 150 GB |
| **Impacto no jogador** | Aumento de RTT com upload ligado | < 20 ms |
| **Peças inválidas** | Peças rejeitadas na verificação | Contar e alertar; 0 aceitas |

## Eventos de telemetria (cliente → coletor)

`read_miss` (arquivo, bloco, prazo, espera real, fonte), `piece_done` (fonte, bytes, duração), `piece_invalid` (peer, motivo), `eviction` (peças, bytes), `qos_change` (limite novo, motivo), `frame_stutter` (duração, causa atribuída), `session` (modo, teto de cache, tipo de NAT).

Inspirado na telemetria do porte de referência: FPS, decomposição dos piores quadros, esperas de I/O e "leituras lentas" com o nome do arquivo e se havia dica para ela.

## Bancada de testes `sw-netlab`

| Componente | Função |
| --- | --- |
| Servidor CDN simulado | HTTP Range com latência, banda e perda configuráveis |
| Perfis de rede | `fibra_300`, `cabo_50`, `adsl_10`, `4g_instavel`, `cgnat` — via `tc netem` em Linux/containers |
| Peers sintéticos | N containers rodando o `sw-core` em modo headless, com mistura de seeders completos e jogadores |
| Jogador robô | Godot headless percorrendo uma rota gravada no mapa de teste, gerando leituras reais |
| Relatório | Script que junta os eventos e gera as métricas acima por rodada |

## Tipos de teste

| Tipo | Onde | Quem costuma escrever |
| --- | --- | --- |
| Unidade (chunker, Merkle, índice, agendador) | CI em todo PR | Todos; júnior e pleno principalmente |
| Propriedades / fuzzing (manifesto, mensagens de rede, journal) | CI noturno | Pleno/sênior |
| Recuperação de falhas (matar processo no meio da escrita, disco cheio) | CI noturno | Pleno |
| Integração (núcleo + CDN simulada) | CI | Pleno |
| Rodada de bancada (rota completa, perfis de rede) | Sob demanda e semanal | Sênior define, pleno/júnior executa |
| Teste com jogadores reais | Fase 5 | Produto + toda a equipe |

## Mapa de teste (PoC)
Projeto Godot com 2–5 GB de assets pesados de licença livre (ex.: Poly Haven, CC0), divididos em regiões, com uma rota de câmera gravada para repetir as rodadas de forma idêntica.
