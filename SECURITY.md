# Política de segurança

O Swarmstream roda no PC do jogador, usa a rede e o disco dele e recebe dados de outros jogadores. Levamos falhas de segurança a sério e preferimos saber delas em privado, antes de qualquer divulgação.

## Como reportar
**Não abra issue pública, PR ou discussão** sobre uma vulnerabilidade.

1. Canal principal: **GitHub → aba Security → "Report a vulnerability"** (relato privado). Só os mantenedores veem.
2. Canal alternativo, se não puder usar o GitHub: `autokabem@gmail.com`.

Inclua, se tiver:
- componente e versão (commit) afetados;
- passos para reproduzir ou uma prova de conceito mínima (um manifesto, uma mensagem de rede, uma entrada de fuzzing);
- impacto que você observou ou imagina;
- se quer ser creditado, e com que nome.

## O que acontece depois

| Etapa | Prazo |
| --- | --- |
| Confirmamos o recebimento | até 3 dias úteis |
| Avaliação inicial (aceita ou não, gravidade) | até 10 dias úteis |
| Correção disponível | meta de 90 dias; antes, se for grave e explorável |
| Divulgação pública (aviso + crédito) | combinada com quem reportou, depois da correção |

Se a correção atrasar, avisamos quem reportou e combinamos uma nova data. Se a falha já estiver sendo explorada, podemos publicar antes um aviso com medidas de contorno.

## Escopo

| Dentro | Exemplos |
| --- | --- |
| `sw-core` | Leitura de dados de peers e da CDN, verificação Merkle, leitura do manifesto, cache em disco, journal |
| `sw-pack` | Geração e assinatura do manifesto |
| `sw-godot` | GDExtension |
| `sw-launcher` | Instalação, atualização, configuração de upload |
| Protocolos documentados | Manifesto, tracker, telemetria |

Também nos interessa: qualquer forma de fazer um cliente aceitar conteúdo que o manifesto assinado não lista, ler ou escrever fora da pasta de cache, ou expor o IP do jogador além do descrito em [docs/07-seguranca-e-privacidade.md](docs/07-seguranca-e-privacidade.md).

**Fora do escopo:** o serviço hospedado em produção (reporte pelo mesmo canal, mas **não teste contra ele** sem autorização por escrito: use a bancada `sw-netlab` local); ataques de negação de serviço por volume; engenharia social; falhas em dependências de terceiros que já tenham correção pública (avise a dependência; ajudamos a atualizar).

## Versões com correção
Antes da versão 1.0, só o branch `main` recebe correções de segurança.

## Porto seguro
Não tomaremos medida legal contra quem pesquisar de boa-fé, dentro deste escopo, sem acessar dados de terceiros, sem degradar serviços e reportando em privado. Se tiver dúvida se algo é permitido, pergunte antes pelo canal acima.

---

## Summary (English)
Please report vulnerabilities **privately** via GitHub → Security → "Report a vulnerability" (or `autokabem@gmail.com`). Do not open public issues. We acknowledge within 3 business days, triage within 10, and aim to ship a fix within 90 days, with coordinated disclosure and credit. In scope: `sw-core`, `sw-pack`, `sw-godot`, `sw-launcher` and the documented protocols. Do not test against the hosted production service; use the local `sw-netlab` test bench. Good-faith research within this policy will not face legal action from us.
