// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-67 parte B: alocador do contexto em dois níveis (F1-60 §7, testes 1, 2 e 8 no componente).
// Os testes 3 a 7 do §7 são da API (partes C e D): varredura de alloc_fn no sw_init/sw_open/
// sw_dir_open, vivos == 0 depois do sw_shutdown, nada depois do retorno, par incompleto e caminho
// quente. Aqui: alinhamento EXATO pelos tipos reais (inclusive sobrealinhados de 64 e 4096),
// ponteiro desalinhado devolvido ao free_fn, tamanho igual no free_fn, nenhum free_fn(NULL),
// falha do alloc_fn, alocador do sistema e 16 threads (o job TSan roda este teste).

#include "api_allocator.hpp"
#include "check.hpp"
#include "handle_registry.hpp"
#include "request_registry.hpp"
#include "test_allocators.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using swarmstream::detail::AllocStatus;
using swarmstream::detail::ContextAllocator;
using swarmstream::detail::kBytesAlign;
using swarmstream::detail::kCacheLineAlign;
using swarmstream::detail::kObjectAlign;
using swarmstream::detail::kRamPoolAlign;
using swarmstream::detail::kRequestPoolAlign;
using swarmstream::test::ExactAlignAllocator;

struct Plain
{
    int a = 1;
    char b = 2;
};
struct alignas(64) Over64
{
    unsigned char bytes[100] = {};
};
struct alignas(4096) Over4096
{
    unsigned char bytes[10] = {};
};

bool aligned_to(const void* p, std::size_t a)
{
    return reinterpret_cast<std::uintptr_t>(p) % a == 0;
}

template <class T>
void check_object_alignment(ExactAlignAllocator& ea, const ContextAllocator& ca, const char* name)
{
    AllocStatus st = AllocStatus::Ok;
    T* p = ca.create<T>(&st);
    SW_CHECK(p != nullptr);
    SW_CHECK(st == AllocStatus::Ok);
    const std::size_t asked = ExactAlignAllocator::header_of(p).alignment;
    // Pedido derivado do tipo: max(alignof(T), alignof(max_align_t)), potência de 2, >= alignof(T).
    SW_CHECK(asked == kObjectAlign<T>);
    SW_CHECK(swarmstream::detail::is_power_of_two(asked));
    SW_CHECK(asked >= alignof(T));
    SW_CHECK(aligned_to(p, alignof(T)));
    // O alocador de teste devolve exatamente o pedido (nunca o dobro): controle de que um pedido
    // menor que o tipo daria ponteiro desalinhado e seria visto.
    SW_CHECK(!aligned_to(p, 2 * asked));
    SW_CHECK(ExactAlignAllocator::header_of(p).size == sizeof(T));
    ca.destroy(p);
    SW_CHECK(ea.live() == 0);
    std::printf("  %-28s alignof=%zu pedido=%zu sizeof=%zu\n", name, alignof(T), asked, sizeof(T));
}

void test_valid_pair()
{
    auto noop_alloc = [](void*, std::size_t, std::size_t) -> void* { return nullptr; };
    auto noop_free = [](void*, void*, std::size_t) {};
    sw_allocator both{+noop_alloc, +noop_free, nullptr};
    sw_allocator none{nullptr, nullptr, nullptr};
    sw_allocator only_alloc{+noop_alloc, nullptr, nullptr};
    sw_allocator only_free{nullptr, +noop_free, nullptr};
    SW_CHECK(ContextAllocator::valid_pair(both));
    SW_CHECK(ContextAllocator::valid_pair(none));
    SW_CHECK(!ContextAllocator::valid_pair(only_alloc)); // par incompleto (o sw_init: INVALID_ARG)
    SW_CHECK(!ContextAllocator::valid_pair(only_free));
    SW_CHECK(!ContextAllocator(none).uses_caller_allocator());
    SW_CHECK(ContextAllocator(both).uses_caller_allocator());
    std::puts("ok: par alloc_fn/free_fn (os dois ou nenhum)");
}

