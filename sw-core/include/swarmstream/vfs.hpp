// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-17: VFS. Resolve caminho + offset + tamanho em TRECHOS DE BLOCOS e lista diretórios virtuais,
// a partir do `ManifestView` do `sw-format` (docs/03 §6, docs/06; semântica de EOF e erros do
// swarmstream.h). O VFS SÓ RESOLVE: não lê cache nem rede, não verifica bytes, não agenda e não é
// a API C (isso é do F1-60, que mapeia `VfsStatus` para `sw_result`).
//
//   Vfs::build()    índice sobre o manifesto (uma tabela de deslocamentos por referência de bloco).
//                   Recusa um manifesto em que um caminho é arquivo e também prefixo "caminho/" de
//                   outro (recusa do VFS, decidida no ADR-0017, aceito:
//                   docs/adr/0017-semantica-do-vfs.md; o §8 do docs/03 aceita esse manifesto).
//                   Aloca; roda uma vez, em sw_init. GARANTIA FORTE, ver "Falha de build" abaixo.
//   Vfs::stat()     arquivo, diretório ou nada. Sem alocação.
//   Vfs::open()     arquivo -> VfsFile. Sem alocação.
//   VfsFile::resolve(offset, size) -> VfsSpans, um cursor sobre os trechos (bloco, deslocamento no
//                   bloco, comprimento). CAMINHO QUENTE: sem alocação, O(log blocos do arquivo)
//                   para achar o primeiro trecho e O(1) por trecho seguinte.
//   Vfs::open_dir() -> VfsDir. Aloca (uma lista de filhos, ordenada); não é caminho quente.
//
// Caminhos: comparação EXATA em bytes com o caminho do manifesto (já NFC, sem "/" inicial ou final,
// docs/03 §6). Nenhuma normalização: "/a", "a/", "a//b", "./a" simplesmente não existem. O caminho
// vazio é a raiz (um diretório). O VFS recebe `string_view`: argumento inválido (ponteiro nulo
// etc.) é da API C (F1-60); aqui não há semântica de "nulo".
//
// Tempo de vida: o `Vfs` guarda uma cópia do `ManifestView`, que aponta para os bytes do manifesto;
// esses bytes têm de viver (e não mudar) mais que o `Vfs` e que todo `VfsFile`, `VfsSpans` e
// `VfsDir` dele. Depois de `build`, tudo é imutável: pode ser chamado por várias threads.
//
// Falha de build (garantia forte, F1-63): `build` monta o índice num objeto TEMPORÁRIO e só o
// publica em `out` depois de sucesso completo (a publicação é um move, que não lança). Em QUALQUER
// falha (FileDirectoryConflict, OutOfMemory) `out` fica EXATAMENTE como estava: vazio continua
// vazio, e um Vfs já construído continua válido e responde igual (stat/open/resolve/next, e os
// handles abertos nele antes continuam valendo). `view` pode ser o próprio `out.manifest()`.
//
// Handles (VfsFile, VfsSpans, VfsDir) e o que pode acontecer com eles:
//   Precondição (F1-65): um `VfsFile` ou `VfsSpans` só é usado depois de o `Vfs::open` ou o
//   `VfsFile::resolve` que o preencheu devolver Ok. Um `VfsFile{}` default, ou o que o `open`
//   deixou vazio por erro (o destino é zerado), é inválido: `size()`, `path()`, `file_class()`,
//   `group()` e `resolve()` nele são comportamento indefinido (desreferenciam ponteiro nulo). O
//   mesmo vale para um `VfsSpans` que nunca foi preenchido por um `resolve` Ok: `next()` nele não é
//   contratado. NÃO há verificação no caminho quente, de propósito. `VfsFile::valid()` é para quem
//   recebe o handle por fora (a API C, F1-60): ela valida o handle ANTES de repassá-lo ao VFS, e
//   essa prova é critério de aceite do F1-60, não daqui.
//   Um handle é do OBJETO `Vfs` em que foi aberto (guarda o endereço dele), não do manifesto.
//   VfsFile e VfsSpans ficam INVÁLIDOS (usá-los é comportamento indefinido) quando o `Vfs` é:
//     - reconstruído com sucesso por cima (`build(..., out)` que devolve Ok troca o conteúdo de
//       `out`: os índices e o `file_` antigos não valem mais);
//     - atribuído por cima (cópia ou move), movido (o endereço muda) ou destruído.
//   Estado de um `Vfs` DE QUE SE MOVEU (construtor ou atribuição de move; os membros de move são os
//   gerados pelo compilador, NÃO há move próprio): a visão (`ManifestView`) continua como era, pois
//   é copiada, e a tabela de índices fica vazia (garantido no construtor de move; na atribuição o
//   padrão só diz "válido, mas não especificado" e as implementações a esvaziam). Visão cheia com
//   tabela vazia é um estado INCOERENTE. Operações permitidas nele: destruir; atribuir por cima
//   (cópia ou move); usá-lo como `out` de um `build` novo; `manifest()` e `index_size()` (só leem).
//   Qualquer outra (`stat`, `open`, `open_dir`, `resolve` de handle antigo) não é contratada, e
//   `resolve` lê fora dos limites. Quem integra (F1-60) não pode reutilizar um Vfs movido-de. Um
//   move próprio (que o deixaria vazio e seguro) NÃO é implementado aqui: seria cartão à parte.
//   Uma reconstrução que FALHA não invalida nada (garantia acima). `valid()` não detecta nenhum
//   desses casos: só diz que o handle foi preenchido por um open. Descarte os handles e reabra
//   depois de reconstruir. Construir uma CÓPIA do Vfs não afeta os handles do original, que também
//   não migram para a cópia (a cópia tem os seus).
//   VfsDir NÃO depende do `Vfs` depois de aberto (guarda cópias das entradas, cujos nomes apontam
//   para os bytes do manifesto): continua válido numa reconstrução, num move ou depois de o Vfs ser
//   destruído, enquanto os bytes do manifesto ANTIGO viverem, e mostra o diretório como era. Um
//   `VfsDir{}` default, ou o que o `open_dir` deixou vazio por erro, é uma listagem vazia
//   (definido).
#pragma once

