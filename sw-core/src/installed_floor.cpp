// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "installed_floor.hpp"

#include "platform_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <swarmstream/crypto.hpp>
#include <swarmstream/manifest_view.hpp>
#include <swarmstream/sha256.hpp>
#include <system_error>
#include <utility>
#include <vector>

namespace swarmstream::detail {

namespace {

namespace fs = std::filesystem;

// O maior manifesto que o leitor aceita: envelope + payload (docs/03 §7.1, §8). Um arquivo
// maior que isso não pode ser um manifesto válido e nem é lido (a pasta não é confiável).
constexpr std::size_t kMaxManifestFile =
    pack::kEnvelopeSize + static_cast<std::size_t>(pack::kMaxPayloadLen);

// ---------------------------------------------------------------------------------------------
// Operações reais
// ---------------------------------------------------------------------------------------------

#if defined(_WIN32)
FileReadStatus read_whole_file(const fs::path& path, std::size_t max_bytes,
                               std::vector<std::byte>& out) noexcept
{
    // Somente leitura e SEM criar: File::open (platform_file.hpp) abre para escrita e cria.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FileReadStatus::NotFound
                                                                        : FileReadStatus::Io;
    }
    FileReadStatus result = FileReadStatus::Ok;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        result = FileReadStatus::Io;
    } else if (static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
        result = FileReadStatus::TooLarge;
    } else {
        try {
            out.resize(static_cast<std::size_t>(size.QuadPart));
        } catch (const std::bad_alloc&) {
            result = FileReadStatus::OutOfMemory;
        } catch (...) {
            result = FileReadStatus::Io;
        }
        std::uint64_t offset = 0;
        std::byte* dst = out.data();
        std::size_t left = result == FileReadStatus::Ok ? out.size() : 0;
        while (left > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD chunk =
                left > (DWORD{1} << 30) ? (DWORD{1} << 30) : static_cast<DWORD>(left);
            DWORD done = 0;
            if (!::ReadFile(h, dst, chunk, &done, &ov) || done == 0) {
                result = FileReadStatus::Io; // fim do arquivo antes do esperado: ele encolheu
                break;
            }
            offset += done;
            dst += done;
            left -= done;
        }
    }
    if (!::CloseHandle(h) && result == FileReadStatus::Ok) result = FileReadStatus::Io;
    return result;
}
#else
FileReadStatus read_whole_file(const fs::path& path, std::size_t max_bytes,
                               std::vector<std::byte>& out) noexcept
{
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return errno == ENOENT ? FileReadStatus::NotFound : FileReadStatus::Io;
    FileReadStatus result = FileReadStatus::Ok;
    struct stat st
    {
    };
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        result = FileReadStatus::Io;
    } else if (static_cast<unsigned long long>(st.st_size) > max_bytes) {
        result = FileReadStatus::TooLarge;
    } else {
        try {
            out.resize(static_cast<std::size_t>(st.st_size));
        } catch (const std::bad_alloc&) {
            result = FileReadStatus::OutOfMemory;
        } catch (...) {
            result = FileReadStatus::Io;
        }
        std::uint64_t offset = 0;
        std::byte* dst = out.data();
        std::size_t left = result == FileReadStatus::Ok ? out.size() : 0;
        while (left > 0) {
            const ssize_t done = ::pread(fd, dst, left, static_cast<off_t>(offset));
            if (done < 0) {
                if (errno == EINTR) continue;
                result = FileReadStatus::Io;
                break;
            }
            if (done == 0) {
                result = FileReadStatus::Io; // fim do arquivo antes do esperado: ele encolheu
                break;
            }
            offset += static_cast<std::uint64_t>(done);
            dst += done;
            left -= static_cast<std::size_t>(done);
        }
    }
    // Não repete em EINTR: no Linux o descritor já foi liberado (como em File::close).
    if (::close(fd) != 0 && result == FileReadStatus::Ok) result = FileReadStatus::Io;
    return result;
}
#endif

