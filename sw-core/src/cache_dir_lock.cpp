// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "cache_dir_lock.hpp"

#include <cerrno>
#include <memory>
#include <new>
#include <string>
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

#if defined(_WIN32)
using NativeHandle = HANDLE;
inline NativeHandle invalid_handle() noexcept { return INVALID_HANDLE_VALUE; }
inline bool is_valid(NativeHandle h) noexcept { return h != INVALID_HANDLE_VALUE; }
int last_os_error() noexcept { return static_cast<int>(::GetLastError()); }
#else
using NativeHandle = int;
inline NativeHandle invalid_handle() noexcept { return -1; }
inline bool is_valid(NativeHandle h) noexcept { return h >= 0; }
int last_os_error() noexcept { return errno; }
#endif

// Caminho devolvido por path() quando não há lock. Construído na inicialização estática, nunca
// dentro de uma função noexcept que possa estar sem memória.
const fs::path kEmptyPath;

// Nome do arquivo de lock como `path`, também construído na inicialização estática. Anexar um
// `const char*` ao caminho DENTRO de acquire() converteria estreito -> largo na hora, e no MSVC em
// Debug essa conversão passa pelo construtor padrão (noexcept) de std::wstring, que aloca o proxy
// dos iteradores: um bad_alloc ali é std::terminate (pilha medida no CI, run 41511882294, K=4).
const fs::path kLockFileLeaf(kCacheLockFileName);

// Guarda do arquivo de lock aberto: se QUALQUER coisa sair de acquire_impl depois da abertura (um
// bad_alloc montando o texto do erro, por exemplo), o destrutor fecha o arquivo, o que também
// solta o lock do SO. Só `dismiss()` (a aquisição completa) tira o arquivo da guarda.
class OpenLockFile
{
public:
    OpenLockFile() noexcept = default;
    explicit OpenLockFile(NativeHandle h) noexcept : h_(h) {}
    OpenLockFile(const OpenLockFile&) = delete;
    OpenLockFile& operator=(const OpenLockFile&) = delete;
    // Último recurso: o erro de fechamento não tem para onde ir (a falha já está sendo reportada).
    ~OpenLockFile()
    {
        if (!is_valid(h_)) return;
#if defined(_WIN32)
        (void)::CloseHandle(h_);
#else
        (void)::close(h_);
#endif
    }
    [[nodiscard]] NativeHandle get() const noexcept { return h_; }
    [[nodiscard]] NativeHandle dismiss() noexcept
    {
        const NativeHandle h = h_;
        h_ = invalid_handle();
        return h;
    }

private:
    NativeHandle h_ = invalid_handle();
};

// Texto do diagnóstico. Só roda em falha, nunca depois de o lock ser tomado com sucesso.
std::shared_ptr<const std::string> make_text(const char* what)
{
    return std::make_shared<const std::string>(what);
}

std::shared_ptr<const std::string> make_detail(const char* what, const fs::path& path, int os_error)
{
    auto text = std::make_shared<std::string>(what);
    text->append(" '");
    text->append(path.string());
    text->append("'");
    if (os_error != 0) {
        text->append(": ");
        text->append(std::system_category().message(os_error));
        text->append(" (codigo ");
        text->append(std::to_string(os_error));
        text->append(")");
    }
    return text;
}

LockStatus make_status(LockCode code, int os_error,
                       std::shared_ptr<const std::string> detail) noexcept
{
    LockStatus s;
    s.code = code;
    s.os_error = os_error;
    s.detail = std::move(detail);
    return s;
}