#include "swarmstream/manifest_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace swarmstream {

enum class VfsKind : std::uint8_t { None, File, Directory };

// Resultado das consultas. O mapeamento para sw_result é do F1-60 (ex.: NotFound -> SW_E_NOT_FOUND,
// OutOfRange -> SW_E_OUT_OF_RANGE); `IsDirectory` e `NotDirectory` ficam separados de `NotFound`
// justamente para essa escolha não travar o VFS.
enum class VfsStatus : std::uint8_t {
    Ok,
    NotFound,     // nem arquivo nem diretório
    IsDirectory,  // pediu um arquivo e o caminho é um diretório (inclui a raiz "")
    NotDirectory, // pediu um diretório e o caminho é um arquivo
    OutOfRange, // offset > tamanho, ou offset < tamanho e size > tamanho - offset (nada é entregue)
    OutOfMemory, // só open_dir (aloca a lista de filhos)
};

enum class VfsBuildStatus : std::uint8_t {
    Ok,
    // Um caminho é arquivo e também prefixo "caminho/" de outro arquivo. Recusa do VFS, decidida no
    // ADR-0017 (o §8 do docs/03 aceita esse manifesto); o mapeamento é do F1-60.
    FileDirectoryConflict,
    // Falta de memória ao montar o índice (mapeia para SW_E_OUT_OF_MEMORY no F1-60, ADR-0017 §4).
    OutOfMemory,
};
// Nos dois erros, `out` fica como estava (garantia forte de `Vfs::build`).

struct VfsStat
{
    VfsKind kind = VfsKind::None;
    std::uint64_t size = 0;                             // bytes descomprimidos; 0 para diretório
    pack::FileClass file_class = pack::FileClass::Core; // só para arquivo
    std::uint16_t group = 0;                            // só para arquivo
};

// Um trecho contíguo dentro de UM bloco do manifesto.
struct BlockSpan
{
    std::uint32_t block = 0;  // índice na tabela de blocos do manifesto
    std::uint32_t offset = 0; // deslocamento dentro dos bytes DESCOMPRIMIDOS do bloco
    std::uint32_t length = 0; // > 0
    friend bool operator==(const BlockSpan&, const BlockSpan&) = default;
};

class Vfs;

// Cursor sobre os trechos de uma leitura. Trivialmente copiável; não aloca. Só se usa depois de um
// `VfsFile::resolve` Ok, e só enquanto o `Vfs` dele não for reconstruído, movido nem destruído
// (precondições e invalidação: ver "Handles" no topo do arquivo).
class VfsSpans
{
public:
    // Quantos bytes a leitura entrega: igual ao `size` pedido, ou 0 quando offset == tamanho.
    // Constante durante a iteração.
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_; }
    // Próximo trecho; false quando acabou.
    [[nodiscard]] bool next(BlockSpan& out) noexcept;

private:
    friend class Vfs;
    friend class VfsFile;
    const Vfs* vfs_ = nullptr;
    std::uint64_t total_ = 0;
    std::uint64_t pos_ = 0;  // posição no arquivo do próximo byte
    std::uint64_t left_ = 0; // bytes que ainda faltam entregar
    std::size_t ref_ = 0;    // índice (global) da referência de bloco que contém pos_
};

// Arquivo aberto. PRECONDIÇÃO: só se usa depois de `Vfs::open` Ok e enquanto o `Vfs` dele não for
// reconstruído, movido nem destruído; `VfsFile{}` (ou o que um open com erro deixou) é inválido e
// usá-lo é comportamento indefinido (ver "Handles" no topo do arquivo). Sem verificação no caminho
// quente.
class VfsFile
{
public:
    VfsFile() = default;
    // true se um `open` Ok preencheu este handle. NÃO prova que o Vfs ainda é o mesmo
    // (reconstruído, movido ou destruído depois do open).
    [[nodiscard]] bool valid() const noexcept { return vfs_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] pack::FileClass file_class() const noexcept;
    [[nodiscard]] std::uint16_t group() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;