void test_alignment_by_real_types()
{
    ExactAlignAllocator ea;
    const ContextAllocator ca(ea.as_sw());
    check_object_alignment<Plain>(ea, ca, "Plain");
    check_object_alignment<Over64>(ea, ca, "alignas(64)");
    check_object_alignment<Over4096>(ea, ca, "alignas(4096)");
    check_object_alignment<swarmstream::detail::PinCore>(ea, ca, "PinCore");
    check_object_alignment<swarmstream::detail::FileRequests>(ea, ca, "FileRequests");
    check_object_alignment<sw_file>(ea, ca, "sw_file (casca)");
    check_object_alignment<sw_dir>(ea, ca, "sw_dir (casca)");

    // Pool de pedidos: max(alignof(Request), 64) (linha de cache).
    AllocStatus st = AllocStatus::Ok;
    constexpr std::size_t n = 7;
    auto* pool = ca.create_array<swarmstream::detail::Request>(n, kRequestPoolAlign, &st);
    SW_CHECK(pool != nullptr);
    SW_CHECK(kRequestPoolAlign == kCacheLineAlign<swarmstream::detail::Request>);
    SW_CHECK(kRequestPoolAlign >= 64 && kRequestPoolAlign >= alignof(swarmstream::detail::Request));
    SW_CHECK(ExactAlignAllocator::header_of(pool).alignment == kRequestPoolAlign);
    SW_CHECK(ExactAlignAllocator::header_of(pool).size == n * sizeof(swarmstream::detail::Request));
    SW_CHECK(aligned_to(pool, kRequestPoolAlign));
    SW_CHECK(!aligned_to(pool, 2 * kRequestPoolAlign));
    for (std::size_t i = 0; i < n; ++i) {
        SW_CHECK(pool[i].state.load() == swarmstream::detail::RequestState::Free); // construído
    }
    ca.destroy_array(pool, n, kRequestPoolAlign);

    // Sobrealinhado no pool (linha de cache maior que 64 vem do tipo).
    auto* big = ca.create_array<Over4096>(3, kCacheLineAlign<Over4096>, &st);
    SW_CHECK(big != nullptr);
    SW_CHECK(ExactAlignAllocator::header_of(big).alignment == 4096);
    SW_CHECK(aligned_to(big, 4096) && !aligned_to(big, 8192));
    ca.destroy_array(big, 3, kCacheLineAlign<Over4096>);

    // Pool do cache de RAM (4096) e bytes do manifesto (max_align_t).
    void* ram = ca.allocate(262144, kRamPoolAlign, &st);
    SW_CHECK(ram != nullptr && aligned_to(ram, 4096) && !aligned_to(ram, 8192));
    SW_CHECK(ExactAlignAllocator::header_of(ram).alignment == 4096);
    ca.deallocate(ram, 262144, kRamPoolAlign);
    void* bytes = ca.allocate(1000, kBytesAlign, &st);
    SW_CHECK(bytes != nullptr && aligned_to(bytes, alignof(std::max_align_t)));
    SW_CHECK(ExactAlignAllocator::header_of(bytes).alignment == alignof(std::max_align_t));
    ca.deallocate(bytes, 1000, kBytesAlign);

    SW_CHECK(ea.live() == 0);
    SW_CHECK(ea.alloc_calls() == ea.free_calls());
    std::puts("ok: alinhamento exato pelos tipos reais (64, 4096, pool de pedidos, RAM, bytes)");
}

void test_misaligned_pointer_is_returned_and_fails()
{
    ExactAlignAllocator ea;
    ea.set_misaligned(true);
    const ContextAllocator ca(ea.as_sw());
    AllocStatus st = AllocStatus::Ok;
    Over64* p = ca.create<Over64>(&st);
    SW_CHECK(p == nullptr);
    SW_CHECK(st == AllocStatus::Misaligned);
    // O bloco voltou ao free_fn, com o mesmo tamanho, e nada ficou vivo.
    SW_CHECK(ea.alloc_calls() == 1 && ea.free_calls() == 1);
    SW_CHECK(ea.last_freed_size() == sizeof(Over64));
    SW_CHECK(ea.live() == 0);
    void* raw = ca.allocate(10, 16, &st);
    SW_CHECK(raw == nullptr && st == AllocStatus::Misaligned && ea.live() == 0);
    std::puts("ok: ponteiro desalinhado do alloc_fn => Misaligned, bloco devolvido ao free_fn");
}

