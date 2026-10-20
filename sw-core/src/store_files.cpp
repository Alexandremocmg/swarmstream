// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/store_files.hpp"

#include "store_files_testing.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace swarmstream {

namespace {

constexpr uint32_t kAlignment = 4096; // docs/04 §1: slots alinhados a 4 KiB

std::filesystem::path file_name(const std::filesystem::path& dir, uint32_t index)
{
    char name[32];
    std::snprintf(name, sizeof name, "store-%03u.bin", static_cast<unsigned>(index));
    return dir / name;
}

// Arquivo aberto, dono do handle nativo. Toda operação devolve CacheErrorCode::Success ou
// o erro; nunca ignora um retorno.
class File
{
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept : h_(o.h_) { o.h_ = kInvalid; }
    File& operator=(File&& o) noexcept
    {
        if (this != &o) {
            (void)close(); // último recurso: quem quer o erro chama close() antes
            h_ = o.h_;
            o.h_ = kInvalid;
        }
        return *this;
    }
    ~File() { (void)close(); } // último recurso: StoreFiles::close() devolve o erro

    [[nodiscard]] bool is_open() const noexcept { return h_ != kInvalid; }

#if defined(_WIN32)
    // INVALID_HANDLE_VALUE é um reinterpret_cast: não pode ser constexpr.
    inline static const HANDLE kInvalid = INVALID_HANDLE_VALUE;

    CacheErrorCode open(const std::filesystem::path& p) noexcept
    {
        h_ = ::CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ == kInvalid ? CacheErrorCode::IoError : CacheErrorCode::Success;
    }

    CacheErrorCode size(uint64_t& out) const noexcept
    {
        LARGE_INTEGER s{};
        if (!::GetFileSizeEx(h_, &s)) return CacheErrorCode::IoError;
        out = static_cast<uint64_t>(s.QuadPart);
        return CacheErrorCode::Success;
    }

    // Reserva os clusters (não esparso) e põe o fim do arquivo em `bytes`.
    CacheErrorCode preallocate(uint64_t bytes) noexcept
    {
        FILE_ALLOCATION_INFO alloc{};
        alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(bytes);
        if (!::SetFileInformationByHandle(h_, FileAllocationInfo, &alloc, sizeof alloc))
            return last_error();
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
        if (!::SetFileInformationByHandle(h_, FileEndOfFileInfo, &eof, sizeof eof))
            return last_error();
        return CacheErrorCode::Success;
    }

    CacheErrorCode write_at(uint64_t offset, const std::byte* data, size_t n) noexcept
    {
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD chunk = n > (DWORD{1} << 30) ? (DWORD{1} << 30) : static_cast<DWORD>(n);
            DWORD done = 0;
            if (!::WriteFile(h_, data, chunk, &done, &ov)) return last_error();
            if (done == 0) return CacheErrorCode::IoError;
            offset += done;
            data += done;
            n -= done;
        }
        return CacheErrorCode::Success;
    }

