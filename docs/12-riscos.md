# Riscos

| # | Risco | Prob. | Impacto | Mitigação | Fase em que se verifica |
| --- | --- | --- | --- | --- | --- |
| R1 | Faltas de cache causam stutter ou pop-in inaceitável | Média | **Alto** | Política de falta, core com mips/LODs baixos, prefetch com traces | F1 (Portão 1) |
| R2 | O swarm absorve bem menos que 60–80% do tráfego | Média | Alto | Agendador por prazo garante qualidade mesmo assim; modelo de custo realista | F3 (Portão 2) |
| R3 | Economia de CDN pequena demais para vender | Média | Alto | Vender retenção e tempo até jogar; medir no piloto | F3, F5 |
| R4 | Jogadores rejeitam upload (franquia, lentidão, desconfiança) | Média | Médio | QoS, opt-in claro, desligar em conexão com franquia, transparência | F4, F5 |
| R5 | Unreal/Epic lança solução nativa equivalente | Média | Alto | Começar por Godot/indies; diferencial P2P e multi-engine; avaliar integrar em vez de competir | Contínuo |
| R6 | Vulnerabilidade no parsing de dados de peers | Baixa | **Alto** | Fuzzing, sanitizers, revisão obrigatória, código de parsing pequeno | F2 em diante |
| R7 | Integração do libtorrent com nosso cache mais difícil que o previsto | Média | Médio | Interface `IContentSource` permite trocar; spike no início da F2 | F2 |
| R8 | NAT/CGNAT impede P2P para muitos jogadores | Média | Médio | CDN como reserva; medir por tipo de NAT; LAN e ISP primeiro | F3 |
| R9 | Corrupção silenciosa do cache persistida | Baixa | Alto | Journal, retornos conferidos, checksum na leitura, testes de queda | F1 |
| R10 | Questões legais (privacidade, termos, uso de upload) | Média | Médio | Revisão jurídica antes do piloto; telemetria anônima | F5 |
| R11 | Equipe sem experiência em engine ou em P2P | Média | Alto | Contratar os dois sêniores com esses perfis antes da F1 | F0 |
| R12 | Lições do P2P abandonado da Blizzard se repetirem | Desconhecida | Alto | Estudar o caso antes do Portão 2 (tarefa de produto) | F3 |
| R13 | Operar o backend pago (disponibilidade, custo de infraestrutura, plantão, cobrança) custa mais do que o previsto | Média | Alto | Sênior de backend/operação desde a F2; a CDN continua servindo se o serviço cair; começar com piloto de poucos clientes; protocolos abertos reduzem o risco de dependência | F2, F4, F5 |
| R14 | Fator ônibus = 1: com 3 recursos, um único sênior concentra cache, agendador e Godot | Alta | Alto | Contratos e mocks para trabalhar sem ele; decisões em ADR; revisão cruzada de código crítico | F1 em diante |
| R15 | A bancada não mede o que se pretende (carga limitada por decodificação de JPEG, não por disco; stutter com amostra de 3 quadros) | Alta | Alto | Texturas em formato GPU comprimido; limites absolutos de stutter; validar a bancada com uma mudança conhecida antes de confiar nos números | F1 (Portão 1) |

| R16 | O loader na frente (ADR-0016) não cobre `FileAccess`/`ConfigFile`, ou não consegue materializar algum tipo pesado a partir de bytes | Média | Alto | Leituras diretas ficam no `core`; portão de materialização no início do F1-20; `.pck` por grupo como reserva; protótipo C para medir o teto | F1 |

Revisar esta tabela no fim de cada fase.
