// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// StoreFiles: os arquivos store-XXX.bin do cache em disco (docs/04 §1, ADR-0005).
//
// - Cada arquivo tem `slots_per_file` slots de `slot_size` bytes (padrão: 2048 × 1 MiB =
//   2 GiB). O slot global s fica no arquivo s / slots_per_file, no offset
//   (s % slots_per_file) × slot_size, alinhado a 4 KiB.
// - Os arquivos são criados SOB DEMANDA (o primeiro acesso a um slot cria o arquivo dele)
//   e PRÉ-ALOCADOS de verdade, não esparsos: o espaço fica reservado no disco na criação,
//   e o disco cheio aparece ali (DiskFull), nunca no meio de uma gravação de peça. O espaço
//   livre é conferido antes de pedir a reserva; se ela falhar, o que foi reservado volta ao
//   disco e um arquivo recém-criado é apagado.
// - Ao criar ou completar um arquivo, o tamanho dele e (POSIX) a entrada na pasta vão ao
//   disco antes de o slot ser usado. Mesmo assim a F1-11 deve tratar store-XXX.bin ausente
//   ou zerado como peças perdidas (o checksum por bloco não confere), nunca como erro fatal.
// - Reabrir não trunca nem apaga nada. Um arquivo maior que o layout esperado é recusado
//   (IoError), porque pode ser de outro layout; menor é completado até o tamanho certo. A única
//   exceção é shrink_to_ceiling(), que a CacheOwner chama quando o teto diminuiu (F1-11).
// - Todo retorno de I/O é conferido, inclusive o do fechamento: escrita ou leitura parcial é
//   repetida até o fim, e qualquer falha volta como erro (docs/04, regra "nada calado").
// - flush() PEDE ao SO e ao disco que gravem os dados (FlushFileBuffers / fdatasync): é a
//   "Barreira 1" do protocolo de escrita do 04 §3, que a F1-11 chama antes de gravar o
//   journal. Não há como provar a durabilidade: um disco que mente sobre o cache perde dado.
// - FALHA DE SYNC É PEGAJOSA. Depois de um erro de fdatasync, o Linux pode descartar as
//   páginas sujas e o sync seguinte devolver sucesso sem os dados no disco. Por isso, depois
//   da primeira falha de sync (no flush ou na criação de arquivo), broken() fica true e toda
//   operação devolve IoError até fechar e reabrir; quem reabre (F1-11) refaz a recuperação e
//   confere os checksums. Nunca repita um flush que falhou esperando sucesso.
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
    // Fecha sem relatar erro (último recurso). Para saber se o fechamento falhou, chame close().
    ~StoreFiles();

    // Todas as operações abaixo devolvem IoError depois de close() ou com broken() true.

    // Garante que o arquivo do slot existe e está pré-alocado. SlotNotFound fora do teto;
    // DiskFull se não há espaço para pré-alocar; IoError nos outros casos.
    [[nodiscard]] Result<void, CacheErrorCode> ensure_slot(SlotId slot);

    // Escrita/leitura posicionais dentro de um slot. InvalidOffset se passa do fim do slot.
    [[nodiscard]] Result<void, CacheErrorCode> write(SlotId slot, size_t offset,
                                                     std::span<const std::byte> data);
    [[nodiscard]] Result<void, CacheErrorCode> read(SlotId slot, size_t offset,
                                                    std::span<std::byte> out);

    // Pede a gravação no disco dos dados de todos os arquivos abertos (Barreira 1 do 04 §3).
    // Uma falha deixa o store quebrado (broken()); ver "falha de sync é pegajosa" acima.
    [[nodiscard]] Result<void, CacheErrorCode> flush();

    // Fecha todos os arquivos conferindo o retorno (close/CloseHandle) e devolve o primeiro
    // erro. Não faz flush: chame flush() antes se os dados precisam estar no disco. Depois de
    // close(), o store só serve para ser destruído; chamar de novo devolve sucesso.
    [[nodiscard]] Result<void, CacheErrorCode> close();

    // true depois de uma falha de sync: o store precisa ser fechado e reaberto.
    [[nodiscard]] bool broken() const noexcept;

    [[nodiscard]] uint32_t total_slots() const noexcept;
    [[nodiscard]] const StoreLayout& layout() const noexcept;
    [[nodiscard]] uint32_t file_count() const noexcept;    // arquivos que o teto pode exigir
    [[nodiscard]] uint32_t files_created() const noexcept; // arquivos abertos até agora

    // F1-11: o teto diminuiu. Apaga os store-XXX.bin além do teto e encurta os que ficaram
    // maiores que o layout pede (o índice já tirou as peças desses slots). Chamar antes de
    // qualquer ensure_slot/read/write. Sem isso, reabrir com teto menor dava IoError.
    [[nodiscard]] Result<void, CacheErrorCode> shrink_to_ceiling();

    // Caminho do arquivo de store `index` em `dir` ("store-%03u.bin").
    [[nodiscard]] static std::filesystem::path file_path(const std::filesystem::path& dir,
                                                         uint32_t index);

    // Bytes realmente reservados no disco para um arquivo (para provar que não é esparso).
    [[nodiscard]] static Result<uint64_t, CacheErrorCode>
    allocated_bytes(const std::filesystem::path& file);

private:
    friend struct StoreFilesTestHook; // src/store_files_testing.hpp, só para os testes
    struct Impl;
    explicit StoreFiles(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace swarmstream