class RealFloorFileOps final : public FloorFileOps
{
public:
    CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        try {
            std::error_code ec;
            fs::create_directory(dir, ec); // false se já existe: não é erro
            if (ec) return CacheErrorCode::IoError;
            const bool is_dir = fs::is_directory(dir, ec);
            if (ec || !is_dir) return CacheErrorCode::IoError;
            return CacheErrorCode::Success;
        } catch (...) {
            return CacheErrorCode::IoError;
        }
    }

    CacheErrorCode write_file_durable(const fs::path& file,
                                      std::span<const std::byte> data) noexcept override
    {
        File f;
        CacheErrorCode rc = f.open(file); // cria; NÃO trunca
        if (rc != CacheErrorCode::Success) return rc;
        // Mesmo descritor do começo ao fim: o fsync tem de ver os erros da escrita.
        rc = f.truncate(0);
        if (rc == CacheErrorCode::Success && !data.empty())
            rc = f.write_at(0, data.data(), data.size());
        if (rc == CacheErrorCode::Success) rc = f.sync();
        const CacheErrorCode closed = f.close();
        return rc != CacheErrorCode::Success ? rc : closed;
    }

    FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                             std::vector<std::byte>& out) noexcept override
    {
        out.clear();
        return read_whole_file(file, max_bytes, out);
    }

    CacheErrorCode replace(const fs::path& from, const fs::path& to) noexcept override
    {
#if defined(_WIN32)
        return replace_file(from, to); // MoveFileExW: nada aqui aloca
#else
        // Equivalente a replace_file (platform_file.hpp:297-309: rename e depois fsync da pasta),
        // salvo o `parent_path()` (que ALOCA) dentro de try. Caso a caso:
        //  (a) rename falha            -> IoError, troca NÃO ocorreu: igual ao helper.
        //  (b) rename e sync_directory ok -> Success: igual.
        //  (c) rename ok, sync_directory falha -> IoError com a troca JÁ feita: igual; o chamador
        //      trata como may_have_changed (piso antigo OU novo).
        //  (d) NOVO: `parent_path()` sem memória depois do rename -> IoError (no helper: o
        //      bad_alloc sai de uma função noexcept e é std::terminate, com a troca já feita).
        //  (e) Diferença de superfície: esta lógica DUPLICA a do helper e pode divergir se ele
        //      mudar (por isso o helper compartilhado não foi alterado: ele também serve ao
        //      índice e ao journal). Testes: (a) test_real_replace_missing_source; (b) toda
        //      gravação com as operações reais no POSIX; (c) test_installed_floor_perm.cpp
        //      (installed/ em 0300, só Linux como não-root); (d) a varredura do POSIX em
        //      test_installed_floor_oom.cpp (só POSIX; não rodou localmente).
        if (::rename(from.c_str(), to.c_str()) != 0) return CacheErrorCode::IoError;
        try {
            return sync_directory(to.parent_path());
        } catch (...) {
            return CacheErrorCode::IoError;
        }
#endif
    }
};

// Inicialização CONSTANTE (nada é construído nem alocado na carga do programa nem na primeira
// chamada): a classe não tem dados e o construtor é constexpr.
constinit RealFloorFileOps g_real_ops;

// ---------------------------------------------------------------------------------------------
// Regra R (F1-69): nada do caminho de falta de memória executa um construtor PADRÃO nem de
// MOVIMENTO de std::string/std::wstring/std::vector/fs::path (nem de struct que os contenha)
// ---------------------------------------------------------------------------------------------
// No MSVC em Debug (_ITERATOR_DEBUG_LEVEL != 0) esses construtores são `noexcept` e ainda assim
// alocam o proxy dos iteradores: um bad_alloc ali é std::terminate NA FRONTEIRA do construtor,
// antes de qualquer catch, e por isso pôr o objeto dentro do `try` não resolve (medido no run
// 41869620737: `FloorRead out;`; o mesmo achado do F1-63, commit bc55e1d). Aqui só se usam
// construtores que NÃO são noexcept (cópia, (ponteiro, tamanho), (n), make_shared) ou tipos que
// não alocam ao construir, copiar e mover (shared_ptr, span, string_view, optional<inteiro>,
// buffers de pilha). Uma alocação que falha vira bad_alloc comum, capturado no `try` de quem chama.
// O que NÃO se sabe e não foi medido está no cartão F1-69 (lista "A medir").

