# Como contribuir

## Antes de tudo
1. Leia [docs/01-visao-e-escopo.md](docs/01-visao-e-escopo.md), [docs/02-arquitetura.md](docs/02-arquitetura.md) e o [glossário](docs/glossario.md).
2. Veja o que se espera do seu nível em [docs/10-equipe-e-niveis.md](docs/10-equipe-e-niveis.md).
3. Escolha uma tarefa em [docs/11-backlog-inicial.md](docs/11-backlog-inicial.md). Chegando agora? Procure `boa-primeira-tarefa`.

## Ambiente (a completar na tarefa F0-13)
- Windows 10/11 ou Linux (Ubuntu 22.04+).
- CMake 3.28+, vcpkg, compilador C++20 (MSVC 2022 ou Clang 17+).
- Godot 4.x (versão fixada no ADR da Fase 0).
- Python 3.11+ e Docker para a bancada `sw-netlab`.

## Fluxo de trabalho
1. Crie um branch a partir de `main`: `tipo/ID-descricao-curta` (ex.: `feat/F1-14-remocao-clock`).
2. Commits pequenos, mensagem no formato `F1-14: implementa marcas d'água da remoção`.
3. Abra o PR cedo (rascunho) e marque o dono do módulo (CODEOWNERS).
4. O PR precisa de: CI verde, testes para o que mudou, doc atualizado se o comportamento mudou.
5. Código marcado `critico` precisa de aprovação de um sênior ou do arquiteto.

## Regras de código
- Nenhuma falha silenciosa: todo erro é retornado ou registrado. **Confira todo valor de retorno de I/O.**
- Nada de alocação no caminho quente de leitura.
- Dado vindo da rede ou de peer é não confiável até ser verificado.
- Toda funcionalidade nova publica as métricas que a provam ([docs/08-metricas-e-testes.md](docs/08-metricas-e-testes.md)).

## Decisões
Mudança de arquitetura ou de contrato = **ADR novo** em [docs/adr/](docs/adr/README.md), revisado antes do código.

## Licença, DCO e cabeçalhos
O núcleo do Swarmstream é open source sob a **Apache-2.0** ([LICENSE](LICENSE), [ADR-0008](docs/adr/0008-licenca-open-source.md)). Ao contribuir, você envia seu código sob essa licença (seção 5 da Apache-2.0).

### DCO: assine cada commit
Usamos o **Developer Certificate of Origin 1.1** ([ADR-0015](docs/adr/0015-politica-de-contribuicao-dco.md)); não há CLA. Assinar um commit é declarar que você tem o direito de enviar aquele código sob a licença do projeto (texto completo em <https://developercertificate.org/>).

```bash
git commit -s -m "F1-14: implementa marcas d'água da remoção"
```

Isso acrescenta `Signed-off-by: Seu Nome <seu@email>` à mensagem, com o mesmo nome e e-mail do autor do commit. O CI recusa PR com commit sem essa linha. Esqueceu? Corrija o último commit com `git commit --amend -s --no-edit`, ou todos os commits do branch com `git rebase --signoff main`, e envie de novo com `git push --force-with-lease`.

### Cabeçalho de arquivo
Todo arquivo de código novo (C, C++, Go, Rust, Python, CMake, scripts) começa com:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
```

Use o comentário da linguagem (`#` em Python, CMake e shell). Não é preciso listar seu nome: a autoria fica no histórico do git.

### Código e material de terceiros
- Não copie código de terceiros sem registrar origem e licença no [NOTICE](NOTICE), no mesmo PR.
- Só entram licenças compatíveis com Apache-2.0 (MIT, BSD, BSL-1.0, Apache-2.0, zlib). GPL/AGPL/LGPL em código do núcleo: não. Na dúvida, pergunte ao arquiteto antes de escrever código.
- As skills em `.claude/skills/` mantêm as licenças de origem (MIT e CC BY-SA 4.0, ver [NOTICE](NOTICE)). Não copie trechos delas para o código ou para a documentação do projeto.

## Segurança
Falhas de segurança: **nunca** em issue pública. Reporte em privado, conforme o [SECURITY.md](SECURITY.md).
