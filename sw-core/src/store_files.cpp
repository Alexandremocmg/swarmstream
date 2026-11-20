// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/store_files.hpp"

#include "cache_journal.hpp" // append_store_file_leaf (nome nativo do store-NNN.bin)
#include "platform_file.hpp"
#include "store_files_testing.hpp"

#if !defined(_WIN32)
#include <dirent.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace swarmstream {

namespace {

constexpr uint32_t kAlignment = 4096; // docs/04 §1: slots alinhados a 4 KiB

std::filesystem::path file_name(const std::filesystem::path& dir, uint32_t index)
{
    char name[32];
    std::snprintf(name, sizeof name, "store-%03u.bin", static_cast<unsigned>(index));
    return dir / name;
}

using detail::File;
using detail::sync_directory;

Unexpected<CacheErrorCode> fail(CacheErrorCode code) { return Unexpected(code); }

// F1-72: o NNN se `name` (um nome de arquivo, sem pasta, com `len` caracteres na largura NATIVA) é
// um nome canônico "store-NNN.bin" (o de file_name, "%03u"). Não converte nem aloca (Regra R).
// Aceita exatamente os nomes que o sscanf("store-%u.bin%c") seguido da comparação com o nome
// canônico aceitava antes: 3 dígitos quaisquer, ou mais de 3 sem zero à esquerda, até 4294967295, e
// nada antes de "store-" nem depois de ".bin" (maiúsculas contam).
bool canonical_store_index(const std::filesystem::path::value_type* name, size_t len,
                           uint32_t& out) noexcept
{
    using Char = std::filesystem::path::value_type;
    static constexpr char kPrefix[] = "store-";
    static constexpr char kSuffix[] = ".bin";
    constexpr size_t prefix_len = sizeof kPrefix - 1;
    constexpr size_t suffix_len = sizeof kSuffix - 1;
    if (len < prefix_len + 3 + suffix_len) return false;
    for (size_t i = 0; i < prefix_len; ++i)
        if (name[i] != static_cast<Char>(kPrefix[i])) return false;
    for (size_t i = 0; i < suffix_len; ++i)
        if (name[len - suffix_len + i] != static_cast<Char>(kSuffix[i])) return false;
    const size_t digits = len - prefix_len - suffix_len;
    if (digits > 3 && name[prefix_len] == static_cast<Char>('0')) return false; // não é "%03u"
    uint64_t value = 0;
    for (size_t i = 0; i < digits; ++i) {
        const Char c = name[prefix_len + i];
        if (c < static_cast<Char>('0') || c > static_cast<Char>('9')) return false;
        value = value * 10 + static_cast<uint64_t>(c - static_cast<Char>('0'));
        if (value > 0xFFFFFFFFu) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

// F1-72: guarda em `out` o NNN de cada "store-NNN.bin" canônico de `dir` com NNN >= `count`.
// NÃO usa std::filesystem::directory_iterator: na libstdc++ o avanço do iterador (_Dir::advance,
// dentro da biblioteca) é noexcept e aloca o caminho de cada entrada, então uma falta de memória
// ali é std::terminate antes de qualquer catch (pilha medida na varredura do F1-72,
// libstdc++ 16.2). A API do sistema escreve o nome num buffer dela (WIN32_FIND_DATAW / dirent) e
// aqui só alocam o padrão de busca (CÓPIA da pasta, no Windows) e o crescimento de `out` (vector de
// inteiros, fora de qualquer noexcept): os dois lançam bad_alloc comum, e o handle da busca é
// fechado pelo guarda. Falta de memória do próprio sistema (ENOMEM, ERROR_NOT_ENOUGH_MEMORY) =>
// OutOfMemory; o resto, IoError. Toda chamada ao sistema tem o retorno conferido, inclusive o
// fechamento.
CacheErrorCode list_store_files_beyond(const std::filesystem::path& dir, uint32_t count,
                                       std::vector<uint32_t>& out)
{
#if defined(_WIN32)
    std::filesystem::path pattern(dir);
    detail::append_path_leaf(pattern, L"*");
    WIN32_FIND_DATAW found;
    struct Search
    {
        HANDLE h;
        ~Search()
        {
            if (h != INVALID_HANDLE_VALUE) (void)::FindClose(h); // só numa exceção (bad_alloc)
        }
    } search{::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                nullptr, 0)};
    if (search.h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return CacheErrorCode::Success; // nada na pasta
        return err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY
                   ? CacheErrorCode::OutOfMemory
                   : CacheErrorCode::IoError;
    }
    CacheErrorCode rc = CacheErrorCode::Success;
    for (;;) {
        uint32_t n = 0;
        if (canonical_store_index(found.cFileName, std::wcslen(found.cFileName), n) && n >= count)
            out.push_back(n);
        if (!::FindNextFileW(search.h, &found)) {
            if (::GetLastError() != ERROR_NO_MORE_FILES) rc = CacheErrorCode::IoError;
            break;
        }
    }
    const BOOL closed = ::FindClose(search.h);
    search.h = INVALID_HANDLE_VALUE;
    if (!closed && rc == CacheErrorCode::Success) rc = CacheErrorCode::IoError;
    return rc;
#else
    struct Search
    {
        DIR* d;
        ~Search()
        {
            if (d != nullptr) (void)::closedir(d); // só numa exceção (bad_alloc)
        }
    } search{::opendir(dir.c_str())};
    if (search.d == nullptr)
        return errno == ENOMEM ? CacheErrorCode::OutOfMemory : CacheErrorCode::IoError;
    CacheErrorCode rc = CacheErrorCode::Success;
    for (;;) {
        errno = 0;
        const dirent* e = ::readdir(search.d);
        if (e == nullptr) {
            if (errno != 0) rc = CacheErrorCode::IoError;
            break;
        }
        uint32_t n = 0;
        if (canonical_store_index(e->d_name, std::strlen(e->d_name), n) && n >= count)
            out.push_back(n);
    }
    const int closed = ::closedir(search.d);
    search.d = nullptr;
    if (closed != 0 && rc == CacheErrorCode::Success) rc = CacheErrorCode::IoError;
    return rc;
#endif
}

} // namespace

struct StoreFiles::Impl
{
    // F1-72, Regra R: roda no start() da CacheOwner. A pasta é COPIADA e o vetor nasce com `(n)`:
    // nenhum construtor padrão de fs::path/std::vector, que no MSVC em Debug é noexcept e aloca o
    // proxy dos iteradores (bad_alloc ali seria std::terminate antes de qualquer catch).
    Impl(const std::filesystem::path& d, uint32_t total, StoreLayout l, std::size_t file_count)
        : dir(d), total_slots(total), layout(l), files(file_count)
    {
    }
    static_assert(!std::is_nothrow_constructible_v<std::vector<File>, std::size_t>);