using FloorChar = fs::path::value_type; // wchar_t no Windows, char no POSIX

#if defined(_WIN32)
constexpr FloorChar kDirNative[] = L"installed";
constexpr FloorChar kFileSuffixNative[] = L".manifest";
constexpr FloorChar kTmpSuffixNative[] = L".manifest.tmp";
constexpr FloorChar kIndexNative[] = L"index.bin";
constexpr FloorChar kJournalNative[] = L"journal.bin";
#else
constexpr FloorChar kDirNative[] = "installed";
constexpr FloorChar kFileSuffixNative[] = ".manifest";
constexpr FloorChar kTmpSuffixNative[] = ".manifest.tmp";
constexpr FloorChar kIndexNative[] = "index.bin";
constexpr FloorChar kJournalNative[] = "journal.bin";
#endif

constexpr bool same_text(const char* a, const FloorChar* b) noexcept
{
    while (*a != 0 && *b != 0) {
        if (static_cast<FloorChar>(*a) != *b) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}
constexpr std::size_t text_length(const FloorChar* s) noexcept
{
    std::size_t n = 0;
    while (s[n] != 0)
        ++n;
    return n;
}
// Os nomes nativos acima são os mesmos do cabeçalho (os testes usam os do cabeçalho).
static_assert(same_text(kInstalledDirName, kDirNative));
static_assert(same_text(kInstalledFileSuffix, kFileSuffixNative));
static_assert(same_text(kInstalledTmpSuffix, kTmpSuffixNative));

constexpr std::size_t kHashHexLen = 64; // SHA-256 em hex minúsculo
constexpr std::size_t kLeafCapacity = 80;
static_assert(kHashHexLen + text_length(kFileSuffixNative) + 1 <= kLeafCapacity);
static_assert(kHashHexLen + text_length(kTmpSuffixNative) + 1 <= kLeafCapacity);

// SHA-256(game_id) em hex minúsculo, `kHashHexLen` caracteres em `out` (sem NUL). Não aloca.
// (O mesmo texto de pack::to_hex(pack::sha256(...)); um teste compara os dois.)
template <class Char>
void put_hash_hex(Char* out, std::string_view game_id) noexcept
{
    const auto* p = reinterpret_cast<const std::byte*>(game_id.data());
    const pack::Sha256Digest d = pack::sha256(std::span<const std::byte>(p, game_id.size()));
    constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < d.size(); ++i) {
        out[2 * i] = static_cast<Char>(kDigits[d[i] >> 4]);
        out[2 * i + 1] = static_cast<Char>(kDigits[d[i] & 0x0F]);
    }
}

// "<hex><sufixo>" NUL-terminado, num buffer de pilha.
struct Leaf
{
    FloorChar text[kLeafCapacity];
};

void make_leaf(Leaf& leaf, std::string_view game_id, const FloorChar* suffix) noexcept
{
    put_hash_hex(leaf.text, game_id);
    std::size_t n = kHashHexLen;
    for (; *suffix != 0; ++suffix)
        leaf.text[n++] = *suffix;
    leaf.text[n] = 0;
}

inline bool is_separator(FloorChar c) noexcept
{
#if defined(_WIN32)
    return c == L'\\' || c == L'/';
#else
    return c == '/';
#endif
}

// Equivale ao separador que `operator/=` acrescenta nos caminhos comuns: nenhum se o caminho está
// vazio ou já termina em separador (e, no Windows, se é só "C:", a pasta atual do disco C).
void add_separator_if_needed(fs::path& p)
{
    const auto& n = p.native();
    if (n.empty() || is_separator(n.back())) return;
#if defined(_WIN32)
    if (n.size() == 2 && n[1] == L':') return;
#endif
    const FloorChar sep = fs::path::preferred_separator;
    p += sep;
}

// `p` já é uma CÓPIA de cache_dir (construtor de cópia: não é noexcept). Acrescenta "/<leaf>".
void append_component(fs::path& p, const FloorChar* leaf)
{
    add_separator_if_needed(p);
    p += leaf;
}