// Tenta tomar o lock. Regra de ouro desta função (a aquisição que falha não pode deixar o recurso
// preso): TODA alocação acontece ANTES de o arquivo ser aberto (o chamador já montou
// `lock_path`) ou numa falha, com o arquivo sob a guarda `OpenLockFile`. Depois do lock tomado
// só há atribuições escalares e a devolução de um LockStatus vazio (sem alocação).
// Em Ok o arquivo aberto e travado fica em `handle`.
LockStatus acquire_impl(const fs::path& cache_dir, const fs::path& lock_path, NativeHandle& handle)
{
    // Única escrita permitida antes da aquisição, junto com o próprio arquivo: a pasta.
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec)
        return make_status(
            LockCode::Io, ec.value(),
            make_detail("nao foi possivel criar a pasta de cache", cache_dir, ec.value()));
    const bool is_dir = fs::is_directory(cache_dir, ec);
    if (ec)
        return make_status(
            LockCode::Io, ec.value(),
            make_detail("nao foi possivel examinar a pasta de cache", cache_dir, ec.value()));
    if (!is_dir)
        return make_status(
            LockCode::Io, 0,
            make_detail("o caminho da pasta de cache nao e uma pasta", cache_dir, 0));

#if defined(_WIN32)
    // Sem SECURITY_ATTRIBUTES: o handle NÃO é herdável. OPEN_ALWAYS não trunca.
    OpenLockFile file(::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!is_valid(file.get())) {
        const int e = last_os_error();
        return make_status(LockCode::Io, e,
                           make_detail("nao foi possivel abrir o arquivo de lock", lock_path, e));
    }
    OVERLAPPED ov{}; // byte 0
    if (!::LockFileEx(file.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                      &ov)) {
        const int e = last_os_error();
        if (e == ERROR_LOCK_VIOLATION)
            return make_status(
                LockCode::Locked, e,
                make_detail("a pasta de cache ja esta em uso por outro dono", lock_path, e));
        return make_status(LockCode::Io, e, make_detail("LockFileEx falhou em", lock_path, e));
    }
#else
    NativeHandle raw = -1;
    do {
        raw = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    } while (raw < 0 && errno == EINTR);
    OpenLockFile file(raw);
    if (!is_valid(file.get())) {
        const int e = last_os_error();
        return make_status(LockCode::Io, e,
                           make_detail("nao foi possivel abrir o arquivo de lock", lock_path, e));
    }
    int rc = 0;
    do {
        rc = ::flock(file.get(), LOCK_EX | LOCK_NB);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        const int e = last_os_error();
        if (e == EWOULDBLOCK || e == EAGAIN)
            return make_status(
                LockCode::Locked, e,
                make_detail("a pasta de cache ja esta em uso por outro dono", lock_path, e));
        return make_status(LockCode::Io, e, make_detail("flock falhou em", lock_path, e));
    }
#endif
    // Lock tomado: daqui em diante nada pode lançar.
    handle = file.dismiss();
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

const fs::path& CacheDirLock::path() const noexcept
{
    return lock_path_ ? *lock_path_ : kEmptyPath;
}

LockStatus CacheDirLock::acquire(const fs::path& cache_dir, CacheDirLock& out) noexcept
{
    try {
        if (out.held_)
            return make_status(LockCode::Io, 0,
                               make_text("acquire: o objeto ja segura um lock (uso incorreto)"));
        // Toda a alocação do caminho acontece AQUI, antes de qualquer arquivo ser aberto. Os
        // construtores usados (cópia, make_unique) não são noexcept: uma falta de memória vira
        // bad_alloc, nunca std::terminate, nem no MSVC em Debug.
        std::unique_ptr<fs::path> lock_path = std::make_unique<fs::path>(cache_dir);
        *lock_path /= kLockFileLeaf;

        NativeHandle handle = invalid_handle();
        LockStatus status = acquire_impl(cache_dir, *lock_path, handle);
        if (status.ok()) {
            // Lock tomado: só atribuições escalares e mover um ponteiro (noexcept de verdade).
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
    LockStatus result;
    if (first_what != nullptr) {
        try {
            result = make_status(LockCode::Io, first_error,
                                 make_detail(first_what, path(), first_error));
        } catch (...) {
            result.code = LockCode::Io;
            result.os_error = first_error;
        }
    }
    lock_path_.reset(); // desalocar não lança
    return result;
}

} // namespace swarmstream::detail