    std::filesystem::path dir;
    uint32_t total_slots = 0;
    StoreLayout layout;
    std::vector<File> files;
    uint32_t created = 0;
    bool closed = false;
    bool broken = false;           // um sync falhou: inutilizável até reabrir (ver o header)
    bool fail_next_sync = false;   // StoreFilesTestHook
    bool skip_space_check = false; // StoreFilesTestHook
    CacheErrorCode fail_write = CacheErrorCode::Success; // StoreFilesTestHook
    bool native_preallocate = true;                      // StoreFilesTestHook
    uint32_t directory_syncs = 0;
    uint64_t file_syncs = 0; // syncs de arquivo que deram certo (StoreFilesTestHook)

    [[nodiscard]] CacheErrorCode usable() const noexcept
    {
        return closed || broken ? CacheErrorCode::IoError : CacheErrorCode::Success;
    }

    // Todo sync passa por aqui: a primeira falha marca o store como quebrado.
    CacheErrorCode sync(File& f) noexcept
    {
        CacheErrorCode rc = CacheErrorCode::IoError;
        if (fail_next_sync) {
            fail_next_sync = false;
        } else {
            rc = f.sync();
        }
        if (rc != CacheErrorCode::Success) {
            broken = true;
        } else {
            ++file_syncs;
        }
        return rc;
    }