// "installed/<hex><sufixo>" em `p` (cópia de cache_dir). Mesmo caminho de
// `cache_dir / "installed" / (hex + sufixo)`, sem construir fs::path nem std::string temporários.
void append_floor_file(fs::path& p, std::string_view game_id, const FloorChar* suffix)
{
    Leaf leaf;
    make_leaf(leaf, game_id, suffix);
    append_component(p, kDirNative);
    append_component(p, leaf.text);
}

// cache_dir/<leaf> existe? Em dúvida (erro ao examinar), avisa (true).
bool cache_file_exists_or_unknown(const fs::path& cache_dir, const FloorChar* leaf)
{
    fs::path p(cache_dir);
    append_component(p, leaf);
    std::error_code ec;
    const bool e = fs::exists(p, ec);
    return ec ? true : e;
}

// ---------------------------------------------------------------------------------------------
// Texto de diagnóstico (só em falha). Cada operação pode lançar bad_alloc: quem chama está num
// `try`. Só usa construtores e operações que NÃO são noexcept (string(const char*), append,
// resize, make_shared).
// ---------------------------------------------------------------------------------------------

#if defined(_WIN32)
// Acrescenta texto largo como UTF-8 (WideCharToMultiByte em dois passos, como o lock).
void append_wide_utf8(std::string& out, const wchar_t* p, std::size_t len)
{
    if (len == 0) return;
    const int need =
        ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    const std::size_t old = out.size();
    out.resize(old + static_cast<std::size_t>(need));
    (void)::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), &out[old], need, nullptr,
                                nullptr);
}
#endif

// O caminho como UTF-8 (Windows) ou os bytes nativos (POSIX). Sem `path::string()`, que no Windows
// converte pela STL (e pela página de código ANSI).
void append_path_text(std::string& out, const fs::path& p)
{
    const auto& n = p.native();
#if defined(_WIN32)
    append_wide_utf8(out, n.data(), n.size());
#else
    out.append(n);
#endif
}

void append_unsigned(std::string& out, unsigned long long v)
{
    char buf[24];
    std::size_t i = sizeof(buf);
    do {
        buf[--i] = static_cast<char>('0' + v % 10U);
        v /= 10U;
    } while (v != 0);
    out.append(buf + i, sizeof(buf) - i);
}

void append_signed(std::string& out, long long v)
{
    if (v < 0) {
        out.push_back('-');
        append_unsigned(out, 0ULL - static_cast<unsigned long long>(v));
    } else {
        append_unsigned(out, static_cast<unsigned long long>(v));
    }
}

// Causa de um std::error_code: Windows (categoria do sistema) FormatMessageW num buffer de pilha,
// sem `ec.message()`; POSIX `ec.message()`. Sempre com o código.
void append_error_code(std::string& out, const std::error_code& ec)
{
#if defined(_WIN32)
    if (ec.category() == std::system_category()) {
        wchar_t buf[512];
        DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                                       FORMAT_MESSAGE_MAX_WIDTH_MASK,
                                   nullptr, static_cast<DWORD>(ec.value()), 0, buf, 512, nullptr);
        while (n > 0 && (buf[n - 1] == L' ' || buf[n - 1] == L'.' || buf[n - 1] == L'\r' ||
                         buf[n - 1] == L'\n'))
            --n;
        if (n > 0)
            append_wide_utf8(out, buf, n);
        else
            out.append("erro desconhecido");
    } else {
        out.append(ec.category().name());
    }
#else
    out.append(ec.message());
#endif
    out.append(" (codigo ");
    append_signed(out, ec.value());
    out.append(")");
}

class DetailBuilder
{
public:
    explicit DetailBuilder(const char* first) : s_(std::make_shared<std::string>(first)) {}