    CacheErrorCode read_at(uint64_t offset, std::byte* data, size_t n) noexcept
    {
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD chunk = n > (DWORD{1} << 30) ? (DWORD{1} << 30) : static_cast<DWORD>(n);
            DWORD done = 0;
            if (!::ReadFile(h_, data, chunk, &done, &ov)) return CacheErrorCode::IoError;
            if (done == 0) return CacheErrorCode::IoError; // fim do arquivo antes do esperado
            offset += done;
            data += done;
            n -= done;
        }
        return CacheErrorCode::Success;
    }

    CacheErrorCode sync() noexcept
    {
        return ::FlushFileBuffers(h_) ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

    // Desfaz uma pré-alocação que falhou: volta o fim do arquivo e a reserva para `bytes`.
    CacheErrorCode truncate(uint64_t bytes) noexcept
    {
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
        if (!::SetFileInformationByHandle(h_, FileEndOfFileInfo, &eof, sizeof eof))
            return CacheErrorCode::IoError;
        FILE_ALLOCATION_INFO alloc{};
        alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(bytes);
        return ::SetFileInformationByHandle(h_, FileAllocationInfo, &alloc, sizeof alloc)
                   ? CacheErrorCode::Success
                   : CacheErrorCode::IoError;
    }

    // Fecha conferindo o retorno. O handle é dado por fechado mesmo se a chamada falhar
    // (repetir CloseHandle num handle já liberado seria pior).
    CacheErrorCode close() noexcept
    {
        if (h_ == kInvalid) return CacheErrorCode::Success;
        const BOOL ok = ::CloseHandle(h_);
        h_ = kInvalid;
        return ok ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

private:
    static CacheErrorCode last_error() noexcept
    {
        const DWORD e = ::GetLastError();
        return (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) ? CacheErrorCode::DiskFull
                                                                     : CacheErrorCode::IoError;
    }
    HANDLE h_ = kInvalid;
#else
    static constexpr int kInvalid = -1;

    CacheErrorCode open(const std::filesystem::path& p) noexcept
    {
        do {
            h_ = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        } while (h_ < 0 && errno == EINTR);
        return h_ < 0 ? CacheErrorCode::IoError : CacheErrorCode::Success;
    }

    CacheErrorCode size(uint64_t& out) const noexcept
    {
        struct stat st
        {
        };
        if (::fstat(h_, &st) != 0) return CacheErrorCode::IoError;
        out = static_cast<uint64_t>(st.st_size);
        return CacheErrorCode::Success;
    }

    // Reserva os blocos de verdade. No Linux usa fallocate(2), que nunca emula; só num sistema
    // de arquivos sem suporte (EOPNOTSUPP: NFS antigo, alguns overlay/ZFS) cai na
    // posix_fallocate, que a glibc emula escrevendo zeros (lento). Em todo caso o espaço livre
    // já foi conferido antes (Impl::open_file), então a emulação não enche o disco.
    CacheErrorCode preallocate(uint64_t bytes) noexcept
    {
        int rc = EOPNOTSUPP;
#if defined(__linux__)
        do {
            rc = ::fallocate(h_, 0, 0, static_cast<off_t>(bytes)) == 0 ? 0 : errno;
        } while (rc == EINTR);
#endif
        if (rc == EOPNOTSUPP || rc == ENOSYS) {
            do {
                rc = ::posix_fallocate(h_, 0, static_cast<off_t>(bytes)); // devolve o erro
            } while (rc == EINTR);
        }
        if (rc == ENOSPC) return CacheErrorCode::DiskFull;
        return rc == 0 ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

    // Desfaz uma pré-alocação que falhou (blocos já reservados voltam ao disco).
    CacheErrorCode truncate(uint64_t bytes) noexcept
    {
        int rc = 0;
        do {
            rc = ::ftruncate(h_, static_cast<off_t>(bytes));
        } while (rc != 0 && errno == EINTR);
        return rc == 0 ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

    CacheErrorCode write_at(uint64_t offset, const std::byte* data, size_t n) noexcept
    {
        while (n > 0) {
            const ssize_t done = ::pwrite(h_, data, n, static_cast<off_t>(offset));
            if (done < 0) {
                if (errno == EINTR) continue;
                return errno == ENOSPC ? CacheErrorCode::DiskFull : CacheErrorCode::IoError;
            }
            if (done == 0) return CacheErrorCode::IoError;
            offset += static_cast<uint64_t>(done);
            data += done;
            n -= static_cast<size_t>(done);
        }
        return CacheErrorCode::Success;
    }

    CacheErrorCode read_at(uint64_t offset, std::byte* data, size_t n) noexcept
    {
        while (n > 0) {
            const ssize_t done = ::pread(h_, data, n, static_cast<off_t>(offset));
            if (done < 0) {
                if (errno == EINTR) continue;
                return CacheErrorCode::IoError;
            }
            if (done == 0) return CacheErrorCode::IoError; // fim do arquivo antes do esperado
            offset += static_cast<uint64_t>(done);
            data += done;
            n -= static_cast<size_t>(done);
        }
        return CacheErrorCode::Success;
    }

    CacheErrorCode sync() noexcept
    {
        int rc = 0;
        do {
            rc = ::fdatasync(h_);
        } while (rc != 0 && errno == EINTR);
        return rc == 0 ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

    // Fecha conferindo o retorno. Não repete em EINTR: no Linux o descritor já foi liberado
    // e repetir poderia fechar outro arquivo aberto no meio-tempo.
    CacheErrorCode close() noexcept
    {
        if (h_ < 0) return CacheErrorCode::Success;
        const int rc = ::close(h_);
        h_ = kInvalid;
        return rc == 0 ? CacheErrorCode::Success : CacheErrorCode::IoError;
    }

private:
    int h_ = kInvalid;
#endif
};

Unexpected<CacheErrorCode> fail(CacheErrorCode code) { return Unexpected(code); }

// Grava no disco a entrada de diretório dos arquivos criados (POSIX): sem isso, depois de
// queda de energia o store-XXX.bin pode sumir mesmo com os dados sincronizados. No Windows
// não há fsync de pasta; o NTFS registra a criação no journal de metadados dele.
CacheErrorCode sync_directory([[maybe_unused]] const std::filesystem::path& dir) noexcept
{
#if defined(_WIN32)
    return CacheErrorCode::Success;
#else
    int fd = -1;
    do {
        fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return CacheErrorCode::IoError;
    int rc = 0;
    do {
        rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
    const int closed = ::close(fd);
    return rc == 0 && closed == 0 ? CacheErrorCode::Success : CacheErrorCode::IoError;
#endif
}

} // namespace

struct StoreFiles::Impl
{
    std::filesystem::path dir;
    uint32_t total_slots = 0;
    StoreLayout layout;
    std::vector<File> files;
    uint32_t created = 0;
    bool closed = false;
    bool broken = false;         // um sync falhou: inutilizável até reabrir (ver o header)
    bool fail_next_sync = false; // StoreFilesTestHook
    uint32_t directory_syncs = 0;

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
        if (rc != CacheErrorCode::Success) broken = true;
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
            CacheErrorCode rc = (!ec && space.available < wanted - current)
                                    ? CacheErrorCode::DiskFull
                                    : f.preallocate(wanted);
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
    auto impl = std::make_unique<Impl>();
    impl->dir = dir;
    impl->total_slots = total_slots;
    impl->layout = layout;
    impl->files.resize((uint64_t{total_slots} + layout.slots_per_file - 1) / layout.slots_per_file);
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

} // namespace swarmstream