    [[nodiscard]] uint64_t file_bytes(uint32_t index) const noexcept
    {
        const uint32_t first = index * layout.slots_per_file;
        const uint32_t slots = std::min(layout.slots_per_file, total_slots - first);
        return uint64_t{slots} * layout.slot_size;
    }

    CacheErrorCode open_file(uint32_t index)
    {
        const std::filesystem::path path = file_name(dir, index);
        File f;
        if (const auto rc = f.open(path); rc != CacheErrorCode::Success) return rc;
        uint64_t current = 0;
        if (const auto rc = f.size(current); rc != CacheErrorCode::Success) return rc;
        const uint64_t wanted = file_bytes(index);
        if (current > wanted) return CacheErrorCode::IoError; // outro layout: não truncar dados
        if (current < wanted) {
            // Confere o livre antes de pedir: barra o disco cheio sem reservar nada, inclusive
            // onde a pré-alocação seria emulada escrevendo zeros. A pré-alocação continua
            // podendo dar DiskFull (outro processo gastou o espaço no meio-tempo).
            std::error_code ec;
            const auto space = std::filesystem::space(dir, ec);
            CacheErrorCode rc = (!skip_space_check && !ec && space.available < wanted - current)
                                    ? CacheErrorCode::DiskFull
                                    : f.preallocate(wanted, native_preallocate);
            if (rc != CacheErrorCode::Success) {
                // Devolve ao disco o que tiver sido reservado e não deixa arquivo vazio para trás.
                if (f.truncate(current) != CacheErrorCode::Success) broken = true;
                if (f.close() != CacheErrorCode::Success) broken = true;
                if (current == 0) std::filesystem::remove(path, ec);
                return rc;
            }
            if (rc = f.size(current); rc != CacheErrorCode::Success) return rc;
            if (current != wanted) return CacheErrorCode::IoError;
            // Tamanho e reserva no disco, e depois a entrada na pasta: a F1-11 grava no journal
            // peças deste arquivo e o arquivo precisa sobreviver a uma queda de energia.
            if (rc = sync(f); rc != CacheErrorCode::Success) return rc;
            if (rc = sync_directory(dir); rc != CacheErrorCode::Success) {
                broken = true;
                return rc;
            }
            ++directory_syncs;
        }
        files[index] = std::move(f);
        ++created;
        return CacheErrorCode::Success;
    }
};

StoreFiles::StoreFiles(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StoreFiles::StoreFiles(StoreFiles&&) noexcept = default;
StoreFiles& StoreFiles::operator=(StoreFiles&&) noexcept = default;
StoreFiles::~StoreFiles() = default; // fecha sem relatar erro; quem quer o erro chama close()

Result<StoreFiles, CacheErrorCode> StoreFiles::open(const std::filesystem::path& dir,
                                                    uint32_t total_slots, StoreLayout layout)
{
    if (layout.slots_per_file == 0 || layout.slot_size == 0 || layout.slot_size % kAlignment != 0) {
        return fail(CacheErrorCode::InvalidSize);
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return fail(CacheErrorCode::IoError);
    }
    auto impl = std::make_unique<Impl>(
        dir, total_slots, layout,
        static_cast<std::size_t>((uint64_t{total_slots} + layout.slots_per_file - 1) /
                                 layout.slots_per_file));
    return StoreFiles(std::move(impl));
}

Result<void, CacheErrorCode> StoreFiles::ensure_slot(SlotId slot)
{
    if (const auto rc = impl_->usable(); rc != CacheErrorCode::Success) return fail(rc);
    if (slot >= impl_->total_slots) return fail(CacheErrorCode::SlotNotFound);
    const uint32_t index = slot / impl_->layout.slots_per_file;
    if (impl_->files[index].is_open()) return {};
    if (const auto rc = impl_->open_file(index); rc != CacheErrorCode::Success) return fail(rc);
    return {};
}

Result<void, CacheErrorCode> StoreFiles::write(SlotId slot, size_t offset,
                                               std::span<const std::byte> data)
{
    if (const auto rc = impl_->usable(); rc != CacheErrorCode::Success) return fail(rc);
    if (slot >= impl_->total_slots) return fail(CacheErrorCode::SlotNotFound);
    if (offset > impl_->layout.slot_size || data.size() > impl_->layout.slot_size - offset) {
        return fail(CacheErrorCode::InvalidOffset);
    }
    if (auto r = ensure_slot(slot); !r) return r;
    const uint32_t index = slot / impl_->layout.slots_per_file;
    const uint64_t base = uint64_t{slot % impl_->layout.slots_per_file} * impl_->layout.slot_size;
    if (impl_->fail_write != CacheErrorCode::Success) { // F1-32: escrita parcial e erro
        const CacheErrorCode injected = std::exchange(impl_->fail_write, CacheErrorCode::Success);
        if (const auto rc =
                impl_->files[index].write_at(base + offset, data.data(), data.size() / 2);
            rc != CacheErrorCode::Success)
            return fail(rc);
        return fail(injected);
    }
    if (const auto rc = impl_->files[index].write_at(base + offset, data.data(), data.size());
        rc != CacheErrorCode::Success) {
        return fail(rc);
    }
    return {};
}

Result<void, CacheErrorCode> StoreFiles::read(SlotId slot, size_t offset, std::span<std::byte> out)
{
    if (const auto rc = impl_->usable(); rc != CacheErrorCode::Success) return fail(rc);
    if (slot >= impl_->total_slots) return fail(CacheErrorCode::SlotNotFound);
    if (offset > impl_->layout.slot_size || out.size() > impl_->layout.slot_size - offset ||
        (out.empty() && offset == impl_->layout.slot_size)) {
        return fail(CacheErrorCode::InvalidOffset);
    }
    if (auto r = ensure_slot(slot); !r) return r;
    const uint32_t index = slot / impl_->layout.slots_per_file;
    const uint64_t base = uint64_t{slot % impl_->layout.slots_per_file} * impl_->layout.slot_size;
    if (const auto rc = impl_->files[index].read_at(base + offset, out.data(), out.size());
        rc != CacheErrorCode::Success) {
        return fail(rc);
    }
    return {};
}

Result<void, CacheErrorCode> StoreFiles::flush()
{
    if (const auto rc = impl_->usable(); rc != CacheErrorCode::Success) return fail(rc);
    for (File& f : impl_->files) {
        if (f.is_open()) {
            if (const auto rc = impl_->sync(f); rc != CacheErrorCode::Success) return fail(rc);
        }
    }
    return {};
}

Result<void, CacheErrorCode> StoreFiles::close()
{
    if (impl_->closed) return {};
    impl_->closed = true;
    CacheErrorCode first = CacheErrorCode::Success;
    for (File& f : impl_->files) {
        const auto rc = f.close(); // fecha todos mesmo assim; devolve o primeiro erro
        if (rc != CacheErrorCode::Success && first == CacheErrorCode::Success) first = rc;
    }
    if (first != CacheErrorCode::Success) return fail(first);
    return {};
}

bool StoreFiles::broken() const noexcept { return impl_->broken; }

uint32_t StoreFiles::total_slots() const noexcept { return impl_->total_slots; }
const StoreLayout& StoreFiles::layout() const noexcept { return impl_->layout; }
uint32_t StoreFiles::file_count() const noexcept
{
    return static_cast<uint32_t>(impl_->files.size());
}
uint32_t StoreFiles::files_created() const noexcept { return impl_->created; }

std::filesystem::path StoreFiles::file_path(const std::filesystem::path& dir, uint32_t index)
{
    return file_name(dir, index);
}

Result<void, CacheErrorCode> StoreFiles::shrink_to_ceiling()
{
    if (const auto rc = impl_->usable(); rc != CacheErrorCode::Success) return fail(rc);
    // Teto 0 apagaria todos os store-NNN.bin: é configuração errada, não "teto menor".
    if (impl_->total_slots == 0) return fail(CacheErrorCode::InvalidSize);
    namespace fs = std::filesystem;
    const uint32_t count = file_count();
    // F1-72, Regra R: roda no start() da CacheOwner. Nenhum fs::path por valor, nenhuma conversão
    // do nome para estreito, nenhum std::vector<fs::path> (crescer o vetor MOVERIA os caminhos) e
    // nenhum directory_iterator (ver list_store_files_beyond): só os números dos arquivos além do
    // teto são guardados, e cada caminho nasce por CÓPIA da pasta.
    // Arquivos inteiros além do teto (só os de nome canônico "store-NNN.bin").
    std::vector<uint32_t> beyond(std::size_t{0});
    if (const auto rc = list_store_files_beyond(impl_->dir, count, beyond);
        rc != CacheErrorCode::Success)
        return fail(rc);
    std::error_code ec;
    for (const uint32_t n : beyond) {
        fs::path p(impl_->dir);
        if (!detail::append_store_file_leaf(p, n)) return fail(CacheErrorCode::Internal);
        if (detail::remove_file(p) != CacheErrorCode::Success) return fail(CacheErrorCode::IoError);
    }
    // Arquivos maiores que o layout pede agora (o último, quando o teto não fecha um arquivo).
    for (uint32_t i = 0; i < count; ++i) {
        fs::path p(impl_->dir);
        if (!detail::append_store_file_leaf(p, i)) return fail(CacheErrorCode::Internal);
        if (!fs::exists(p, ec) || ec) {
            if (ec) return fail(CacheErrorCode::IoError);
            continue;
        }
        const uint64_t size = fs::file_size(p, ec);
        if (ec) return fail(CacheErrorCode::IoError);
        const uint64_t wanted = impl_->file_bytes(i);
        if (size <= wanted) continue;
        File& open_file = impl_->files[i];
        File local;
        File& f = open_file.is_open() ? open_file : local;
        if (!f.is_open()) {
            if (const auto rc = f.open(p); rc != CacheErrorCode::Success) return fail(rc);
        }
        CacheErrorCode rc = f.truncate(wanted);
        if (rc == CacheErrorCode::Success) rc = impl_->sync(f);
        if (&f == &local) {
            const CacheErrorCode closed = local.close();
            if (rc == CacheErrorCode::Success) rc = closed;
        }
        if (rc != CacheErrorCode::Success) return fail(rc);
    }
    if (!beyond.empty() && sync_directory(impl_->dir) != CacheErrorCode::Success) {
        impl_->broken = true;
        return fail(CacheErrorCode::IoError);
    }
    return {};
}

Result<uint64_t, CacheErrorCode> StoreFiles::allocated_bytes(const std::filesystem::path& file)
{
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(CacheErrorCode::IoError);
    FILE_STANDARD_INFO info{};
    const BOOL ok = ::GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof info);
    const BOOL closed = ::CloseHandle(h); // só leitura de atributos, mas nada calado
    if (!ok || !closed) return fail(CacheErrorCode::IoError);
    return static_cast<uint64_t>(info.AllocationSize.QuadPart);
#else
    struct stat st
    {
    };
    if (::stat(file.c_str(), &st) != 0) return fail(CacheErrorCode::IoError);
    return static_cast<uint64_t>(st.st_blocks) * 512u;
#endif
}

void StoreFilesTestHook::fail_next_sync(StoreFiles& store) noexcept
{
    store.impl_->fail_next_sync = true;
}

uint32_t StoreFilesTestHook::directory_syncs(const StoreFiles& store) noexcept
{
    return store.impl_->directory_syncs;
}

uint64_t StoreFilesTestHook::file_syncs(const StoreFiles& store) noexcept
{
    return store.impl_->file_syncs;
}

void StoreFilesTestHook::skip_space_check(StoreFiles& store) noexcept
{
    store.impl_->skip_space_check = true;
}

void StoreFilesTestHook::fail_next_write(StoreFiles& store, CacheErrorCode code) noexcept
{
    store.impl_->fail_write = code;
}

void StoreFilesTestHook::force_posix_fallocate(StoreFiles& store) noexcept
{
    store.impl_->native_preallocate = false;
}

} // namespace swarmstream
