// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "installed_floor.hpp"

#include "platform_file.hpp"

#include <swarmstream/crypto.hpp>
#include <swarmstream/manifest_view.hpp>
#include <swarmstream/sha256.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

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
            const DWORD chunk = left > (DWORD{1} << 30) ? (DWORD{1} << 30) : static_cast<DWORD>(left);
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
        return replace_file(from, to);
    }
};

// ---------------------------------------------------------------------------------------------
// Verificação de um manifesto (o novo ou o instalado)
// ---------------------------------------------------------------------------------------------

struct Verified
{
    FloorStatus status = FloorStatus::Ok;
    FloorCategory category = FloorCategory::None;
    std::string detail;
    std::string game_id;
    std::uint64_t build_seq = 0;
};

std::string key_id_hex(std::span<const std::byte> bytes)
{
    // Envelope: key_id em [24, 32) (docs/03 §7.1).
    if (bytes.size() < 32) return "?";
    std::array<std::uint8_t, 8> id{};
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(bytes[24 + i]);
    return pack::to_hex(id);
}

bool key_is_trusted(std::span<const std::byte> bytes,
                    std::span<const pack::Ed25519PublicKey> trusted_keys)
{
    if (bytes.size() < 32) return false;
    pack::KeyId wanted{};
    for (std::size_t i = 0; i < wanted.size(); ++i)
        wanted[i] = static_cast<std::uint8_t>(bytes[24 + i]);
    return std::any_of(trusted_keys.begin(), trusted_keys.end(),
                       [&](const pack::Ed25519PublicKey& k) { return pack::key_id_of(k) == wanted; });
}

std::string describe(FloorCategory category, const fs::path& path, const std::string& extra)
{
    std::string text = to_string(category);
    text += ": '" + path.string() + "'";
    if (!extra.empty()) text += ": " + extra;
    return text;
}

// Verifica `bytes` com as trusted_keys atuais (assinatura e §8, SEM piso). `existing` = é a
// cópia instalada (categorias de estado existente) e não o manifesto novo.
Verified verify_manifest(std::span<const std::byte> bytes,
                         std::span<const pack::Ed25519PublicKey> trusted_keys,
                         const fs::path& path, bool existing)
{
    Verified v;
    pack::ManifestView view;
    pack::ReadOptions options;
    options.trusted_keys = trusted_keys;
    const pack::ManifestStatus st = pack::ManifestView::open(bytes, options, view);
    if (st.ok()) {
        v.game_id.assign(view.game_id());
        v.build_seq = view.build_seq();
        return v;
    }
    v.status = FloorStatus::Io;
    std::string extra = std::string(pack::to_string(st.code)) + " (" + st.detail + ")";
    switch (st.code) {
    case pack::ManifestError::Unsupported:
        v.category = existing ? FloorCategory::Unsupported : FloorCategory::NewManifest;
        break;
    case pack::ManifestError::SignatureInvalid:
        if (!key_is_trusted(bytes, trusted_keys)) {
            v.category = existing ? FloorCategory::UnknownKey : FloorCategory::NewManifest;
            extra += "; key_id=" + key_id_hex(bytes);
        } else {
            v.category = existing ? FloorCategory::Signature : FloorCategory::NewManifest;
        }
        break;
    case pack::ManifestError::Invalid:
    case pack::ManifestError::Rollback: // impossível sem piso; tratado como defeito do arquivo
    case pack::ManifestError::Ok:
    default:
        v.category = existing ? FloorCategory::Format : FloorCategory::NewManifest;
        break;
    }
    v.detail = describe(v.category, path, extra);
    return v;
}

