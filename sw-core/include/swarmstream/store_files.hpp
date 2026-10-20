// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// StoreFiles: os arquivos store-XXX.bin do cache em disco (docs/04 §1, ADR-0005).
//
// - Cada arquivo tem `slots_per_file` slots de `slot_size` bytes (padrão: 2048 × 1 MiB =
//   2 GiB). O slot global s fica no arquivo s / slots_per_file, no offset
//   (s % slots_per_file) × slot_size, alinhado a 4 KiB.
// - Os arquivos são criados SOB DEMANDA (o primeiro acesso a um slot cria o arquivo dele)
//   e PRÉ-ALOCADOS de verdade, não esparsos: o espaço fica reservado no disco na criação,
//   e o disco cheio aparece ali (DiskFull), nunca no meio de uma gravação de peça.
// - Reabrir não trunca nem apaga nada. Um arquivo maior que o layout esperado é recusado
//   (IoError), porque pode ser de outro layout; menor é completado até o tamanho certo.
// - Todo retorno de I/O é conferido: escrita ou leitura parcial é repetida até o fim, e
//   qualquer falha volta como erro (docs/04 §Estrutura em disco, regra "nada calado").
// - flush() força os dados ao disco (FlushFileBuffers / fdatasync): é a "Barreira 1" do
//   protocolo de escrita do 04 §3, que a F1-11 chama antes de gravar o journal.
//
// Não sabe de peças, índice nem journal (F1-11) e não é thread-safe: o dono é a thread de
// I/O do cache (ADR-0005, F1-12).
//
// Limitação conhecida (Windows): o NTFS zera, na primeira escrita, a faixa entre o fim dos
// dados válidos do arquivo e o ponto escrito. Com a política do menor slot livre do
// SlotAllocator as escritas tendem a ser sequenciais, então o custo fica perto do da própria
// escrita; medir na F1-12 antes de decidir zerar os arquivos em segundo plano.

#pragma once

#include "swarmstream/result.hpp"
#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace swarmstream {

struct StoreLayout
{
    uint32_t slots_per_file = 2048;         // 2 GiB por arquivo com slots de 1 MiB
    uint32_t slot_size = uint32_t{1} << 20; // múltiplo de 4096 (docs/04 §1)
};

class StoreFiles
{
public:
    // `dir` precisa existir. `total_slots` é o teto do cache em slots.
    // InvalidSize se o layout é inválido (zero slots por arquivo, slot não múltiplo de 4096);
    // IoError se `dir` não é uma pasta.
    [[nodiscard]] static Result<StoreFiles, CacheErrorCode>
    open(const std::filesystem::path& dir, uint32_t total_slots, StoreLayout layout = {});

    StoreFiles(StoreFiles&&) noexcept;
    StoreFiles& operator=(StoreFiles&&) noexcept;
    StoreFiles(const StoreFiles&) = delete;
    StoreFiles& operator=(const StoreFiles&) = delete;
    ~StoreFiles();

    // Garante que o arquivo do slot existe e está pré-alocado. SlotNotFound fora do teto;
    // DiskFull se não há espaço para pré-alocar; IoError nos outros casos.
    [[nodiscard]] Result<void, CacheErrorCode> ensure_slot(SlotId slot);

    // Escrita/leitura posicionais dentro de um slot. InvalidOffset se passa do fim do slot.
    [[nodiscard]] Result<void, CacheErrorCode> write(SlotId slot, size_t offset,
                                                     std::span<const std::byte> data);
    [[nodiscard]] Result<void, CacheErrorCode> read(SlotId slot, size_t offset,
                                                    std::span<std::byte> out);

    // Dados de todos os arquivos abertos no disco (Barreira 1 do 04 §3).
    [[nodiscard]] Result<void, CacheErrorCode> flush();

    [[nodiscard]] uint32_t total_slots() const noexcept;
    [[nodiscard]] const StoreLayout& layout() const noexcept;
    [[nodiscard]] uint32_t file_count() const noexcept;    // arquivos que o teto pode exigir
    [[nodiscard]] uint32_t files_created() const noexcept; // arquivos abertos até agora

    // Bytes realmente reservados no disco para um arquivo (para provar que não é esparso).
    [[nodiscard]] static Result<uint64_t, CacheErrorCode>
    allocated_bytes(const std::filesystem::path& file);

private:
    struct Impl;
    explicit StoreFiles(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