    DetailBuilder& text(const char* t)
    {
        s_->append(t);
        return *this;
    }
    DetailBuilder& view(std::string_view t)
    {
        s_->append(t.data(), t.size());
        return *this;
    }
    DetailBuilder& path(const fs::path& p)
    {
        append_path_text(*s_, p);
        return *this;
    }
    DetailBuilder& number(unsigned long long v)
    {
        append_unsigned(*s_, v);
        return *this;
    }
    DetailBuilder& error(const std::error_code& ec)
    {
        append_error_code(*s_, ec);
        return *this;
    }
    // key_id do envelope em [24, 32) (docs/03 §7.1), em hex minúsculo; "?" se os bytes não bastam.
    DetailBuilder& key_id(std::span<const std::byte> bytes)
    {
        if (bytes.size() < 32) {
            s_->append("?");
            return *this;
        }
        constexpr char kDigits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 8; ++i) {
            const auto b = static_cast<std::uint8_t>(bytes[24 + i]);
            s_->push_back(kDigits[b >> 4]);
            s_->push_back(kDigits[b & 0x0F]);
        }
        return *this;
    }
    [[nodiscard]] FloorDetail done() noexcept { return FloorDetail(std::move(s_)); }

private:
    std::shared_ptr<std::string> s_;
};

// "<categoria>: '<caminho>'" (o resto o chamador acrescenta).
DetailBuilder described(FloorCategory category, const fs::path& p)
{
    DetailBuilder b(to_string(category));
    b.text(": '").path(p).text("'");
    return b;
}

// "<categoria>: '<caminho>'" e, se houver, ": <extra>".
FloorDetail describe(FloorCategory category, const fs::path& p, const char* extra)
{
    DetailBuilder b = described(category, p);
    if (extra != nullptr && *extra != 0) b.text(": ").text(extra);
    return b.done();
}

// Texto sem categoria nem caminho.
FloorDetail make_text(const char* what) { return std::make_shared<const std::string>(what); }

// "<categoria>: '<caminho>': o manifesto instalado e de '<instalado>', esperado '<esperado>'"
FloorDetail describe_mismatch(const fs::path& p, std::string_view installed,
                              std::string_view expected)
{
    DetailBuilder b = described(FloorCategory::GameIdMismatch, p);
    b.text(": o manifesto instalado e de '").view(installed).text("', esperado '");
    b.view(expected).text("'");
    return b.done();
}

// ---------------------------------------------------------------------------------------------
// Verificação de um manifesto (o novo ou o instalado)
// ---------------------------------------------------------------------------------------------

struct Verified
{
    FloorStatus status = FloorStatus::Ok;
    FloorCategory category = FloorCategory::None;
    FloorDetail detail;
    FloorDetail game_id; // só com status Ok
    std::uint64_t build_seq = 0;

    [[nodiscard]] std::string_view id() const noexcept
    {
        return game_id ? std::string_view(*game_id) : std::string_view();
    }
};

bool key_is_trusted(std::span<const std::byte> bytes,
                    std::span<const pack::Ed25519PublicKey> trusted_keys)
{
    if (bytes.size() < 32) return false;
    pack::KeyId wanted{};
    for (std::size_t i = 0; i < wanted.size(); ++i)
        wanted[i] = static_cast<std::uint8_t>(bytes[24 + i]);
    return std::any_of(
        trusted_keys.begin(), trusted_keys.end(),
        [&](const pack::Ed25519PublicKey& k) { return pack::key_id_of(k) == wanted; });
}

// Verifica `bytes` com as trusted_keys atuais (assinatura e §8, SEM piso). `existing` = é a
// cópia instalada (categorias de estado existente) e não o manifesto novo.
//
// `pack::ManifestView view;` é o construtor PADRÃO implícito do sw-format (membro `chunker_` com um
// std::string): a API pública `ManifestView::open(bytes, options, ManifestView& out)` exige um
// objeto já construído, e o sw-format está fora do escopo deste cartão. Se esse construtor é uma
// fronteira de terminate no MSVC Debug é uma SUSPEITA por leitura do código (manifest_view.hpp:71 e
// :220), A MEDIR pelo mapa da varredura; não é afirmação.
Verified verify_manifest(std::span<const std::byte> bytes,
                         std::span<const pack::Ed25519PublicKey> trusted_keys, const fs::path& path,
                         bool existing)
{
    Verified v;
    pack::ManifestView view;
    pack::ReadOptions options;
    options.trusted_keys = trusted_keys;
    const pack::ManifestStatus st = pack::ManifestView::open(bytes, options, view);
    if (st.ok()) {
        const std::string_view id = view.game_id();
        v.game_id = std::make_shared<const std::string>(id.data(), id.size());
        v.build_seq = view.build_seq();
        return v;
    }
    v.status = FloorStatus::Io;
    bool with_key_id = false;
    switch (st.code) {
    case pack::ManifestError::Unsupported:
        v.category = existing ? FloorCategory::Unsupported : FloorCategory::NewManifest;
        break;
    case pack::ManifestError::SignatureInvalid:
        if (!key_is_trusted(bytes, trusted_keys)) {
            v.category = existing ? FloorCategory::UnknownKey : FloorCategory::NewManifest;
            with_key_id = true;
        } else {
            v.category = existing ? FloorCategory::Signature : FloorCategory::NewManifest;
        }
        break;
    case pack::ManifestError::Invalid:
    case pack::ManifestError::Rollback: // impossível sem piso; tratado como defeito do arquivo
    case pack::ManifestError::Ok:
    default: v.category = existing ? FloorCategory::Format : FloorCategory::NewManifest; break;
    }
    DetailBuilder b = described(v.category, path);
    b.text(": ").text(pack::to_string(st.code)).text(" (").text(st.detail).text(")");
    if (with_key_id) b.text("; key_id=").key_id(bytes);
    v.detail = b.done();
    return v;
}

