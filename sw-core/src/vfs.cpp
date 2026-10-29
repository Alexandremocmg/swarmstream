// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/vfs.hpp"

#include <algorithm>
#include <new>

namespace swarmstream {
namespace {

// Primeiro índice em [lo, hi) para o qual `pred` é verdadeiro; `pred` é monótono
// (falso...verdadeiro).
template <typename Pred>
std::size_t first_true(std::size_t lo, std::size_t hi, Pred pred) noexcept
{
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (pred(mid)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

// `path` começa com "dir/"? Sem montar a string.
bool under_dir(std::string_view path, std::string_view dir) noexcept
{
    return path.size() > dir.size() && path[dir.size()] == '/' && path.starts_with(dir);
}

// path < dir + "/" ? Sem montar a string. Ordem em bytes (sem sinal), a mesma do manifesto.
bool before_dir(std::string_view path, std::string_view dir) noexcept
{
    const std::size_t n = std::min(path.size(), dir.size());
    const int c = path.substr(0, n).compare(dir.substr(0, n));
    if (c != 0) {
        return c < 0;
    }
    if (path.size() <= dir.size()) {
        return true; // `path` é prefixo de `dir` (ou igual): menor que "dir/"
    }
    return static_cast<unsigned char>(path[dir.size()]) < static_cast<unsigned char>('/');
}

} // namespace

// ---------------------------------------------------------------- Vfs::build

VfsBuildStatus Vfs::build(const pack::ManifestView& view, Vfs& out) noexcept
{
    try {
        out.view_ = view;
        out.ref_start_.assign(view.block_ref_count(), 0);
        for (std::size_t f = 0; f < view.file_count(); ++f) {
            const std::size_t first = view.file_first_ref(f);
            const std::size_t count = view.file_ref_count(f);
            std::uint64_t running = 0;
            for (std::size_t r = first; r < first + count; ++r) {
                out.ref_start_[r] = running;
                running += view.block_raw_size(view.block_ref(r));
            }
        }

        // Conflito arquivo/diretório: recusa do VFS, pendente de ADR (o §8 do docs/03 aceita "a" e
        // "a/b" juntos). Os caminhos sob "p/" são
        // contíguos logo depois de "p" e dos que começam com "p" seguido de byte < '/'. Uma pilha
        // com os arquivos que ainda são prefixo do caminho atual acha o conflito em uma passada.
        std::vector<std::string_view> prefixes;
        for (std::size_t i = 0; i < view.file_count(); ++i) {
            const std::string_view path = view.file_path(i);
            while (!prefixes.empty() && !path.starts_with(prefixes.back())) {
                prefixes.pop_back();
            }
            for (const std::string_view prefix : prefixes) {
                if (path[prefix.size()] == '/') {
                    out.ref_start_.clear();
                    return VfsBuildStatus::FileDirectoryConflict;
                }
            }
            prefixes.push_back(path);
        }
    } catch (const std::bad_alloc&) {
        return VfsBuildStatus::OutOfMemory;
    }
    return VfsBuildStatus::Ok;
}

// ---------------------------------------------------------------- busca

std::size_t Vfs::find_file(std::string_view path) const noexcept
{
    const std::size_t n = view_.file_count();
    const std::size_t i =
        first_true(0, n, [&](std::size_t k) { return !(view_.file_path(k) < path); });
    return (i < n && view_.file_path(i) == path) ? i : n;
}

void Vfs::dir_range(std::string_view dir, std::size_t& lo, std::size_t& hi) const noexcept
{
    const std::size_t n = view_.file_count();
    if (dir.empty()) {
        lo = 0;
        hi = n;
        return;
    }
    lo = first_true(0, n, [&](std::size_t k) { return !before_dir(view_.file_path(k), dir); });
    hi = first_true(lo, n, [&](std::size_t k) { return !under_dir(view_.file_path(k), dir); });
}

VfsStatus Vfs::stat(std::string_view path, VfsStat& out) const noexcept
{
    out = VfsStat{};
    if (path.empty()) {
        out.kind = VfsKind::Directory; // a raiz sempre existe, mesmo com zero arquivos
        return VfsStatus::Ok;
    }
    const std::size_t f = find_file(path);
    if (f < view_.file_count()) {
        out.kind = VfsKind::File;
        out.size = view_.file_size(f);
        out.file_class = view_.file_class(f);
        out.group = view_.file_group(f);
        return VfsStatus::Ok;
    }
    std::size_t lo = 0;
    std::size_t hi = 0;
    dir_range(path, lo, hi);
    if (lo < hi) {
        out.kind = VfsKind::Directory;
        return VfsStatus::Ok;
    }
    return VfsStatus::NotFound;
}

VfsStatus Vfs::open(std::string_view path, VfsFile& out) const noexcept
{
    out = VfsFile{};
    VfsStat info;
    const VfsStatus s = stat(path, info);
    if (s != VfsStatus::Ok) {
        return s;
    }
    if (info.kind == VfsKind::Directory) {
        return VfsStatus::IsDirectory;
    }
    out.vfs_ = this;
    out.file_ = find_file(path);
    return VfsStatus::Ok;
}

VfsStatus Vfs::open_dir(std::string_view path, VfsDir& out) const noexcept
{
    out.entries_.clear();
    out.next_ = 0;
    std::size_t lo = 0;
    std::size_t hi = 0;
    dir_range(path, lo, hi);
    if (!path.empty() && lo >= hi) {
        return find_file(path) < view_.file_count() ? VfsStatus::NotDirectory : VfsStatus::NotFound;
    }
    const std::size_t prefix_len = path.empty() ? 0 : path.size() + 1;
    try {
        std::size_t i = lo;
        while (i < hi) {
            const std::string_view full = view_.file_path(i);
            const std::string_view rest = full.substr(prefix_len);
            const std::size_t slash = rest.find('/');
            VfsDirEntry entry;
            if (slash == std::string_view::npos) {
                entry.name = rest;
                entry.kind = VfsKind::File;
                entry.size = view_.file_size(i);
                entry.file_class = view_.file_class(i);
                entry.group = view_.file_group(i);
                ++i;
            } else {
                entry.name = rest.substr(0, slash);
                entry.kind = VfsKind::Directory;
                // Os arquivos de "dir/sub/" são contíguos a partir de i: pula todos de uma vez.
                const std::string_view sub = full.substr(0, prefix_len + slash);
                i = first_true(i + 1, hi,
                               [&](std::size_t k) { return !under_dir(view_.file_path(k), sub); });
            }
            out.entries_.push_back(entry);
        }
    } catch (const std::bad_alloc&) {
        out.entries_.clear();
        return VfsStatus::OutOfMemory;
    }
    std::sort(out.entries_.begin(), out.entries_.end(),
              [](const VfsDirEntry& a, const VfsDirEntry& b) { return a.name < b.name; });
    return VfsStatus::Ok;
}

// ---------------------------------------------------------------- VfsDir

bool VfsDir::next(VfsDirEntry& out) noexcept
{
    if (next_ >= entries_.size()) {
        return false;
    }
    out = entries_[next_++];
    return true;
}

// ---------------------------------------------------------------- VfsFile e VfsSpans

std::uint64_t VfsFile::size() const noexcept { return vfs_->view_.file_size(file_); }
pack::FileClass VfsFile::file_class() const noexcept { return vfs_->view_.file_class(file_); }
std::uint16_t VfsFile::group() const noexcept { return vfs_->view_.file_group(file_); }
std::string_view VfsFile::path() const noexcept { return vfs_->view_.file_path(file_); }

VfsStatus VfsFile::resolve(std::uint64_t offset, std::uint64_t size, VfsSpans& out) const noexcept
{
    const std::uint64_t file_size = this->size();
    if (offset > file_size) {
        return VfsStatus::OutOfRange;
    }
    // Tudo ou nada, sem somar offset + size: em offset == tamanho, 0 bytes para qualquer size;
    // antes do fim, size maior que o resto do arquivo é erro (nenhum trecho parcial).
    const std::uint64_t remaining = file_size - offset;
    if (remaining != 0 && size > remaining) {
        return VfsStatus::OutOfRange;
    }
    const std::uint64_t length = remaining == 0 ? 0 : size;
    out = VfsSpans{};
    out.vfs_ = vfs_;
    out.total_ = length;
    out.pos_ = offset;
    out.left_ = length;
    if (length > 0) {
        // length > 0 => offset < file_size => o arquivo tem pelo menos uma referência, e a primeira
        // começa em 0 <= offset: o último ref_start_ <= offset existe.
        const std::size_t first = vfs_->view_.file_first_ref(file_);
        const std::size_t count = vfs_->view_.file_ref_count(file_);
        const auto begin = vfs_->ref_start_.begin() + static_cast<std::ptrdiff_t>(first);
        const auto it = std::upper_bound(begin, begin + static_cast<std::ptrdiff_t>(count), offset);
        out.ref_ = first + static_cast<std::size_t>(it - begin) - 1;
    }
    return VfsStatus::Ok;
}

bool VfsSpans::next(BlockSpan& out) noexcept
{
    if (left_ == 0) {
        return false;
    }
    const pack::ManifestView& view = vfs_->view_;
    const std::uint32_t block = view.block_ref(ref_);
    const std::uint64_t raw = view.block_raw_size(block);
    const std::uint64_t in_block = pos_ - vfs_->ref_start_[ref_]; // < raw
    const std::uint64_t take = std::min(left_, raw - in_block);
    out.block = block;
    out.offset = static_cast<std::uint32_t>(in_block);
    out.length = static_cast<std::uint32_t>(take);
    pos_ += take;
    left_ -= take;
    if (in_block + take == raw) {
        ++ref_;
    }
    return true;
}

} // namespace swarmstream
