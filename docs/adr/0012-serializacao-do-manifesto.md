# ADR-0012: Manifesto em FlatBuffers dentro de um envelope assinado

**Status:** Aceita (2026-10-17: revisão R1, leitura independente, aprovação do dono do produto) · **Data:** 2026-10-17 · **Autor:** R1 (líder técnico)

**Revisão (regra de 3 pessoas, [15](../15-portao-fase-0-e-plano-3-recursos.md#regra-de-revisão-com-3-pessoas)):** R1 revisou · leitura e comentários: revisor independente · aprovação: dono do produto (2026-10-17). Registro: [revisão F0](../revisoes/F0-revisao-adrs-0010-0016.md)

## Contexto
O manifesto lista todos os arquivos, blocos, peças e grupos de uma versão. Ele é gerado pelo `sw-pack` (C++), lido pelo `sw-core` (C++) e validado pelo serviço de manifestos (Go, [ADR-0010](0010-linguagem-dos-servicos-de-backend.md)). Pelo [ADR-0009](0009-abertura-de-backend-e-launcher.md), o formato é um **protocolo aberto e documentado**. A pergunta em aberto em [02](../02-arquitetura.md#contratos-entre-módulos) era: FlatBuffers ou CBOR.

A especificação v0 ([03](../03-formato-de-conteudo.md), F0-08) já adotou FlatBuffers e um envelope fixo como **padrão provisório**, à espera deste ADR ([03 §13](../03-formato-de-conteudo.md#13-decisões-provisórias-que-dependem-de-adr)). Este ADR confirma a escolha e fixa os princípios; o layout byte a byte fica na especificação.

### Ordem de grandeza
Pelo orçamento de [03 §7.3](../03-formato-de-conteudo.md#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo): ≈ 45 bytes por bloco, ≈ 1,2 milhão de blocos num jogo de 150 GB com bloco médio de 128 KB, ≈ **54 MB** de manifesto. Passa da meta de 50 MB. Os 32 bytes do hash de cada bloco (≈ 39 MB) dominam, e só são usados para calcular patches e verificar o bloco na primeira entrada no cache de RAM, não em cada leitura.

## Opções consideradas

| Critério | **FlatBuffers** | CBOR | Protobuf | Binário próprio |
| --- | --- | --- | --- | --- |
| Leitura sem cópia (arquivo mapeado) | **Sim** | Não: decodifica tudo para a RAM | Não | Sim |
| Abertura com 1,2 M blocos | Verificação linear, sem alocação | Decodificação + milhões de objetos alocados | Idem CBOR | Mínima |
| Tabelas colunares compactas | Sim (vetores de escalares e de `struct`) | Não (cabeçalho por item) | Não | Sim |
| Verificador de limites pronto | Sim (`Verifier`) | Depende da biblioteca | Sim | Escrever e fuzzar à mão (risco R6) |
| C++ e Go oficiais | Sim | Bibliotecas de terceiros | Sim | Escrever duas vezes |
| Esquema publicável para terceiros | `.fbs` | CDDL | `.proto` | Só prosa |
| Codificação canônica | Não | Sim (RFC 8949 §4.2) | Não | Sim |

A falta de codificação canônica do FlatBuffers não importa: a assinatura cobre os **bytes exatos do arquivo**, nunca uma reserialização.

## Decisão
1. **Payload em FlatBuffers**, esquema `manifest.fbs` definido em [03 §7.2](../03-formato-de-conteudo.md#72-payload-schema-flatbuffers-v0) (dono: sênior de engine). Versão do FlatBuffers fixada no manifesto do vcpkg (F0-02) e no `go.mod`.
2. **Envelope binário fixo antes do payload**, como em [03 §7.1](../03-formato-de-conteudo.md#71-envelope-cabeçalho-fixo--payload): `magic`, `format_version`, `hash_alg`, `sig_alg`, `key_id`, assinatura Ed25519 com prefixo de separação de domínio. A assinatura cobre os bytes exatos; não existe canonicalização. O `key_id` permite rotação de chave.
3. **Ordem obrigatória no leitor**, igual no C++ e no Go ([03 §7.1](../03-formato-de-conteudo.md#71-envelope-cabeçalho-fixo--payload) e [§8](../03-formato-de-conteudo.md#8-validações-do-leitor)): campos fixos do envelope → chave pelo `key_id` → **assinatura** → `Verifier` → validações semânticas. Nada do payload é usado antes da assinatura passar.
4. **Não existe modo "manifesto sem assinatura"**, nem para desenvolvimento. A F1 (F1-04) assina com uma **chave de teste** publicada em `sw-pack/testdata/`. O CI falha se um build de release do `sw-core`, do launcher ou do jogo de teste tiver essa chave entre as confiáveis.
5. **Evolução, em dois níveis:**
   - `format_version` (no envelope) versiona **só o envelope** e é conferido por igualdade: o leitor recusa valor que não conhece. Só muda se o layout do envelope mudar.
   - A compatibilidade do **payload** é dada por `min_reader` (no payload, lido depois da assinatura): o escritor põe ali a menor versão de leitor capaz de entender o conteúdo, e o leitor recusa com `SW_E_MANIFEST_UNSUPPORTED` se `min_reader` for maior que a sua. Campo novo no fim de tabela não muda `min_reader`; mudança de `struct`, remoção ou reordenação de campo sobem `min_reader` e exigem ADR.
   - Sem essa separação, um `format_version` novo seria recusado no passo 1 e o `min_reader` nunca seria consultado. A spec ([03 §7.1](../03-formato-de-conteudo.md#71-envelope-cabeçalho-fixo--payload), §7.2 e §8) precisa ser alinhada a esta regra pelo dono da F0-08.
6. **Orçamento de memória:** o `sw-core` mapeia o manifesto em memória e não copia as tabelas. A verificação Ed25519 e o `manifest_id` leem o payload inteiro uma vez, o que deixa todas as páginas residentes. Por isso, depois de verificar, o `sw-core` **devolve as páginas do mapeamento ao sistema** (`madvise(MADV_DONTNEED)` no Linux; desmapear e mapear de novo no Windows), e daí em diante só as colunas lidas voltam à RAM. A meta de 50 MB de [03](../03-formato-de-conteudo.md) passa a valer para a **memória residente depois de uma rota completa** no mapa de teste, que é o que o jogador sente; o tamanho do arquivo é medido e reportado junto. As saídas (a)–(c) de [03 §7.3](../03-formato-de-conteudo.md#73-orçamento-de-tamanho-meta--50-mb-para-150-gb-de-jogo) continuam valendo se a memória residente estourar.

## Consequências
- (+) O cliente abre manifestos de dezenas de MB sem decodificar nem alocar; a RAM residente fica com as colunas efetivamente lidas.
- (+) Verificador pronto e esquema `.fbs` publicável como especificação aberta (ADR-0009).
- (+) Assinar bytes, não estrutura, elimina toda uma classe de erro de canonicalização.
- (+) Sem modo sem assinatura, o caminho de verificação é exercitado desde a F1.
- (−) A F1 precisa de Ed25519 desde já: assinar no `sw-pack` (F1-04) e verificar no `sw_init` do `sw-core` (dependência a mais no vcpkg, por exemplo libsodium). O backlog e o roadmap deixam de falar em "manifesto sem assinatura".
- (−) Regras de evolução do FlatBuffers são rígidas; quem errar quebra clientes antigos. Mitigação: teste no CI que lê os manifestos de `sw-pack/testdata/` de versões anteriores.
- (−) O `Verifier` não substitui as validações de [03 §8](../03-formato-de-conteudo.md#8-validações-do-leitor), que são código nosso e `critico`: fuzzing com libFuzzer (C++) e `go test -fuzz` (Go) desde a F1.
- Verificação: F1-04 mede o tamanho do arquivo e a memória residente com o mapa da F0-09 e extrapola para 150 GB. Se a memória residente passar de 50 MB, escolher entre as saídas de 03 §7.3 por ADR, com o número medido. A escolha do FlatBuffers só é reaberta se a **abertura completa** (mapear, verificar a assinatura, calcular o `manifest_id`, rodar o `Verifier` e as validações) custar mais de 300 ms, medidos, extrapolados para o manifesto de 150 GB. A assinatura e o hash entram nessa conta, porque são o custo dominante e não dependem do formato.
