// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "cache_dir_lock.hpp"

#include <cerrno>
#include <new>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace swarmstream::detail {

namespace {

namespace fs = std::filesystem;

LockStatus make_status(LockCode code, int os_error, std::string detail)
{
    LockStatus s;
    s.code = code;
    s.os_error = os_error;
    s.detail = std::move(detail);
    return s;
}

std::string describe(const char* what, const fs::path& path, int os_error)
{
    std::string text = std::string(what) + " '" + path.string() + "'";
    if (os_error != 0) {
        text += ": ";
        text += std::system_category().message(os_error);
        text += " (codigo " + std::to_string(os_error) + ")";
    }
    return text;
}

#if defined(_WIN32)
int last_os_error() noexcept
{
    return static_cast<int>(::GetLastError());
}
#else
int last_os_error() noexcept
{
    return errno;
}
#endif

LockStatus acquire_impl(const fs::path& cache_dir, bool& taken,
#if defined(_WIN32)
                        HANDLE& handle, fs::path& lock_path)
#else
                        int& handle, fs::path& lock_path)
#endif
{
    taken = false;

    // Única escrita permitida antes da aquisição, junto com o próprio arquivo: a pasta.
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec)
        return make_status(LockCode::Io, ec.value(),
                           describe("nao foi possivel criar a pasta de cache", cache_dir, ec.value()));
    const bool is_dir = fs::is_directory(cache_dir, ec);
    if (ec)
        return make_status(LockCode::Io, ec.value(),
                           describe("nao foi possivel examinar a pasta de cache", cache_dir, ec.value()));
    if (!is_dir)
        return make_status(LockCode::Io, 0,
                           describe("o caminho da pasta de cache nao e uma pasta", cache_dir, 0));

    const fs::path path = cache_dir / kCacheLockFileName;

#if defined(_WIN32)
    // Sem SECURITY_ATTRIBUTES: o handle NÃO é herdável. OPEN_ALWAYS não trunca.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const int e = last_os_error();
        return make_status(LockCode::Io, e, describe("nao foi possivel abrir o arquivo de lock", path, e));
    }
    OVERLAPPED ov{}; // byte 0
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        const int e = last_os_error();
        // Último recurso: a falha reportada é a do lock, não a do fechamento.
        (void)::CloseHandle(h);
        if (e == ERROR_LOCK_VIOLATION)
            return make_status(LockCode::Locked, e,
                               describe("a pasta de cache ja esta em uso por outro dono", path, e));
        return make_status(LockCode::Io, e, describe("LockFileEx falhou em", path, e));
    }
#else
    int h = -1;
    do {
        h = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    } while (h < 0 && errno == EINTR);
    if (h < 0) {
        const int e = last_os_error();
        return make_status(LockCode::Io, e, describe("nao foi possivel abrir o arquivo de lock", path, e));
    }
    int rc = 0;
    do {
        rc = ::flock(h, LOCK_EX | LOCK_NB);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        const int e = last_os_error();
        // Último recurso: a falha reportada é a do lock, não a do fechamento.
        (void)::close(h);
        if (e == EWOULDBLOCK || e == EAGAIN)
            return make_status(LockCode::Locked, e,
                               describe("a pasta de cache ja esta em uso por outro dono", path, e));
        return make_status(LockCode::Io, e, describe("flock falhou em", path, e));
    }
#endif
    handle = h;
    lock_path = path;
    taken = true;
    return LockStatus{};
}

} // namespace

CacheDirLock::CacheDirLock(CacheDirLock&& o) noexcept
    : h_(o.h_), held_(o.held_), lock_path_(std::move(o.lock_path_))
{
#if defined(_WIN32)
    o.h_ = INVALID_HANDLE_VALUE;
#else
    o.h_ = -1;
#endif
    o.held_ = false;
}

CacheDirLock& CacheDirLock::operator=(CacheDirLock&& o) noexcept
{
    if (this != &o) {
        (void)release(); // último recurso: quem quer o erro chama release() antes
        h_ = o.h_;
        held_ = o.held_;
        lock_path_ = std::move(o.lock_path_);
#if defined(_WIN32)
        o.h_ = INVALID_HANDLE_VALUE;
#else
        o.h_ = -1;
#endif
        o.held_ = false;
    }
    return *this;
}

CacheDirLock::~CacheDirLock()
{
    (void)release(); // último recurso
}

LockStatus CacheDirLock::acquire(const fs::path& cache_dir, CacheDirLock& out) noexcept
{
    try {
        if (out.held_)
            return make_status(LockCode::Io, 0, "acquire: o objeto ja segura um lock (uso incorreto)");
        bool taken = false;
#if defined(_WIN32)
        HANDLE handle = INVALID_HANDLE_VALUE;
#else
        int handle = -1;
#endif
        fs::path lock_path;
        LockStatus status = acquire_impl(cache_dir, taken, handle, lock_path);
        if (taken) {
            out.h_ = handle;
            out.held_ = true;
            out.lock_path_ = std::move(lock_path);
        }
        return status;
    } catch (const std::bad_alloc&) {
        LockStatus s;
        s.code = LockCode::OutOfMemory; // sem texto: montá-lo alocaria
        return s;
    } catch (...) {
        LockStatus s;
        s.code = LockCode::Io;
        return s;
    }
}

LockStatus CacheDirLock::release() noexcept
{
    if (!held_) return LockStatus{};
    // O objeto é dado por solto mesmo se uma chamada falhar: repetir seria pior.
    held_ = false;
    int first_error = 0;
    const char* first_what = nullptr;
#if defined(_WIN32)
    OVERLAPPED ov{};
    if (!::UnlockFileEx(h_, 0, 1, 0, &ov)) {
        first_error = last_os_error();
        first_what = "UnlockFileEx falhou em";
    }
    if (!::CloseHandle(h_) && first_what == nullptr) {
        first_error = last_os_error();
        first_what = "CloseHandle falhou em";
    }
    h_ = INVALID_HANDLE_VALUE;
#else
    // LOCK_UN explícito: o lock vale para a abertura, e um filho criado com fork() sem exec()
    // ainda a compartilharia depois do close() daqui.
    int rc = 0;
    do {
        rc = ::flock(h_, LOCK_UN);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        first_error = last_os_error();
        first_what = "flock(LOCK_UN) falhou em";
    }
    // Não repete em EINTR: no Linux o descritor já foi liberado (como em File::close).
    if (::close(h_) != 0 && first_what == nullptr) {
        first_error = last_os_error();
        first_what = "close falhou em";
    }
    h_ = -1;
#endif
    if (first_what == nullptr) return LockStatus{};
    try {
        return make_status(LockCode::Io, first_error, describe(first_what, lock_path_, first_error));
    } catch (...) {
        LockStatus s;
        s.code = LockCode::Io;
        s.os_error = first_error;
        return s;
    }
}

} // namespace swarmstream::detail