    // Resolve [offset, offset + size) em trechos. Tudo ou nada:
    //   offset == tamanho  -> Ok, 0 bytes (para qualquer size);
    //   offset > tamanho   -> OutOfRange (mesmo com size 0);
    //   size 0 e offset <= tamanho -> Ok, 0 trechos;
    //   offset < tamanho e size > tamanho - offset -> OutOfRange: nenhum trecho parcial, o chamador
    //   (F1-60) não trunca em silêncio. size == tamanho - offset é Ok.
    // O intervalo é validado SEM somar offset + size (não estoura uint64). O cursor `out` só é
    // preenchido quando o resultado é Ok; em erro fica intocado.
    [[nodiscard]] VfsStatus resolve(std::uint64_t offset, std::uint64_t size,
                                    VfsSpans& out) const noexcept;

private:
    friend class Vfs;
    const Vfs* vfs_ = nullptr;
    std::size_t file_ = 0;
};

struct VfsDirEntry
{
    std::string_view name; // só o nome; aponta para os bytes do manifesto (vale enquanto ele viver)
    VfsKind kind = VfsKind::None;
    std::uint64_t size = 0; // 0 para diretório
    pack::FileClass file_class = pack::FileClass::Core;
    std::uint16_t group = 0;
};

// Filhos diretos de um diretório, ordenados por nome em bytes UTF-8 (arquivos e subdiretórios
// misturados: "sub" vem antes de "sub.bin", embora no manifesto "sub.bin" preceda "sub/x").
// Independente do `Vfs` depois de aberto: sobrevive a reconstrução, move e destruição dele enquanto
// os bytes do manifesto vivem; `VfsDir{}` é uma listagem vazia (ver "Handles" no topo do arquivo).
class VfsDir
{
public:
    // true com uma entrada; false quando acabou (SW_DONE).
    [[nodiscard]] bool next(VfsDirEntry& out) noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }

private:
    friend class Vfs;
    std::vector<VfsDirEntry> entries_;
    std::size_t next_ = 0;
};

class Vfs
{
public:
    // O `view` precisa ter vindo de `ManifestView::open` com sucesso (todas as validações do §8
    // já foram feitas) e os bytes dele têm de sobreviver ao Vfs.
    // Garantia forte: devolve Ok e então `out` passa a ser o índice de `view` (sobrescrevendo o que
    // tinha, e invalidando VfsFile/VfsSpans abertos em `out`, ver o topo do arquivo); em qualquer
    // outro resultado `out` é exatamente o que era antes da chamada. `view` pode ser
    // `out.manifest()`.
    [[nodiscard]] static VfsBuildStatus build(const pack::ManifestView& view, Vfs& out) noexcept;

    [[nodiscard]] VfsStatus stat(std::string_view path, VfsStat& out) const noexcept;
    [[nodiscard]] VfsStatus open(std::string_view path, VfsFile& out) const noexcept;
    [[nodiscard]] VfsStatus open_dir(std::string_view path, VfsDir& out) const noexcept;

    [[nodiscard]] const pack::ManifestView& manifest() const noexcept { return view_; }
    // Entradas da tabela de índices (uma por referência de bloco). INVARIANTE: igual a
    // `manifest().block_ref_count()` (0 num Vfs vazio), exceto num Vfs de que se moveu.
    // Por que existe (F1-63): a garantia de `build` é "visão e tabela nunca ficam incoerentes", e
    // a API existente não a deixa observar: `manifest()` mostra só a visão, e olhar a tabela por
    // `resolve()` é justamente a leitura fora dos limites que se quer detectar (um `clear()` não
    // libera a memória, então a leitura devolveria dados velhos e o defeito passaria). Superfície
    // adicional: um getter `const noexcept` inline sobre `size()`, sem estado, sem alocação, que
    // `stat/open/resolve/next` não chamam (não é caminho quente). Interno ao sw-core; não faz
    // parte do `swarmstream.h`.
    [[nodiscard]] std::size_t index_size() const noexcept { return ref_start_.size(); }

private:
    friend class VfsSpans;
    friend class VfsFile;

    // Índice do arquivo com este caminho exato, ou file_count().
    [[nodiscard]] std::size_t find_file(std::string_view path) const noexcept;
    // Faixa [lo, hi) de arquivos sob "dir/". Vazia se `dir` não existe como diretório.
    // "" é a raiz: todos os arquivos.
    void dir_range(std::string_view dir, std::size_t& lo, std::size_t& hi) const noexcept;

    pack::ManifestView view_;
    // ref_start_[r] = deslocamento, dentro do arquivo, do primeiro byte da r-ésima referência de
    // bloco (r global em block_refs). Cada arquivo ocupa uma faixa contígua [first_ref, +count).
    std::vector<std::uint64_t> ref_start_;
};

} // namespace swarmstream