FloorStatus status_of_read(FileReadStatus st) noexcept
{
    return st == FileReadStatus::OutOfMemory ? FloorStatus::OutOfMemory : FloorStatus::Io;
}

FloorCategory category_of_read(FileReadStatus st) noexcept
{
    return st == FileReadStatus::TooLarge ? FloorCategory::Format : FloorCategory::Read;
}

const char* text_of_read(FileReadStatus st) noexcept
{
    switch (st) {
    case FileReadStatus::TooLarge: return "arquivo maior que qualquer manifesto valido";
    case FileReadStatus::OutOfMemory: return "sem memoria para ler o arquivo";
    case FileReadStatus::NotFound: return "arquivo ausente";
    case FileReadStatus::Io:
    case FileReadStatus::Ok:
    default: return "falha de leitura";
    }
}

} // namespace

const char* to_string(FloorCategory category) noexcept
{
    switch (category) {
    case FloorCategory::None: return "nenhuma";
    case FloorCategory::Read: return "leitura";
    case FloorCategory::Format: return "formato";
    case FloorCategory::Signature: return "assinatura";
    case FloorCategory::UnknownKey: return "chave desconhecida";
    case FloorCategory::GameIdMismatch: return "game_id divergente";
    case FloorCategory::Unsupported: return "min_reader nao suportado";
    case FloorCategory::NewManifest: return "manifesto novo nao verificavel";
    case FloorCategory::CreateDir: return "W1 criar installed/";
    case FloorCategory::WriteTmp: return "W2/W3 gravar o .tmp";
    case FloorCategory::VerifyTmp: return "W4 reler o .tmp";
    case FloorCategory::Replace: return "W5 trocar o alvo";
    case FloorCategory::LowerSeq: return "build_seq menor que o instalado";
    }
    return "?";
}

FloorFileOps& real_floor_file_ops() noexcept { return g_real_ops; }

std::string installed_hash_hex(std::string_view game_id)
{
    char buf[kHashHexLen];
    put_hash_hex(buf, game_id);
    return std::string(buf, kHashHexLen);
}

fs::path installed_floor_path(const fs::path& cache_dir, std::string_view game_id)
{
    fs::path p(cache_dir);
    append_floor_file(p, game_id, kFileSuffixNative);
    return p;
}

fs::path installed_tmp_path(const fs::path& cache_dir, std::string_view game_id)
{
    fs::path p(cache_dir);
    append_floor_file(p, game_id, kTmpSuffixNative);
    return p;
}

