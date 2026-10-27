// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-15: cache de blocos descomprimidos em RAM com pool de buffers (docs/04 §Memória).
//
// Tudo é reservado no construtor: os buffers (um por bloco, do tamanho máximo de um bloco,
// 256 KiB), a tabela de entradas, a tabela hash e a lista de livres. Depois disso, acerto
// (`acquire`), preenchimento (`reserve`/`commit`), remoção e soltar o lease NÃO alocam: é o
// critério de aceite da F1-15 e o teste mede com um `operator new` contador.
//
// Regras:
//  - Um bloco com lease ativo nunca é removido nem reaproveitado (como ADR-0005, mas por bloco).
//  - Remoção CLOCK entre os blocos sem lease; um acerto marca o bloco como "lido".
//  - Quem descomprime escreve direto no buffer do pool (`BlockFill::buffer()`), sem cópia
//    intermediária; só `commit()` torna o bloco visível, então ninguém lê bloco pela metade.
//  - Uma trava curta (`std::mutex`) protege a tabela. Ela cobre só buscas e trocas de contadores,
//    nunca a cópia dos bytes nem a descompressão. Um bloco só deve entrar se quem o preencheu já
//    o verificou (SHA-256 na primeira entrada, docs/03 §4); este cache não confere hash.
//  - `BlockLease` e `BlockFill` guardam ponteiro para o cache: ele tem de viver mais que eles.
#pragma once

#include "swarmstream/result.hpp"
#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace swarmstream {

// Tamanho máximo do `raw_size` de um bloco (docs/03 §2).
inline constexpr size_t kMaxBlockSize = 256 * 1024;

struct BlockKey
{
    PieceId piece{};
    uint32_t block{0}; // índice do bloco dentro da peça

    constexpr bool operator==(const BlockKey& o) const noexcept
    {
        return block == o.block && piece == o.piece;
    }
};

enum class BlockCacheError {
    InvalidSize,  // size == 0 ou maior que o buffer
    Exists,       // o bloco já está no cache (use `acquire`)
    Filling,      // outro preenchimento do mesmo bloco está em andamento
    OutOfBuffers, // todos os buffers estão presos por lease ou por preenchimento
};

struct BlockCacheStats
{
    size_t capacity_blocks{0};
    size_t resident_blocks{0}; // preenchidos e visíveis
    size_t leased_blocks{0};   // com pelo menos um lease
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
};

class BlockRamCache;

// Lease de leitura de um bloco. Só se move. Soltar não aloca.
class BlockLease
{
public:
    BlockLease() noexcept = default;
    ~BlockLease() { reset(); }
    BlockLease(const BlockLease&) = delete;
    BlockLease& operator=(const BlockLease&) = delete;
    BlockLease(BlockLease&& o) noexcept
        : cache_(std::exchange(o.cache_, nullptr)), index_(o.index_),
          data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0))
    {
    }
    BlockLease& operator=(BlockLease&& o) noexcept
    {
        if (this != &o) {
            reset();
            cache_ = std::exchange(o.cache_, nullptr);
            index_ = o.index_;
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return cache_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept { return {data_, size_}; }
    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    void reset() noexcept;

private:
    friend class BlockRamCache;
    BlockLease(BlockRamCache* c, uint32_t index, const uint8_t* data, size_t size) noexcept
        : cache_(c), index_(index), data_(data), size_(size)
    {
    }
    BlockRamCache* cache_{nullptr};
    uint32_t index_{0};
    const uint8_t* data_{nullptr};
    size_t size_{0};
};

// Buffer reservado para preencher um bloco. Se for destruído sem `commit()`, devolve o buffer.
class BlockFill
{
public:
    BlockFill() noexcept = default;
    ~BlockFill() { abort(); }
    BlockFill(const BlockFill&) = delete;
    BlockFill& operator=(const BlockFill&) = delete;
    BlockFill(BlockFill&& o) noexcept
        : cache_(std::exchange(o.cache_, nullptr)), index_(o.index_),
          data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0))
    {
    }
    BlockFill& operator=(BlockFill&& o) noexcept
    {
        if (this != &o) {
            abort();
            cache_ = std::exchange(o.cache_, nullptr);
            index_ = o.index_;
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    // Bytes a escrever: exatamente `size` (o `raw_size` do bloco).
    [[nodiscard]] std::span<uint8_t> buffer() noexcept { return {data_, size_}; }
    [[nodiscard]] bool valid() const noexcept { return cache_ != nullptr; }
    // Publica o bloco e devolve um lease dele. Depois disso o objeto fica vazio.
    [[nodiscard]] BlockLease commit() noexcept;
    // Desiste: o buffer volta ao pool.
    void abort() noexcept;

private:
    friend class BlockRamCache;
    BlockFill(BlockRamCache* c, uint32_t index, uint8_t* data, size_t size) noexcept
        : cache_(c), index_(index), data_(data), size_(size)
    {
    }
    BlockRamCache* cache_{nullptr};
    uint32_t index_{0};
    uint8_t* data_{nullptr};
    size_t size_{0};
};

class BlockRamCache
{
public:
    // `capacity_bytes` é arredondado para baixo em múltiplos de `buffer_size` (mínimo 1 buffer).
    explicit BlockRamCache(size_t capacity_bytes, size_t buffer_size = kMaxBlockSize);
    ~BlockRamCache() = default;
    BlockRamCache(const BlockRamCache&) = delete;
    BlockRamCache& operator=(const BlockRamCache&) = delete;

    // Acerto: devolve um lease (inválido se o bloco não está pronto). Não aloca.
    [[nodiscard]] BlockLease acquire(const BlockKey& key) noexcept;

    // Falta: reserva um buffer (removendo por CLOCK um bloco sem lease, se preciso). Não aloca.
    [[nodiscard]] Result<BlockFill, BlockCacheError> reserve(const BlockKey& key,
                                                             size_t size) noexcept;

    [[nodiscard]] BlockCacheStats stats() const noexcept;
    [[nodiscard]] size_t buffer_size() const noexcept { return buffer_size_; }

private:
    friend class BlockLease;
    friend class BlockFill;

    static constexpr uint32_t kNone = ~uint32_t{0};
    enum class State : uint8_t { Free, Filling, Ready };
    struct Entry
    {
        BlockKey key{};
        uint32_t size{0};
        uint32_t refs{0};
        uint32_t next{kNone}; // próximo na corrente do balde
        State state{State::Free};
        bool referenced{false};
    };

    void release(uint32_t index) noexcept;
    [[nodiscard]] BlockLease publish(uint32_t index) noexcept;
    void discard(uint32_t index) noexcept;
    [[nodiscard]] uint32_t find_locked(const BlockKey& key) const noexcept;
    void unlink_locked(uint32_t index) noexcept;
    [[nodiscard]] uint32_t take_victim_locked() noexcept;
    [[nodiscard]] size_t bucket_of(const BlockKey& key) const noexcept;
    [[nodiscard]] uint8_t* buffer_at(uint32_t index) const noexcept
    {
        return pool_.get() + static_cast<size_t>(index) * buffer_size_;
    }

    size_t buffer_size_;
    size_t capacity_blocks_;
    std::unique_ptr<uint8_t[]> pool_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> buckets_;
    std::vector<uint32_t> free_list_; // pilha; capacidade reservada para todas as entradas
    size_t hand_{0};
    mutable std::mutex mutex_;
    size_t resident_{0};
    size_t leased_{0};
    uint64_t hits_{0};
    uint64_t misses_{0};
    uint64_t evictions_{0};
};

} // namespace swarmstream
