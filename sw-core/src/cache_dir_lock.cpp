// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "cache_dir_lock.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cwchar>
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

// Nada global aqui tem construtor que aloque: só constantes (`constexpr`, inicialização constante).
// No MSVC em Debug (_ITERATOR_DEBUG_LEVEL != 0) os construtores PADRÃO e de movimento de
// std::string/std::wstring/fs::path são noexcept e alocam o proxy dos iteradores, então uma falta
// de memória ali é std::terminate antes de qualquer catch (pilhas medidas no CI, runs 41511882294
// e 41514250716: path::operator/=<const char*> e fs::create_directories). Por isso, NO WINDOWS, o
// caminho do arquivo de lock é um buffer `new wchar_t[]` e as chamadas ao SO são as nativas.
constexpr NativeChar kNoPath[1] = {0};

#if defined(_WIN32)
using NativeHandle = HANDLE;
inline NativeHandle invalid_handle() noexcept { return INVALID_HANDLE_VALUE; }
inline bool is_valid(NativeHandle h) noexcept { return h != INVALID_HANDLE_VALUE; }
int last_os_error() noexcept { return static_cast<int>(::GetLastError()); }
using LockPathArg = NativeChar*; // o buffer é cortado com NUL durante as chamadas de pasta
constexpr NativeChar kLockLeaf[] = L"swarmstream.lock";
constexpr NativeChar kSeparator = L'\\';
inline bool is_separator(NativeChar c) noexcept { return c == L'\\' || c == L'/'; }
#else
using NativeHandle = int;
inline NativeHandle invalid_handle() noexcept { return -1; }
inline bool is_valid(NativeHandle h) noexcept { return h >= 0; }
int last_os_error() noexcept { return errno; }
using LockPathArg = const NativeChar*;
#endif

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

// ------------------------------------------------------------------------ texto de diagnóstico
// Só roda em falha, nunca depois de o lock ser tomado com sucesso. Só usa construtores e operações
// que não são noexcept (cópia a partir de `const char*`, append, resize): nunca o construtor padrão
// de std::string, que no MSVC em Debug é noexcept e aloca.

// Acrescenta o caminho nativo como UTF-8 (Windows: WideCharToMultiByte; POSIX: os bytes).
void append_native(std::string& out, const NativeChar* p, std::size_t len)
{
#if defined(_WIN32)
    if (len == 0) return;
    const int need =
        ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    const std::size_t old = out.size();
    out.resize(old + static_cast<std::size_t>(need));
    (void)::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), &out[old], need, nullptr,
                                nullptr);
#else
    out.append(p, len);
#endif
}

void append_int(std::string& out, int value)
{
    char digits[16];
    std::size_t n = 0;
    unsigned int v =
        value < 0 ? 0U - static_cast<unsigned int>(value) : static_cast<unsigned int>(value);
    do {
        digits[n++] = static_cast<char>('0' + v % 10U);
        v /= 10U;
    } while (v != 0);
    if (value < 0) out.append(1, '-');
    while (n > 0)
        out.append(1, digits[--n]);
}

// Mensagem do SO. Windows: FormatMessageW num buffer de pilha (sem std::string intermediária).
void append_os_message(std::string& out, int os_error)
{
#if defined(_WIN32)
    wchar_t buf[512];
    DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                                   FORMAT_MESSAGE_MAX_WIDTH_MASK,
                               nullptr, static_cast<DWORD>(os_error), 0, buf, 512, nullptr);
    while (n > 0 &&
           (buf[n - 1] == L' ' || buf[n - 1] == L'.' || buf[n - 1] == L'\r' || buf[n - 1] == L'\n'))
        --n;
    if (n == 0) {
        out.append("erro desconhecido");
        return;
    }
    append_native(out, buf, n);
#else
    out.append(std::system_category().message(os_error));
#endif
}

std::shared_ptr<const std::string> make_text(const char* what)
{
    return std::make_shared<const std::string>(what);
}

std::shared_ptr<const std::string> make_detail(const char* what, const NativeChar* path,
                                               std::size_t path_len, int os_error)
{
    auto text = std::make_shared<std::string>(what);
    text->append(" '");
    append_native(*text, path, path_len);
    text->append("'");
    if (os_error != 0) {
        text->append(": ");
        append_os_message(*text, os_error);
        text->append(" (codigo ");
        append_int(*text, os_error);
        text->append(")");
    }
    return text;
}

std::size_t native_length(const NativeChar* p) noexcept
{
#if defined(_WIN32)
    return std::wcslen(p);
#else
    return std::strlen(p);
#endif
}