void test_failure_and_invalid_requests()
{
    ExactAlignAllocator ea;
    const ContextAllocator ca(ea.as_sw());
    AllocStatus st = AllocStatus::Ok;
    ea.fail_at(1);
    SW_CHECK(ca.create<Plain>(&st) == nullptr);
    SW_CHECK(st == AllocStatus::OutOfMemory);
    SW_CHECK(ea.failed_calls() == 1 && ea.live() == 0 && ea.free_calls() == 0);
    ea.fail_at(0);

    // Uso inválido: não chega ao alloc_fn.
    const std::uint64_t calls = ea.alloc_calls() + ea.failed_calls();
    SW_CHECK(ca.allocate(0, 16, &st) == nullptr && st == AllocStatus::InvalidArgument);
    SW_CHECK(ca.allocate(8, 3, &st) == nullptr && st == AllocStatus::InvalidArgument);
    SW_CHECK(ca.allocate(8, 0, &st) == nullptr && st == AllocStatus::InvalidArgument);
    SW_CHECK(ca.create_array<Plain>(0, 16, &st) == nullptr && st == AllocStatus::InvalidArgument);
    SW_CHECK(ca.create_array<Plain>(static_cast<std::size_t>(-1) / 2, 16, &st) == nullptr);
    SW_CHECK(st == AllocStatus::OutOfMemory); // estouro de n * sizeof(T): nenhum alloc_fn
    SW_CHECK(ca.create_aligned<Over64>(32, &st) == nullptr && st == AllocStatus::InvalidArgument);
    SW_CHECK(ea.alloc_calls() + ea.failed_calls() == calls);

    // Nunca free_fn(NULL).
    const std::uint64_t frees = ea.free_calls();
    ca.deallocate(nullptr, 10, 16);
    ca.destroy<Plain>(nullptr);
    ca.destroy_array<Plain>(nullptr, 3, 16);
    SW_CHECK(ea.free_calls() == frees);
    std::puts("ok: falha do alloc_fn, uso inválido sem chamar o alocador, nenhum free_fn(NULL)");
}

void test_system_allocator()
{
    const ContextAllocator sys;
    AllocStatus st = AllocStatus::Ok;
    Over4096* p = sys.create<Over4096>(&st);
    SW_CHECK(p != nullptr && st == AllocStatus::Ok && aligned_to(p, 4096));
    sys.destroy(p);
    auto* pool = sys.create_array<swarmstream::detail::Request>(5, kRequestPoolAlign, &st);
    SW_CHECK(pool != nullptr && aligned_to(pool, kRequestPoolAlign));
    sys.destroy_array(pool, 5, kRequestPoolAlign);
    std::puts("ok: alocador do sistema (alinhado, nothrow)");
}

void test_sixteen_threads()
{
    ExactAlignAllocator ea;
    const ContextAllocator ca(ea.as_sw());
    constexpr int kThreads = 16;
    constexpr int kIters = 2000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ca, t] {
            for (int i = 0; i < kIters; ++i) {
                AllocStatus st = AllocStatus::Ok;
                if ((i + t) % 3 == 0) {
                    Over64* p = ca.create<Over64>(&st);
                    SW_CHECK(p != nullptr && aligned_to(p, 64));
                    p->bytes[0] = static_cast<unsigned char>(i);
                    ca.destroy(p);
                } else if ((i + t) % 3 == 1) {
                    auto* a = ca.create_array<Plain>(5, 16, &st);
                    SW_CHECK(a != nullptr);
                    a[4].a = i;
                    ca.destroy_array(a, 5, 16);
                } else {
                    void* b = ca.allocate(257, 32, &st);
                    SW_CHECK(b != nullptr && aligned_to(b, 32));
                    ca.deallocate(b, 257, 32);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
    SW_CHECK(ea.live() == 0);
    SW_CHECK(ea.alloc_calls() == static_cast<std::uint64_t>(kThreads) * kIters);
    SW_CHECK(ea.free_calls() == ea.alloc_calls());
    std::puts("ok: 16 threads no mesmo alocador (vivos == 0, tamanhos conferidos no free_fn)");
}

} // namespace

int main()
{
    test_valid_pair();
    test_alignment_by_real_types();
    test_misaligned_pointer_is_returned_and_fails();
    test_failure_and_invalid_requests();
    test_system_allocator();
    test_sixteen_threads();
    std::puts("TODOS OS TESTES DO ALOCADOR DO CONTEXTO (F1-67 B) PASSARAM");
    return 0;
}