FloorRead read_installed_floor(const fs::path& cache_dir, std::string_view game_id,
                               std::span<const pack::Ed25519PublicKey> trusted_keys,
                               FloorFileOps& ops) noexcept
{
    // Regra R: `out` só tem enums, optional<inteiro>, bool e shared_ptr: construir, copiar e mover
    // não alocam (nem no MSVC Debug), então nenhum retorno daqui, nem o dos handlers, aloca.
    FloorRead out;
    try {
        if (!pack::crypto_ready()) {
            out.status = FloorStatus::Io;
            out.category = FloorCategory::Format;
            out.detail = make_text("libsodium nao iniciou; o piso nao pode ser verificado");
            return out;
        }
        fs::path path(cache_dir); // cópia: construtor que não é noexcept
        append_floor_file(path, game_id, kFileSuffixNative);
        std::vector<std::byte> bytes(std::size_t{0}); // vector(n): não é noexcept (padrão do F1-63)
        const FileReadStatus rd = ops.read_file(path, kMaxManifestFile, bytes);
        if (rd == FileReadStatus::NotFound) {
            // Primeira instalação deste game_id (única leitura tratada assim: ENOENT). Mas no
            // Windows "installed" ser um arquivo comum também dá PATH_NOT_FOUND: não é ausência.
            fs::path dir(cache_dir);
            append_component(dir, kDirNative);
            std::error_code ec;
            const fs::file_status ds = fs::status(dir, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) {
                out.status = FloorStatus::Io;
                out.category = FloorCategory::Read;
                DetailBuilder b = described(out.category, dir);
                b.text(": ").error(ec);
                out.detail = b.done();
                return out;
            }
            if (!ec && fs::exists(ds) && !fs::is_directory(ds)) {
                out.status = FloorStatus::Io;
                out.category = FloorCategory::Read;
                out.detail = describe(out.category, dir, "installed existe e nao e uma pasta");
                return out;
            }
            out.existing_cache_without_floor =
                cache_file_exists_or_unknown(cache_dir, kIndexNative) ||
                cache_file_exists_or_unknown(cache_dir, kJournalNative);
            return out;
        }
        if (rd != FileReadStatus::Ok) {
            out.status = status_of_read(rd);
            out.category = category_of_read(rd);
            out.detail = describe(out.category, path, text_of_read(rd));
            return out;
        }
        const Verified v = verify_manifest(bytes, trusted_keys, path, /*existing=*/true);
        if (v.status != FloorStatus::Ok) {
            out.status = v.status;
            out.category = v.category;
            out.detail = v.detail;
            return out;
        }
        if (v.id() != game_id) {
            out.status = FloorStatus::Io;
            out.category = FloorCategory::GameIdMismatch;
            out.detail = describe_mismatch(path, v.id(), game_id);
            return out;
        }
        out.floor = v.build_seq;
        return out;
    } catch (const std::bad_alloc&) {
        // Sem texto: montá-lo alocaria. Só atribuições que não alocam.
        out.status = FloorStatus::OutOfMemory;
        out.category = FloorCategory::None;
        out.floor.reset();
        out.existing_cache_without_floor = false;
        out.detail.reset();
        return out;
    } catch (...) {
        out.status = FloorStatus::Io;
        out.category = FloorCategory::Read;
        out.floor.reset();
        out.existing_cache_without_floor = false;
        out.detail.reset();
        return out;
    }
}