std::shared_ptr<const std::string> make_detail(const char* what, const NativeChar* path,
                                               int os_error)
{
    return make_detail(what, path, native_length(path), os_error);
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

#if defined(_WIN32)
// ----------------------------------------------------- pasta, com chamadas nativas (só Windows)
enum class DirKind { Missing, Directory, NotDirectory, Error };

// Corta `buf` com NUL em `at` enquanto o objeto existir e restaura o caractere no destrutor.
class TerminateAt
{
public:
    TerminateAt(NativeChar* buf, std::size_t at) noexcept : buf_(buf), at_(at), saved_(buf[at])
    {
        buf_[at_] = 0;
    }
    TerminateAt(const TerminateAt&) = delete;
    TerminateAt& operator=(const TerminateAt&) = delete;
    ~TerminateAt() { buf_[at_] = saved_; }

private:
    NativeChar* buf_;
    std::size_t at_;
    NativeChar saved_;
};

DirKind dir_kind(NativeChar* buf, std::size_t len, int& os_error) noexcept
{
    const TerminateAt cut(buf, len);
    const DWORD attr = ::GetFileAttributesW(buf);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return DirKind::Missing;
        os_error = static_cast<int>(e);
        return DirKind::Error;
    }
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DirKind::Directory : DirKind::NotDirectory;
}

// Cria uma pasta; "já existe" é sucesso. Devolve 0 ou o erro do SO.
int make_one_dir(NativeChar* buf, std::size_t len) noexcept
{
    const TerminateAt cut(buf, len);
    if (::CreateDirectoryW(buf, nullptr) != 0) return 0;
    const DWORD e = ::GetLastError();
    return e == ERROR_ALREADY_EXISTS ? 0 : static_cast<int>(e);
}

// Tamanho do pai de buf[0, len): tira os separadores do fim, o último componente e os separadores
// que o antecedem. 0 se não há pai (caminho de um componente só).
std::size_t parent_length(const NativeChar* buf, std::size_t len) noexcept
{
    std::size_t p = len;
    while (p > 0 && is_separator(buf[p - 1]))
        --p;
    while (p > 0 && !is_separator(buf[p - 1]))
        --p;
    while (p > 0 && is_separator(buf[p - 1]))
        --p;
    return p;
}

// Garante que `buf[0, len)` é uma pasta e cria as que faltarem (como create_directories, mas sem
// std::filesystem e sem alocar). Sobe a partir da pasta final até achar um pai que existe (ou cuja
// situação não dá para saber), então cria de cima para baixo; falha nos pais é ignorada (quem
// decide é a pasta final). Não sonda a raiz de um UNC nem o prefixo estendido: só os ancestrais
// que faltam. Devolve Directory, NotDirectory ou Error (com `os_error`).
DirKind ensure_directory(NativeChar* buf, std::size_t len, int& os_error) noexcept
{
    const DirKind first = dir_kind(buf, len, os_error);
    if (first != DirKind::Missing) return first;
    std::size_t cut = len; // buf[0, cut) é sabidamente ausente
    for (;;) {
        const std::size_t p = parent_length(buf, cut);
        if (p == 0) break;
        int ignored = 0;
        if (dir_kind(buf, p, ignored) != DirKind::Missing) break;
        cut = p;
    }
    for (std::size_t i = cut;;) {
        const int e = make_one_dir(buf, i);
        if (i >= len) {
            if (e != 0) {
                os_error = e;
                return DirKind::Error;
            }
            break;
        }
        while (i < len && is_separator(buf[i]))
            ++i;
        while (i < len && !is_separator(buf[i]))
            ++i;
    }
    return dir_kind(buf, len, os_error);
}
#endif

