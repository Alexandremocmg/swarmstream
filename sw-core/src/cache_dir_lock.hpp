// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Lock exclusivo da pasta de cache entre processos (F1-69; ADR-0011, F1-60 §6.1). Interno do
// sw-core: ainda não está ligado ao sw_init (isso é do F1-67), e nada aqui é exportado.
//
// A posse é o lock do sistema operacional sobre a ABERTURA de `swarmstream.lock`, na pasta de
// cache; a existência do arquivo não significa nada:
//   - Windows: CreateFileW (handle NÃO herdável) + LockFileEx(EXCLUSIVE | FAIL_IMMEDIATELY);
//   - POSIX:   open(O_CLOEXEC) + flock(LOCK_EX | LOCK_NB).
// As duas travam por abertura, então um segundo acquire() no MESMO processo também colide (o
// fcntl clássico não serviria: é por processo e some quando qualquer descritor é fechado).
// O SO solta o lock se o processo morre. O arquivo é vazio, sem PID, e NUNCA é apagado: nem
// release(), nem falha, nem recuperação. Um arquivo sem dono não impede nada.
//
// Antes da aquisição só se cria a pasta (se faltar) e o próprio arquivo de lock: nenhum outro
// arquivo da pasta é aberto, lido ou escrito.
//
// Limites (docs/04): `flock` em NFS não é confiável (a pasta de cache tem de ser local); no POSIX
// um filho criado com fork() SEM exec() compartilha a abertura e portanto o lock (com exec() o
// O_CLOEXEC fecha o descritor). release() chama flock(LOCK_UN) antes de fechar, então solta o
// lock mesmo se um filho assim ainda existir.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace swarmstream::detail {

inline constexpr const char* kCacheLockFileName = "swarmstream.lock";

enum class LockCode : std::uint8_t {
    Ok = 0,
    Locked,      // outra abertura (outro processo ou outro lock deste) segura a pasta
    Io,          // qualquer outro erro do SO (permissão, disco somente leitura, pasta que não pôde ser criada...)
    OutOfMemory, // falta de memória montando o caminho ou a mensagem
};

struct LockStatus
{
    LockCode code = LockCode::Ok;
    int os_error = 0;   // errno / GetLastError, 0 se não houve
    std::string detail; // diagnóstico para o log_fn (operação, caminho, causa); vazio em Ok

    [[nodiscard]] bool ok() const noexcept { return code == LockCode::Ok; }
};

class CacheDirLock
{
public:
    CacheDirLock() noexcept = default;
    CacheDirLock(const CacheDirLock&) = delete;
    CacheDirLock& operator=(const CacheDirLock&) = delete;
    CacheDirLock(CacheDirLock&& o) noexcept;
    CacheDirLock& operator=(CacheDirLock&& o) noexcept;
    // Último recurso (solta o lock se ainda o segura): quem quer o erro chama release().
    ~CacheDirLock();

    // Cria `cache_dir` se faltar, abre/cria `swarmstream.lock` e tenta o lock sem esperar.
    // Em qualquer falha `out` fica sem lock e o handle aberto é fechado. `out` não pode já
    // segurar um lock (uso incorreto => Io, sem tocar na pasta).
    [[nodiscard]] static LockStatus acquire(const std::filesystem::path& cache_dir,
                                            CacheDirLock& out) noexcept;

    // Solta o lock e fecha o arquivo (que fica na pasta). O objeto é dado por solto mesmo se
    // uma chamada falhar. Sem lock seguro: Ok.
    [[nodiscard]] LockStatus release() noexcept;

    [[nodiscard]] bool held() const noexcept { return held_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return lock_path_; }

private:
#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int h_ = -1;
#endif
    bool held_ = false;
    std::filesystem::path lock_path_;
};

} // namespace swarmstream::detail
