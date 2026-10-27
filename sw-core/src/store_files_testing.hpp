// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Ganchos de teste do StoreFiles (F1-10). Fica em src/, fora da API instalada: só os testes
// do sw-core incluem este arquivo.
#pragma once

#include "swarmstream/store_files.hpp"

#include <cstdint>

namespace swarmstream {

struct StoreFilesTestHook
{
    // O próximo sync (flush ou criação de arquivo) falha como se o disco tivesse devolvido EIO.
    static void fail_next_sync(StoreFiles& store) noexcept;
    // Quantas vezes a pasta foi sincronizada depois de criar ou completar um arquivo.
    static uint32_t directory_syncs(const StoreFiles& store) noexcept;
    // Syncs de arquivo (flush, criação) que chegaram ao disco com sucesso.
    static uint64_t file_syncs(const StoreFiles& store) noexcept;
    // Pula a conferência do espaço livre antes de reservar, para exercitar o DiskFull que vem
    // do próprio SO (ENOSPC / ERROR_DISK_FULL). Só num sistema de arquivos pequeno e isolado.
    static void skip_space_check(StoreFiles& store) noexcept;
    // F1-32: o próximo write() grava metade dos bytes e devolve `code` (DiskFull ou IoError), como
    // um ENOSPC ou EIO no meio da escrita.
    static void fail_next_write(StoreFiles& store, CacheErrorCode code) noexcept;
    // POSIX: pula o fallocate(2) e usa direto a posix_fallocate, o ramo de EOPNOTSUPP.
    static void force_posix_fallocate(StoreFiles& store) noexcept;
};

} // namespace swarmstream
