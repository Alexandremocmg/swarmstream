# Segurança e privacidade

## Ameaças e respostas

| Ameaça | Resposta |
| --- | --- |
| Peer envia dado adulterado ou corrompido | Verificação Merkle por folha de 16 KB; peça inválida descartada, peer penalizado e depois banido da sessão. |
| Manifesto falso (fazer o cliente aceitar outro conteúdo) | Manifesto assinado com Ed25519; chave pública da publisher embutida no jogo/launcher; rotação de chave prevista no formato. |
| Pirataria: baixar o jogo pelo swarm sem comprar | O swarm é só transporte. Com **cifra por pacote** opcional, peers transportam texto cifrado; a chave vem do serviço de licença da publisher. O DRM continua sendo da publisher. |
| Exposição de IP do jogador a outros jogadores | Upload desligado em partidas online; opção de desligar P2P totalmente; tracker não devolve peers da mesma partida; documentar na política de privacidade. |
| DDoS contra jogador cujo IP vazou | Mesmas medidas acima; jogos competitivos podem desligar P2P durante partidas por padrão. |
| Envenenamento do tracker (peers falsos) | Tracker autenticado (token do cliente), limite de taxa por IP/conta. |
| Cliente modificado para só baixar (leecher) | Tolerado; tit-for-tat reduz prioridade. Não é ameaça de segurança. |
| Vulnerabilidade de parsing no cliente (dados de peers chegam pela rede) | Código de parsing pequeno, fuzzing contínuo (libFuzzer) das mensagens de rede e do manifesto, compilação com sanitizers no CI. |

## Privacidade (LGPD / GDPR)

- IP é dado pessoal. O tracker guarda IP só enquanto o peer está ativo e logs com retenção curta.
- Telemetria sem identificador pessoal por padrão; ID anônimo por instalação; opção de recusar.
- Consentimento claro no launcher para upload P2P ("Ajude outros jogadores a baixar mais rápido"), **desligável a qualquer momento**.
- Revisão jurídica antes do piloto público (tarefa da Fase 5).

## Software do jogador

- Assinatura de código do cliente e do launcher (evita alertas de antivírus e SmartScreen).
- Regra de firewall pedida uma vez, com explicação.
- O cliente nunca abre portas TCP sem UPnP/NAT-PMP consentido.

## Responsáveis
Segurança é transversal, mas tem um dono: o **arquiteto/tech lead** aprova mudanças em verificação, assinatura e parsing de rede. Ver [10-equipe-e-niveis.md](10-equipe-e-niveis.md).
