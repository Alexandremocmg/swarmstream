// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Alocador do contexto da API C (F1-67 parte B; F1-60 §7, aprovado pelo dono em 2026-11-02).
// Interno do sw-core: nada aqui é exportado nem faz parte do swarmstream.h.
//
// DOIS NÍVEIS (a lista vale para a documentação ao integrador: "o alocador cobre as alocações
// grandes e de vida longa do contexto; o resto usa o sistema"; não prometer mais que o nível 1):
//   Nível 1, alocador do CHAMADOR (`sw_config.allocator`, ou o do sistema se vier NULL):
//     o `Context`; as cópias de strings, chaves e URLs da `sw_config`; os bytes do manifesto; as
//     cascas de `sw_file`/`sw_dir` e a quarentena delas; as estruturas pesadas dos handles; o pool
//     de pedidos, a tabela hash dele e a fila de conclusão (`RequestRegistry`, F1-67 B); o pool de
//     256 KiB x N do cache de RAM.
//   Nível 2, EXCEÇÃO DECLARADA (alocador do sistema, nunca o do chamador):
//     contêineres internos de `ManifestView` e `Vfs`; `CacheOwner`, índice, journal, alocador de
//     slots; `TelemetryLog`; agendador e fonte (etapa 2); `std::filesystem::path`, `std::string`
//     de log; pilhas e estado das threads (inclusive as threads de conclusão); a tabela do
//     `HandleRegistry` e as cascas de `sw_ctx` (são do PROCESSO, compartilhadas por contextos com
//     alocadores diferentes, F1-60 §1.3); a libsodium.
//
// ALINHAMENTO PEDIDO (derivado do tipo real, não de constantes soltas; cada ponto de alocação tem
// `static_assert(alignof(T) <= pedido)`):
//   objetos                  max(alignof(T), alignof(std::max_align_t))   (kObjectAlign<T>)
//   pool de pedidos e filas   max(alignof(T), 64)  (linha de cache)          (kCacheLineAlign<T>)
//   pool do cache de RAM      4096 (casa com o alinhamento de slot do docs/04) (kRamPoolAlign)
//   bytes do manifesto        alignof(std::max_align_t)                     (kBytesAlign)
//
// CONTRATO DO sw_allocator (header): os dois ponteiros juntos ou nenhum; thread-safe; `alignment`
// é potência de 2; `size` no free_fn é o mesmo do alloc_fn. Este wrapper garante, do lado da
// biblioteca: nunca chama alloc_fn com tamanho 0 nem alinhamento que não seja potência de 2; nunca
// chama free_fn(NULL); o `size` do free_fn é SEMPRE o do alloc_fn correspondente. Do lado do
// chamador ele confere o que dá para conferir: um alloc_fn que devolve ponteiro DESALINHADO tem o
// bloco devolvido na hora ao free_fn (mesmo tamanho) e a alocação falha com `Misaligned` (o
// sw_init, parte C, mapeia para SW_E_OUT_OF_MEMORY e registra ERROR no log_fn).
//
// Nada aqui lança exceção nem aloca por conta própria (além do bloco pedido). Não há estado
// global: um ContextAllocator é uma cópia de três ponteiros.
#pragma once

#include "swarmstream.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace swarmstream::detail {

template <class T>
inline constexpr std::size_t kObjectAlign = alignof(T) > alignof(std::max_align_t)
                                                ? alignof(T)
                                                : alignof(std::max_align_t);

template <class T>
inline constexpr std::size_t kCacheLineAlign = alignof(T) > 64 ? alignof(T) : std::size_t{64};

inline constexpr std::size_t kRamPoolAlign = 4096;
inline constexpr std::size_t kBytesAlign = alignof(std::max_align_t);

enum class AllocStatus : std::uint8_t {
    Ok = 0,
    OutOfMemory, // alloc_fn devolveu NULL (ou o sistema não tinha memória), ou tamanho estoura
    Misaligned,  // alloc_fn devolveu um ponteiro que não é múltiplo do alinhamento pedido
    InvalidArgument, // tamanho 0 ou alinhamento que não é potência de 2 (erro de uso interno)
};

[[nodiscard]] const char* to_string(AllocStatus status) noexcept;

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}

class ContextAllocator
{
public:
    // Alocador do sistema (`operator new` alinhado, nothrow).
    constexpr ContextAllocator() noexcept = default;

