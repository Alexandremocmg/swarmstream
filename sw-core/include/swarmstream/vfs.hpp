// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-17: VFS. Resolve caminho + offset + tamanho em TRECHOS DE BLOCOS e lista diretórios virtuais,
// a partir do `ManifestView` do `sw-format` (docs/03 §6, docs/06; semântica de EOF e erros do
// swarmstream.h). O VFS SÓ RESOLVE: não lê cache nem rede, não verifica bytes, não agenda e não é
// a API C (isso é do F1-60, que mapeia `VfsStatus` para `sw_result`).
//
//   Vfs::build()    índice sobre o manifesto (uma tabela de deslocamentos por referência de bloco).
//                   Recusa um manifesto em que um caminho é arquivo e também prefixo "caminho/" de
//                   outro (recusa do VFS, PENDENTE DE ADR: o §8 do docs/03 não exige isso hoje).
//                   Aloca; roda uma vez, em sw_init.
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
    // Um caminho é arquivo e também prefixo "caminho/" de outro arquivo. Recusa do VFS, pendente de
    // ADR (o §8 do docs/03 aceita esse manifesto); o mapeamento é do F1-60.
    FileDirectoryConflict,
    OutOfMemory,
};

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

// Cursor sobre os trechos de uma leitura. Trivialmente copiável; não aloca.
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

class VfsFile
{
public:
    VfsFile() = default;
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
    [[nodiscard]] static VfsBuildStatus build(const pack::ManifestView& view, Vfs& out) noexcept;

    [[nodiscard]] VfsStatus stat(std::string_view path, VfsStat& out) const noexcept;
    [[nodiscard]] VfsStatus open(std::string_view path, VfsFile& out) const noexcept;
    [[nodiscard]] VfsStatus open_dir(std::string_view path, VfsDir& out) const noexcept;

    [[nodiscard]] const pack::ManifestView& manifest() const noexcept { return view_; }

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
