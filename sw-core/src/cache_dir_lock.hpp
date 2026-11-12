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
#include <memory>
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

// Caractere nativo dos caminhos do SO (wchar_t no Windows, char no POSIX).
#if defined(_WIN32)
using NativeChar = wchar_t;
#else
using NativeChar = char;
#endif

inline constexpr const char* kCacheLockFileName = "swarmstream.lock";

enum class LockCode : std::uint8_t {
    Ok = 0,
    Locked, // outra abertura (outro processo ou outro lock deste) segura a pasta
    Io, // qualquer outro erro do SO (permissão, disco somente leitura, pasta que não pôde ser
        // criada...)
    OutOfMemory, // falta de memória montando o caminho ou a mensagem
};

// Resultado de acquire()/release(). NÃO tem std::string inline de propósito: o Ok (e qualquer
// retorno por valor) não aloca em nenhuma configuração, nem no MSVC em Debug, onde construir ou
// mover uma std::string aloca o proxy dos iteradores dentro de um construtor `noexcept`. Isso
// importa porque o Ok é devolvido DEPOIS de o lock estar tomado: nada que possa falhar por falta
// de memória pode ficar depois da tomada do lock. O texto (só nas falhas) vai num shared_ptr.
struct LockStatus
{
    LockCode code = LockCode::Ok;
    int os_error = 0; // errno / GetLastError, 0 se não houve
    // Diagnóstico para o log_fn (operação, caminho, causa). Nulo em Ok e em OutOfMemory.
    std::shared_ptr<const std::string> detail;

    [[nodiscard]] bool ok() const noexcept { return code == LockCode::Ok; }
    // Texto do diagnóstico; "" se não há.
    [[nodiscard]] const char* message() const noexcept { return detail ? detail->c_str() : ""; }
};

#if defined(_WIN32)
// Monta "<cache_dir><separador><swarmstream.lock>" num buffer NUL-terminado (um único `new[]`) e
// devolve em `dir_len` o tamanho de <cache_dir>. Só COMPÕE o texto: nenhuma chamada ao SO. Existe
// em separado para os testes cobrirem a composição (UNC, prefixo estendido, barras, "C:") sem E/S.
// `cache_dir` não pode ser vazio. Lança std::bad_alloc se faltar memória.
[[nodiscard]] std::unique_ptr<NativeChar[]>
compose_lock_path(const std::filesystem::path& cache_dir, std::size_t& dir_len);
#endif

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
    // Caminho de `swarmstream.lock`, NUL-terminado, no formato nativo do SO (wchar_t no Windows,
    // char no POSIX); "" sem lock. VIDA ÚTIL: o ponteiro pertence ao CacheDirLock e vale enquanto
    // ele segurar o lock (held()): release(), uma nova aquisição, mover DE este objeto e a
    // destruição o invalidam; mover PARA um objeto o leva junto com o buffer (o ponteiro lido do
    // objeto de origem continua valendo, agora do destino). Não é uma cópia: quem precisa dele
    // depois copia o texto. É ponteiro, e não `const fs::path&`, para que nada global (com
    // construtor que aloca na inicialização estática) seja necessário para devolver o caminho
    // vazio.
    [[nodiscard]] const NativeChar* native_path() const noexcept;

private:
#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int h_ = -1;
#endif
    bool held_ = false;
    // Em ponteiro para que mover o CacheDirLock seja só mover um ponteiro (noexcept de verdade:
    // mover um fs::path/std::string no MSVC em Debug aloca) e para que o caminho seja alocado
    // ANTES de o arquivo ser aberto. Nulo sem lock.
#if defined(_WIN32)
    // Buffer de wchar_t montado com um único `new[]` (cache_dir + separador + nome): no MSVC em
    // Debug os construtores de std::wstring/fs::path são noexcept e alocam o proxy dos iteradores,
    // então uma falta de memória ali é std::terminate (medido no CI, K=4 e K=5).
    std::unique_ptr<NativeChar[]> lock_path_;
#else
    std::unique_ptr<std::filesystem::path> lock_path_;
#endif
};

} // namespace swarmstream::detail