    // O par do sw_config. PRECONDIÇÃO: `valid_pair(a)` (o sw_init confere antes e devolve
    // SW_E_INVALID_ARGUMENT se não for). Os dois NULL = alocador do sistema.
    explicit ContextAllocator(const sw_allocator& a) noexcept
        : alloc_fn_(a.alloc_fn), free_fn_(a.free_fn), user_(a.user)
    {
    }

    // Os dois ponteiros juntos ou nenhum (header).
    [[nodiscard]] static bool valid_pair(const sw_allocator& a) noexcept
    {
        return (a.alloc_fn == nullptr) == (a.free_fn == nullptr);
    }

    [[nodiscard]] bool uses_caller_allocator() const noexcept { return alloc_fn_ != nullptr; }

    // Bloco cru de `size` bytes alinhado a `alignment`. nullptr em falha (motivo em `*status` se
    // não for nulo). Não constrói nada.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment,
                                 AllocStatus* status = nullptr) const noexcept;

    // Devolve um bloco de `allocate`. `size` e `alignment` têm de ser os mesmos da alocação.
    // nullptr: nada a fazer (nunca chama free_fn(NULL)).
    void deallocate(void* p, std::size_t size, std::size_t alignment) const noexcept;

    // Um objeto T com o alinhamento de objeto (kObjectAlign<T>), ou outro maior (`alignment`).
    // T precisa de construtor noexcept: nada aqui captura exceção.
    template <class T, class... Args>
    [[nodiscard]] T* create_aligned(std::size_t alignment, AllocStatus* status,
                                    Args&&... args) const noexcept
    {
        static_assert(std::is_nothrow_constructible_v<T, Args...>,
                      "ContextAllocator::create: o construtor de T tem de ser noexcept");
        static_assert(alignof(T) <= kObjectAlign<T>);
        if (alignment < alignof(T) || !is_power_of_two(alignment)) {
            if (status != nullptr) *status = AllocStatus::InvalidArgument;
            return nullptr;
        }
        void* p = allocate(sizeof(T), alignment, status);
        if (p == nullptr) return nullptr;
        return ::new (p) T(std::forward<Args>(args)...);
    }

    template <class T, class... Args>
    [[nodiscard]] T* create(AllocStatus* status, Args&&... args) const noexcept
    {
        return create_aligned<T>(kObjectAlign<T>, status, std::forward<Args>(args)...);
    }

    // Destrói e devolve um objeto de create_aligned (mesmo `alignment`). nullptr: nada.
    template <class T>
    void destroy_aligned(T* p, std::size_t alignment) const noexcept
    {
        static_assert(std::is_nothrow_destructible_v<T>);
        if (p == nullptr) return;
        p->~T();
        deallocate(p, sizeof(T), alignment);
    }

    template <class T>
    void destroy(T* p) const noexcept
    {
        destroy_aligned(p, kObjectAlign<T>);
    }

    // `n` objetos T construídos por valor (T{}), contíguos, alinhados a `alignment`. n == 0 ou
    // n * sizeof(T) que estoura size_t => nullptr (OutOfMemory / InvalidArgument).
    template <class T>
    [[nodiscard]] T* create_array(std::size_t n, std::size_t alignment,
                                  AllocStatus* status) const noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        if (n == 0 || alignment < alignof(T) || !is_power_of_two(alignment)) {
            if (status != nullptr) *status = AllocStatus::InvalidArgument;
            return nullptr;
        }
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
            if (status != nullptr) *status = AllocStatus::OutOfMemory;
            return nullptr;
        }
        void* p = allocate(n * sizeof(T), alignment, status);
        if (p == nullptr) return nullptr;
        T* first = static_cast<T*>(p);
        for (std::size_t i = 0; i < n; ++i)
            ::new (static_cast<void*>(first + i)) T();
        return first;
    }

    template <class T>
    void destroy_array(T* p, std::size_t n, std::size_t alignment) const noexcept
    {
        static_assert(std::is_nothrow_destructible_v<T>);
        if (p == nullptr) return;
        for (std::size_t i = n; i > 0; --i)
            p[i - 1].~T();
        deallocate(p, n * sizeof(T), alignment);
    }

private:
    void*(SW_CALL* alloc_fn_)(void*, std::size_t, std::size_t) = nullptr;
    void(SW_CALL* free_fn_)(void*, void*, std::size_t) = nullptr;
    void* user_ = nullptr;
};

} // namespace swarmstream::detail