FloorWrite commit_installed_floor(const fs::path& cache_dir,
                                  std::span<const std::byte> manifest_bytes,
                                  std::span<const pack::Ed25519PublicKey> trusted_keys,
                                  FloorFileOps& ops) noexcept
{
    // Regra R: ver read_installed_floor. O retorno de sucesso (`out.wrote = true; return out;`),
    // que acontece DEPOIS da troca do arquivo, não aloca em nenhuma configuração: não há
    // std::terminate possível entre a troca e o retorno por construção do tipo (a medir no MSVC
    // Debug; o mapa da varredura é a evidência).
    FloorWrite out;
    try {
        auto fail = [&out](FloorStatus st, FloorCategory cat, FloorDetail detail) -> FloorWrite {
            out.status = st;
            out.category = cat;
            out.detail = std::move(detail);
            return out; // cópia de um shared_ptr: não aloca
        };
        if (!pack::crypto_ready())
            return fail(FloorStatus::Io, FloorCategory::NewManifest,
                        make_text("libsodium nao iniciou; o piso nao pode ser gravado"));

        // Só se grava um manifesto que passou na verificação completa (agora, de novo).
        const Verified fresh = verify_manifest(manifest_bytes, trusted_keys, cache_dir, false);
        if (fresh.status != FloorStatus::Ok)
            return fail(fresh.status, fresh.category, fresh.detail);

        fs::path target(cache_dir);
        append_floor_file(target, fresh.id(), kFileSuffixNative);
        fs::path tmp(cache_dir);
        append_floor_file(tmp, fresh.id(), kTmpSuffixNative);

        // Regra de gravação: ausente, MAIOR, ou IGUAL com bytes diferentes; nunca menor.
        std::vector<std::byte> existing(std::size_t{0});
        const FileReadStatus rd = ops.read_file(target, kMaxManifestFile, existing);
        if (rd == FileReadStatus::Ok) {
            const Verified old = verify_manifest(existing, trusted_keys, target, /*existing=*/true);
            if (old.status != FloorStatus::Ok) return fail(old.status, old.category, old.detail);
            if (old.id() != fresh.id())
                return fail(FloorStatus::Io, FloorCategory::GameIdMismatch,
                            describe_mismatch(target, old.id(), fresh.id()));
            if (fresh.build_seq < old.build_seq) {
                DetailBuilder b = described(FloorCategory::LowerSeq, target);
                b.text(": novo ").number(fresh.build_seq);
                b.text(" < instalado ").number(old.build_seq);
                return fail(FloorStatus::Rollback, FloorCategory::LowerSeq, b.done());
            }
            if (fresh.build_seq == old.build_seq && existing.size() == manifest_bytes.size() &&
                std::memcmp(existing.data(), manifest_bytes.data(), existing.size()) == 0)
                return out; // já é esta cópia: nada a gravar
        } else if (rd != FileReadStatus::NotFound) {
            return fail(status_of_read(rd), category_of_read(rd),
                        describe(category_of_read(rd), target, text_of_read(rd)));
        }

        // W1
        fs::path dir(cache_dir);
        append_component(dir, kDirNative);
        if (ops.create_directory(dir) != CacheErrorCode::Success)
            return fail(FloorStatus::Io, FloorCategory::CreateDir,
                        describe(FloorCategory::CreateDir, dir, "nao foi possivel criar a pasta"));
        // W2 + W3
        const CacheErrorCode wr = ops.write_file_durable(tmp, manifest_bytes);
        if (wr != CacheErrorCode::Success)
            return fail(
                FloorStatus::Io, FloorCategory::WriteTmp,
                describe(FloorCategory::WriteTmp, tmp,
                         wr == CacheErrorCode::DiskFull ? "disco cheio" : "falha de escrita"));
        // W4
        std::vector<std::byte> readback(std::size_t{0});
        const FileReadStatus rb = ops.read_file(tmp, manifest_bytes.size(), readback);
        if (rb != FileReadStatus::Ok)
            return fail(status_of_read(rb), FloorCategory::VerifyTmp,
                        describe(FloorCategory::VerifyTmp, tmp, text_of_read(rb)));
        if (readback.size() != manifest_bytes.size() ||
            std::memcmp(readback.data(), manifest_bytes.data(), readback.size()) != 0)
            return fail(FloorStatus::Io, FloorCategory::VerifyTmp,
                        describe(FloorCategory::VerifyTmp, tmp, "a releitura diverge da memoria"));
        // W5
        if (ops.replace(tmp, target) != CacheErrorCode::Success) {
            out.may_have_changed = true; // ANTES de montar o texto: se faltar memória ali, vale
            return fail(FloorStatus::Io, FloorCategory::Replace,
                        describe(FloorCategory::Replace, target,
                                 "replace_file falhou; o piso pode ter sido atualizado "
                                 "(antigo ou novo)"));
        }
        out.wrote = true;
        return out;
    } catch (const std::bad_alloc&) {
        // Sem texto: montá-lo alocaria. `may_have_changed` fica como está (já pode ser true).
        out.status = FloorStatus::OutOfMemory;
        out.category = FloorCategory::None;
        out.wrote = false;
        out.detail.reset();
        return out;
    } catch (...) {
        out.status = FloorStatus::Io;
        out.category = FloorCategory::Read;
        out.wrote = false;
        out.detail.reset();
        return out;
    }
}

} // namespace swarmstream::detail
