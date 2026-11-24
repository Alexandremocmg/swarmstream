// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Alocadores de teste no formato do `sw_allocator` (F1-67 parte B; F1-60 §7). Usam std::malloc
// direto (não o operator new), para não mexer nos contadores das varreduras de operator new.
//
//   ExactAlignAllocator  devolve ponteiros alinhados EXATAMENTE ao pedido: múltiplos de
//                        `alignment` e NUNCA de 2 x `alignment` (se o código pedisse menos do que o
//                        tipo exige, o ponteiro sairia desalinhado e o teste veria). Guarda, num
//                        cabeçalho antes do bloco, o tamanho e o alinhamento pedidos, e confere no
//                        free_fn que o `size` é o mesmo (divergência reprova). free_fn(NULL)
//                        reprova. Modos: falhar a K-ésima alocação (NULL) e devolver ponteiro
//                        desalinhado.
//   ReissueAllocator     blocos de tamanho fixo; o próximo alloc devolve SEMPRE o último bloco
//                        liberado (pilha), para o teste do limite documentado da quarentena (F1-60
//                        §1.4 teste 2).
#pragma once

#include "check.hpp"
#include "swarmstream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace swarmstream::test {

class ExactAlignAllocator
{
public:
    struct Header
    {
        std::uint64_t magic;
        void* base;
        std::size_t size;
        std::size_t alignment;
    };
    static constexpr std::uint64_t kMagic = 0x5357'4130'4c4c'4f43ull; // "SWA0LLOC"
    static constexpr std::uint64_t kFreed = 0x4652'4545'4446'4545ull;

    sw_allocator as_sw() noexcept { return sw_allocator{&alloc_cb, &free_cb, this}; }

    // 0 = nunca falha; K > 0: a K-ésima chamada de alloc_fn (contada desde arm) devolve NULL.
    void fail_at(std::int64_t k) noexcept
    {
        calls_since_arm_.store(0);
        fail_at_.store(k);
    }
    void set_misaligned(bool on) noexcept { misaligned_.store(on); }

    [[nodiscard]] std::int64_t live() const noexcept { return live_.load(); }
    [[nodiscard]] std::uint64_t alloc_calls() const noexcept { return allocs_.load(); }
    [[nodiscard]] std::uint64_t free_calls() const noexcept { return frees_.load(); }
    [[nodiscard]] std::uint64_t failed_calls() const noexcept { return failed_.load(); }
    // Último par pedido ao alloc_fn (para conferir o alinhamento derivado do tipo).
    [[nodiscard]] std::size_t last_alignment() const noexcept { return last_align_.load(); }
    [[nodiscard]] std::size_t last_size() const noexcept { return last_size_.load(); }
    // Último ponteiro devolvido ao free_fn e o tamanho dele (para o teste de ponteiro desalinhado).
    [[nodiscard]] const void* last_freed() const noexcept { return last_freed_.load(); }
    [[nodiscard]] std::size_t last_freed_size() const noexcept { return last_freed_size_.load(); }

    // Alinhamento e tamanho pedidos para um bloco vivo desta instância.
    static const Header& header_of(const void* p) noexcept
    {
        return *reinterpret_cast<const Header*>(static_cast<const unsigned char*>(p) -
                                                sizeof(Header));
    }

private:
    static void* SW_CALL alloc_cb(void* user, std::size_t size, std::size_t alignment)
    {
        return static_cast<ExactAlignAllocator*>(user)->do_alloc(size, alignment);
    }
    static void SW_CALL free_cb(void* user, void* ptr, std::size_t size)
    {
        static_cast<ExactAlignAllocator*>(user)->do_free(ptr, size);
    }

    void* do_alloc(std::size_t size, std::size_t alignment)
    {
        SW_CHECK(size > 0);
        SW_CHECK(alignment != 0 && (alignment & (alignment - 1)) == 0); // potência de 2 (header)
        last_align_.store(alignment);
        last_size_.store(size);
        const std::int64_t k = fail_at_.load();
        if (k > 0 && calls_since_arm_.fetch_add(1) + 1 == k) {
            failed_.fetch_add(1);
            return nullptr;
        }
        const std::size_t a = alignment < alignof(Header) ? alignof(Header) : alignment;
        // Espaço para: cabeçalho + subir até múltiplo de 2a + a (para ficar múltiplo de a, não 2a).
        const std::size_t total = size + sizeof(Header) + 4 * a + 1;
        auto* base = static_cast<unsigned char*>(std::malloc(total));
        SW_CHECK(base != nullptr);
        std::uintptr_t u = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
        const std::uintptr_t two = static_cast<std::uintptr_t>(2 * a);
        u = (u + two - 1) / two * two + a; // múltiplo de a, nunca de 2a
        if (misaligned_.load()) u += 1;    // modo desalinhado
        auto* p = reinterpret_cast<unsigned char*>(u);
        auto* h = reinterpret_cast<Header*>(p - sizeof(Header));
        // O cabeçalho pode não estar alinhado no modo desalinhado: copiado byte a byte.
        const Header value{kMagic, base, size, alignment};
        std::memcpy(static_cast<void*>(h), &value, sizeof(Header));
        live_.fetch_add(1);
        allocs_.fetch_add(1);
        return p;
    }

