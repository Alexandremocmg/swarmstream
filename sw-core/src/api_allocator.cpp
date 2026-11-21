// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Alocador do contexto (F1-67 parte B; F1-60 §7). Ver api_allocator.hpp.

#include "api_allocator.hpp"

#include <new>

namespace swarmstream::detail {

const char* to_string(AllocStatus status) noexcept
{
    switch (status) {
    case AllocStatus::Ok: return "Ok";
    case AllocStatus::OutOfMemory: return "OutOfMemory";
    case AllocStatus::Misaligned: return "Misaligned";
    case AllocStatus::InvalidArgument: return "InvalidArgument";
    }
    return "AllocStatus?";
}

void* ContextAllocator::allocate(std::size_t size, std::size_t alignment,
                                 AllocStatus* status) const noexcept
{
    if (size == 0 || !is_power_of_two(alignment)) {
        if (status != nullptr) *status = AllocStatus::InvalidArgument;
        return nullptr;
    }
    if (alloc_fn_ == nullptr) {
        // Alocador do sistema: operator new alinhado, sem exceção (nothrow).
        void* p = ::operator new(size, std::align_val_t(alignment), std::nothrow);
        if (status != nullptr) *status = p != nullptr ? AllocStatus::Ok : AllocStatus::OutOfMemory;
        return p;
    }
    void* p = alloc_fn_(user_, size, alignment);
    if (p == nullptr) {
        if (status != nullptr) *status = AllocStatus::OutOfMemory;
        return nullptr;
    }
    if (reinterpret_cast<std::uintptr_t>(p) % alignment != 0) {
        // O alocador do chamador não cumpriu o alinhamento: o bloco volta na hora, com o MESMO
        // tamanho, e a alocação falha (o chamador registra ERROR no log_fn).
        free_fn_(user_, p, size);
        if (status != nullptr) *status = AllocStatus::Misaligned;
        return nullptr;
    }
    if (status != nullptr) *status = AllocStatus::Ok;
    return p;
}

void ContextAllocator::deallocate(void* p, std::size_t size, std::size_t alignment) const noexcept
{
    if (p == nullptr) return; // nunca free_fn(NULL)
    if (alloc_fn_ == nullptr) {
        ::operator delete(p, size, std::align_val_t(alignment));
        return;
    }
    free_fn_(user_, p, size);
}

} // namespace swarmstream::detail
