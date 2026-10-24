// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// SlotAllocator: quais slots dos arquivos de store estão ocupados (docs/04 §1, ADR-0005).
//
// Os slots têm tamanho fixo (uma peça de até 1 MiB), então não existe fragmentação externa:
// qualquer slot livre serve para qualquer peça. A política é sempre o MENOR slot livre, para
// que a parte usada dos arquivos fique compacta no começo (os arquivos do fim podem ficar sem
// uso e, no futuro, ser devolvidos ao disco).
//
// Memória fixa: um bit por slot mais um bit por palavra de 64 slots (~128 KiB para 1 milhão
// de slots). Nenhuma operação aloca. allocate() custa O(capacidade / 4096) no pior caso.
//
// NÃO é thread-safe, de propósito: o dono é a thread de I/O do cache (ADR-0005, F1-12); as
// outras threads só pedem a ela.

#pragma once

#include "swarmstream/result.hpp"
#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swarmstream {

class SlotAllocator
{
public:
    explicit SlotAllocator(uint32_t capacity);

    // O menor slot livre. OutOfSlots quando todos estão usados.
    [[nodiscard]] Result<SlotId, CacheErrorCode> allocate() noexcept;

    // Devolve um slot. SlotNotFound se está fora do intervalo ou já estava livre.
    [[nodiscard]] Result<void, CacheErrorCode> release(SlotId slot) noexcept;

    // Marca um slot como usado sem passar pela política (recuperação pelo journal, F1-11).
    // SlotNotFound se fora do intervalo; SlotBusy se já estava usado.
    [[nodiscard]] Result<void, CacheErrorCode> mark_used(SlotId slot) noexcept;

    [[nodiscard]] bool is_used(SlotId slot) const noexcept;
    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint32_t used() const noexcept { return used_; }

    // 1 + o maior slot usado (0 se nenhum): quantos slots do começo dos arquivos estão em uso.
    // O(capacidade / 64) no pior caso; não é do caminho quente.
    [[nodiscard]] uint32_t high_water() const noexcept;

    // Bytes de memória que o alocador ocupa (fixo depois de construído).
    [[nodiscard]] size_t memory_bytes() const noexcept;

private:
    void set_used(uint32_t slot) noexcept;
    void set_free(uint32_t slot) noexcept;

    uint32_t capacity_;
    uint32_t used_{0};
    std::vector<uint64_t>
        free_bits_; // bit 1 = slot livre; um por slot (bits além da capacidade = 0)
    std::vector<uint64_t> has_free_; // bit 1 = a palavra correspondente tem algum slot livre
};

} // namespace swarmstream
