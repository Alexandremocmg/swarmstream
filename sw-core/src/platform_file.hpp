// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Arquivo do cache com todo retorno de E/S conferido (Windows e POSIX). Interno do sw-core: usado
// pelo StoreFiles (F1-10) e pelo journal e pelo índice (F1-11). Saiu de store_files.cpp sem
// mudança de comportamento; a F1-11 acrescentou replace_file e remove_file.
#pragma once

#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>

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
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace swarmstream::detail {

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

    // Reserva os clusters (não esparso) e põe o fim do arquivo em `bytes`. `native` só existe
    // para a versão POSIX (gancho de teste do fallback).
    CacheErrorCode preallocate(uint64_t bytes, [[maybe_unused]] bool native = true) noexcept
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
    // `native` = false pula o fallocate(2) e vai direto ao fallback (StoreFilesTestHook).
    CacheErrorCode preallocate(uint64_t bytes, [[maybe_unused]] bool native = true) noexcept
    {
        int rc = EOPNOTSUPP;
#if defined(__linux__)
        if (native) {
            do {
                rc = ::fallocate(h_, 0, 0, static_cast<off_t>(bytes)) == 0 ? 0 : errno;
            } while (rc == EINTR);
        }
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

// Grava no disco a entrada de diretório dos arquivos criados (POSIX): sem isso, depois de
// queda de energia o store-XXX.bin pode sumir mesmo com os dados sincronizados. No Windows
// não há fsync de pasta; o NTFS registra a criação no journal de metadados dele.
inline CacheErrorCode sync_directory([[maybe_unused]] const std::filesystem::path& dir) noexcept
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

// Troca `to` por `from` de forma atômica e durável (checkpoint do índice e do journal, 04 §4).
// Windows: MoveFileExW com MOVEFILE_WRITE_THROUGH, que só volta depois de o rename estar no
// disco. POSIX: rename(2) e depois fsync da pasta.
inline CacheErrorCode replace_file(const std::filesystem::path& from,
                                   const std::filesystem::path& to) noexcept
{
#if defined(_WIN32)
    if (!::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return CacheErrorCode::IoError;
    return CacheErrorCode::Success;
#else
    if (::rename(from.c_str(), to.c_str()) != 0) return CacheErrorCode::IoError;
    return sync_directory(to.parent_path());
#endif
}

// Apaga um arquivo; não existir já é sucesso.
inline CacheErrorCode remove_file(const std::filesystem::path& p) noexcept
{
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return ec ? CacheErrorCode::IoError : CacheErrorCode::Success;
}

} // namespace swarmstream::detail