bool path_exists_or_unknown(const fs::path& p)
{
    std::error_code ec;
    const bool e = fs::exists(p, ec);
    return ec ? true : e; // em dúvida, avisa
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

FloorFileOps& real_floor_file_ops() noexcept
{
    static RealFloorFileOps ops;
    return ops;
}

std::string installed_hash_hex(std::string_view game_id)
{
    const auto* p = reinterpret_cast<const std::byte*>(game_id.data());
    return pack::to_hex(pack::sha256(std::span<const std::byte>(p, game_id.size())));
}

fs::path installed_floor_path(const fs::path& cache_dir, std::string_view game_id)
{
    return cache_dir / kInstalledDirName / (installed_hash_hex(game_id) + kInstalledFileSuffix);
}

fs::path installed_tmp_path(const fs::path& cache_dir, std::string_view game_id)
{
    return cache_dir / kInstalledDirName / (installed_hash_hex(game_id) + kInstalledTmpSuffix);
}

FloorRead read_installed_floor(const fs::path& cache_dir, std::string_view game_id,
                               std::span<const pack::Ed25519PublicKey> trusted_keys,
                               FloorFileOps& ops) noexcept
{
    FloorRead out;
    try {
        if (!pack::crypto_ready()) {
            out.status = FloorStatus::Io;
            out.category = FloorCategory::Format;
            out.detail = "libsodium nao iniciou; o piso nao pode ser verificado";
            return out;
        }
        const fs::path path = installed_floor_path(cache_dir, game_id);
        std::vector<std::byte> bytes;
        const FileReadStatus rd = ops.read_file(path, kMaxManifestFile, bytes);
        if (rd == FileReadStatus::NotFound) {
            // Primeira instalação deste game_id (única leitura tratada assim: ENOENT). Mas no
            // Windows "installed" ser um arquivo comum também dá PATH_NOT_FOUND: não é ausência.
            const fs::path dir = cache_dir / kInstalledDirName;
            std::error_code ec;
            const fs::file_status ds = fs::status(dir, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) {
                out.status = FloorStatus::Io;
                out.category = FloorCategory::Read;
                out.detail = describe(out.category, dir, ec.message());
                return out;
            }
            if (!ec && fs::exists(ds) && !fs::is_directory(ds)) {
                out.status = FloorStatus::Io;
                out.category = FloorCategory::Read;
                out.detail = describe(out.category, dir, "installed existe e nao e uma pasta");
                return out;
            }
            out.existing_cache_without_floor =
                path_exists_or_unknown(cache_dir / "index.bin") ||
                path_exists_or_unknown(cache_dir / "journal.bin");
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
        if (v.game_id != game_id) {
            out.status = FloorStatus::Io;
            out.category = FloorCategory::GameIdMismatch;
            out.detail = describe(out.category, path,
                                  "o manifesto instalado e de '" + v.game_id + "', esperado '" +
                                      std::string(game_id) + "'");
            return out;
        }
        out.floor = v.build_seq;
        return out;
    } catch (const std::bad_alloc&) {
        FloorRead oom;
        oom.status = FloorStatus::OutOfMemory;
        return oom; // sem texto: montá-lo alocaria
    } catch (...) {
        FloorRead bad;
        bad.status = FloorStatus::Io;
        bad.category = FloorCategory::Read;
        return bad;
    }
}

FloorWrite commit_installed_floor(const fs::path& cache_dir,
                                  std::span<const std::byte> manifest_bytes,
                                  std::span<const pack::Ed25519PublicKey> trusted_keys,
                                  FloorFileOps& ops) noexcept
{
    FloorWrite out;
    try {
        auto fail = [&out](FloorStatus st, FloorCategory cat, std::string detail) -> FloorWrite {
            out.status = st;
            out.category = cat;
            out.detail = std::move(detail);
            return out;
        };
        if (!pack::crypto_ready())
            return fail(FloorStatus::Io, FloorCategory::NewManifest,
                        "libsodium nao iniciou; o piso nao pode ser gravado");

        // Só se grava um manifesto que passou na verificação completa (agora, de novo).
        const Verified fresh = verify_manifest(manifest_bytes, trusted_keys, cache_dir, false);
        if (fresh.status != FloorStatus::Ok) return fail(fresh.status, fresh.category, fresh.detail);

        const fs::path target = installed_floor_path(cache_dir, fresh.game_id);
        const fs::path tmp = installed_tmp_path(cache_dir, fresh.game_id);

        // Regra de gravação: ausente, MAIOR, ou IGUAL com bytes diferentes; nunca menor.
        std::vector<std::byte> existing;
        const FileReadStatus rd = ops.read_file(target, kMaxManifestFile, existing);
        if (rd == FileReadStatus::Ok) {
            const Verified old = verify_manifest(existing, trusted_keys, target, /*existing=*/true);
            if (old.status != FloorStatus::Ok) return fail(old.status, old.category, old.detail);
            if (old.game_id != fresh.game_id)
                return fail(FloorStatus::Io, FloorCategory::GameIdMismatch,
                            describe(FloorCategory::GameIdMismatch, target,
                                     "o manifesto instalado e de '" + old.game_id + "', esperado '" +
                                         fresh.game_id + "'"));
            if (fresh.build_seq < old.build_seq)
                return fail(FloorStatus::Rollback, FloorCategory::LowerSeq,
                            describe(FloorCategory::LowerSeq, target,
                                     "novo " + std::to_string(fresh.build_seq) + " < instalado " +
                                         std::to_string(old.build_seq)));
            if (fresh.build_seq == old.build_seq && existing.size() == manifest_bytes.size() &&
                std::memcmp(existing.data(), manifest_bytes.data(), existing.size()) == 0)
                return out; // já é esta cópia: nada a gravar
        } else if (rd != FileReadStatus::NotFound) {
            return fail(status_of_read(rd), category_of_read(rd),
                        describe(category_of_read(rd), target, text_of_read(rd)));
        }

        // W1
        const fs::path dir = cache_dir / kInstalledDirName;
        if (ops.create_directory(dir) != CacheErrorCode::Success)
            return fail(FloorStatus::Io, FloorCategory::CreateDir,
                        describe(FloorCategory::CreateDir, dir, "nao foi possivel criar a pasta"));
        // W2 + W3
        const CacheErrorCode wr = ops.write_file_durable(tmp, manifest_bytes);
        if (wr != CacheErrorCode::Success)
            return fail(FloorStatus::Io, FloorCategory::WriteTmp,
                        describe(FloorCategory::WriteTmp, tmp,
                                 wr == CacheErrorCode::DiskFull ? "disco cheio" : "falha de escrita"));
        // W4
        std::vector<std::byte> readback;
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
            out.may_have_changed = true;
            return fail(FloorStatus::Io, FloorCategory::Replace,
                        describe(FloorCategory::Replace, target,
                                 "replace_file falhou; o piso pode ter sido atualizado "
                                 "(antigo ou novo)"));
        }
        out.wrote = true;
        return out;
    } catch (const std::bad_alloc&) {
        FloorWrite oom;
        oom.status = FloorStatus::OutOfMemory;
        oom.may_have_changed = out.may_have_changed;
        return oom; // sem texto: montá-lo alocaria
    } catch (...) {
        FloorWrite bad;
        bad.status = FloorStatus::Io;
        bad.category = FloorCategory::Read;
        bad.may_have_changed = out.may_have_changed;
        return bad;
    }
}

} // namespace swarmstream::detail