    void do_free(void* ptr, std::size_t size)
    {
        SW_CHECK(ptr != nullptr); // nunca free_fn(NULL)
        Header h{};
        std::memcpy(&h, static_cast<unsigned char*>(ptr) - sizeof(Header), sizeof(Header));
        SW_CHECK(h.magic ==
                 kMagic);         // ponteiro desta instância, ainda vivo (sem liberar duas vezes)
        SW_CHECK(h.size == size); // o size do free_fn é o mesmo do alloc_fn
        last_freed_.store(ptr);
        last_freed_size_.store(size);
        h.magic = kFreed;
        std::memcpy(static_cast<unsigned char*>(ptr) - sizeof(Header), &h, sizeof(Header));
        std::free(h.base);
        live_.fetch_sub(1);
        frees_.fetch_add(1);
    }

    std::atomic<std::int64_t> fail_at_{0};
    std::atomic<std::int64_t> calls_since_arm_{0};
    std::atomic<bool> misaligned_{false};
    std::atomic<std::int64_t> live_{0};
    std::atomic<std::uint64_t> allocs_{0};
    std::atomic<std::uint64_t> frees_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::size_t> last_align_{0};
    std::atomic<std::size_t> last_size_{0};
    std::atomic<const void*> last_freed_{nullptr};
    std::atomic<std::size_t> last_freed_size_{0};
};

// Blocos de tamanho fixo (`block_size`, alinhados a 64); o alloc devolve o ÚLTIMO bloco liberado,
// se houver. Nunca devolve memória ao sistema antes do destrutor.
class ReissueAllocator
{
public:
    explicit ReissueAllocator(std::size_t block_size) noexcept : block_size_(block_size) {}
    ~ReissueAllocator()
    {
        for (std::size_t i = 0; i < all_count_; ++i)
            std::free(all_[i]);
    }
    ReissueAllocator(const ReissueAllocator&) = delete;
    ReissueAllocator& operator=(const ReissueAllocator&) = delete;

    sw_allocator as_sw() noexcept { return sw_allocator{&alloc_cb, &free_cb, this}; }
    [[nodiscard]] std::int64_t live() const noexcept { return live_; }
    [[nodiscard]] const void* last_issued() const noexcept { return last_issued_; }

private:
    static constexpr std::size_t kMax = 4096;
    static void* SW_CALL alloc_cb(void* user, std::size_t size, std::size_t alignment)
    {
        return static_cast<ReissueAllocator*>(user)->do_alloc(size, alignment);
    }
    static void SW_CALL free_cb(void* user, void* ptr, std::size_t size)
    {
        static_cast<ReissueAllocator*>(user)->do_free(ptr, size);
    }
    void* do_alloc(std::size_t size, std::size_t alignment)
    {
        std::lock_guard<std::mutex> g(m_);
        SW_CHECK(size <= block_size_);
        SW_CHECK(alignment <= 64);
        void* p = nullptr;
        if (free_count_ > 0) {
            p = free_[--free_count_];
        } else {
            SW_CHECK(all_count_ < kMax);
            void* raw = std::malloc(block_size_ + 64);
            SW_CHECK(raw != nullptr);
            all_[all_count_++] = raw;
            const std::uintptr_t u = (reinterpret_cast<std::uintptr_t>(raw) + 63) / 64 * 64;
            p = reinterpret_cast<void*>(u);
        }
        ++live_;
        last_issued_ = p;
        return p;
    }
    void do_free(void* ptr, std::size_t size)
    {
        std::lock_guard<std::mutex> g(m_);
        SW_CHECK(ptr != nullptr);
        SW_CHECK(size <= block_size_);
        SW_CHECK(free_count_ < kMax);
        free_[free_count_++] = ptr;
        --live_;
    }

    std::mutex m_;
    std::size_t block_size_;
    void* all_[kMax] = {};
    std::size_t all_count_ = 0;
    void* free_[kMax] = {};
    std::size_t free_count_ = 0;
    std::int64_t live_ = 0;
    const void* last_issued_ = nullptr;
};

} // namespace swarmstream::test