// Tenta tomar o lock. Regra de ouro desta função (a aquisição que falha não pode deixar o recurso
// preso): TODA alocação acontece ANTES de o arquivo ser aberto (o chamador já montou o caminho)
// ou numa falha, com o arquivo sob a guarda `OpenLockFile`. Depois do lock tomado só há
// atribuições escalares e a devolução de um LockStatus vazio (sem alocação). Em Ok o arquivo
// aberto e travado fica em `handle`. `dir_len` (só Windows) é o tamanho de <cache_dir> no buffer.
LockStatus acquire_impl([[maybe_unused]] const fs::path& cache_dir, LockPathArg lock_path,
                        [[maybe_unused]] std::size_t dir_len, NativeHandle& handle)
{
    // Única escrita permitida antes da aquisição, junto com o próprio arquivo: a pasta.
#if defined(_WIN32)
    int os_error = 0;
    switch (ensure_directory(lock_path, dir_len, os_error)) {
    case DirKind::Directory: break;
    case DirKind::NotDirectory:
        return make_status(
            LockCode::Io, 0,
            make_detail("o caminho da pasta de cache nao e uma pasta", lock_path, dir_len, 0));
    case DirKind::Missing:
    case DirKind::Error:
        return make_status(LockCode::Io, os_error,
                           make_detail("nao foi possivel criar ou examinar a pasta de cache",
                                       lock_path, dir_len, os_error));
    }

    // Sem SECURITY_ATTRIBUTES: o handle NÃO é herdável. OPEN_ALWAYS não trunca.
    OpenLockFile file(::CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE,
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
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec)
        return make_status(LockCode::Io, ec.value(),
                           make_detail("nao foi possivel criar a pasta de cache", cache_dir.c_str(),
                                       cache_dir.native().size(), ec.value()));
    const bool is_dir = fs::is_directory(cache_dir, ec);
    if (ec)
        return make_status(LockCode::Io, ec.value(),
                           make_detail("nao foi possivel examinar a pasta de cache",
                                       cache_dir.c_str(), cache_dir.native().size(), ec.value()));
    if (!is_dir)
        return make_status(LockCode::Io, 0,
                           make_detail("o caminho da pasta de cache nao e uma pasta",
                                       cache_dir.c_str(), cache_dir.native().size(), 0));

    NativeHandle raw = -1;
    do {
        raw = ::open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
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

#if defined(_WIN32)
std::unique_ptr<NativeChar[]> compose_lock_path(const fs::path& cache_dir, std::size_t& dir_len)
{
    const auto& dir = cache_dir.native();
    dir_len = dir.size();
    constexpr std::size_t kLeafLen = sizeof(kLockLeaf) / sizeof(kLockLeaf[0]) - 1;
    // "C:" é "a pasta atual do disco C": colar o nome direto (como fs::path faz), sem separador.
    const bool drive_only = dir_len == 2 && dir[1] == L':';
    const bool add_sep = !is_separator(dir[dir_len - 1]) && !drive_only;
    const std::size_t total = dir_len + (add_sep ? 1 : 0) + kLeafLen + 1;
    std::unique_ptr<NativeChar[]> buf(new NativeChar[total]); // new[] não é noexcept
    std::memcpy(buf.get(), dir.data(), dir_len * sizeof(NativeChar));
    std::size_t at = dir_len;
    if (add_sep) buf[at++] = kSeparator;
    std::memcpy(buf.get() + at, kLockLeaf, (kLeafLen + 1) * sizeof(NativeChar));
    return buf;
}
#endif

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

const NativeChar* CacheDirLock::native_path() const noexcept
{
    if (!lock_path_) return kNoPath;
#if defined(_WIN32)
    return lock_path_.get();
#else
    return lock_path_->c_str();
#endif
}

LockStatus CacheDirLock::acquire(const fs::path& cache_dir, CacheDirLock& out) noexcept
{
    try {
        if (out.held_)
            return make_status(LockCode::Io, 0,
                               make_text("acquire: o objeto ja segura um lock (uso incorreto)"));
        if (cache_dir.native().empty())
            return make_status(LockCode::Io, 0,
                               make_text("acquire: o caminho da pasta de cache esta vazio"));
            // Toda a alocação do caminho acontece AQUI, antes de qualquer arquivo ser aberto.
            // Windows: um único `new wchar_t[]`. POSIX: cópia do fs::path e make_unique. Nenhum
            // desses é noexcept: uma falta de memória vira bad_alloc, nunca std::terminate.
#if defined(_WIN32)
        std::size_t dir_len = 0;
        std::unique_ptr<NativeChar[]> lock_path = compose_lock_path(cache_dir, dir_len);
        const LockPathArg lock_arg = lock_path.get();
#else
        const std::size_t dir_len = 0;
        std::unique_ptr<fs::path> lock_path = std::make_unique<fs::path>(cache_dir);
        *lock_path /= kCacheLockFileName;
        const LockPathArg lock_arg = lock_path->c_str();
#endif

        NativeHandle handle = invalid_handle();
        LockStatus status = acquire_impl(cache_dir, lock_arg, dir_len, handle);
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
                                 make_detail(first_what, native_path(), first_error));
        } catch (...) {
            result.code = LockCode::Io;
            result.os_error = first_error;
        }
    }
    lock_path_.reset(); // desalocar não lança
    return result;
}

} // namespace swarmstream::detail
