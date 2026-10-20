// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/store_files.hpp"

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
            close();
            h_ = o.h_;
            o.h_ = kInvalid;
        }
        return *this;
    }
    ~File() { close(); }

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

    void close() noexcept
    {
        if (h_ != kInvalid) {
            ::CloseHandle(h_);
            h_ = kInvalid;
        }
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

    CacheErrorCode preallocate(uint64_t bytes) noexcept
    {
        int rc = 0;
        do {
            rc = ::posix_fallocate(h_, 0,
                                   static_cast<off_t>(bytes)); // devolve o erro; não usa errno
        } while (rc == EINTR);
        if (rc == ENOSPC) return CacheErrorCode::DiskFull;
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

    void close() noexcept
    {
        if (h_ >= 0) {
            ::close(h_);
            h_ = kInvalid;
        }
    }

private:
    int h_ = kInvalid;
#endif
};

Unexpected<CacheErrorCode> fail(CacheErrorCode code) { return Unexpected(code); }

} // namespace

struct StoreFiles::Impl
{
    std::filesystem::path dir;
    uint32_t total_slots = 0;
    StoreLayout layout;
    std::vector<File> files;
    uint32_t created = 0;

    [[nodiscard]] uint64_t file_bytes(uint32_t index) const noexcept
    {
        const uint32_t first = index * layout.slots_per_file;
        const uint32_t slots = std::min(layout.slots_per_file, total_slots - first);
        return uint64_t{slots} * layout.slot_size;
    }

    CacheErrorCode open_file(uint32_t index)
    {
        File f;
        if (const auto rc = f.open(file_name(dir, index)); rc != CacheErrorCode::Success) return rc;
        uint64_t current = 0;
        if (const auto rc = f.size(current); rc != CacheErrorCode::Success) return rc;
        const uint64_t wanted = file_bytes(index);
        if (current > wanted) return CacheErrorCode::IoError; // outro layout: não truncar dados
        if (current < wanted) {
            if (const auto rc = f.preallocate(wanted); rc != CacheErrorCode::Success) return rc;
            if (const auto rc = f.size(current); rc != CacheErrorCode::Success) return rc;
            if (current != wanted) return CacheErrorCode::IoError;
        }
        files[index] = std::move(f);
        ++created;
        return CacheErrorCode::Success;
    }
};

StoreFiles::StoreFiles(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StoreFiles::StoreFiles(StoreFiles&&) noexcept = default;
StoreFiles& StoreFiles::operator=(StoreFiles&&) noexcept = default;
StoreFiles::~StoreFiles() = default;

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
    if (slot >= impl_->total_slots) return fail(CacheErrorCode::SlotNotFound);
    const uint32_t index = slot / impl_->layout.slots_per_file;
    if (impl_->files[index].is_open()) return {};
    if (const auto rc = impl_->open_file(index); rc != CacheErrorCode::Success) return fail(rc);
    return {};
}

Result<void, CacheErrorCode> StoreFiles::write(SlotId slot, size_t offset,
                                               std::span<const std::byte> data)
{
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
    for (File& f : impl_->files) {
        if (f.is_open()) {
            if (const auto rc = f.sync(); rc != CacheErrorCode::Success) return fail(rc);
        }
    }
    return {};
}

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
    ::CloseHandle(h);
    if (!ok) return fail(CacheErrorCode::IoError);
    return static_cast<uint64_t>(info.AllocationSize.QuadPart);
#else
    struct stat st
    {
    };
    if (::stat(file.c_str(), &st) != 0) return fail(CacheErrorCode::IoError);
    return static_cast<uint64_t>(st.st_blocks) * 512u;
#endif
}

} // namespace swarmstream
