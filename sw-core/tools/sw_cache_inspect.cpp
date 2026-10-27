// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// sw-cache-inspect (F1-18): lista as peças, o uso e o estado do cache em disco, SEM alterá-lo.
//
//   sw-cache-inspect <pasta-do-cache> [--list] [--slots-per-file N] [--slot-size BYTES]
//
// Sem --list mostra só o resumo. O layout (padrão: 2048 slots de 1 MiB) tem de ser o do cache:
// um index.bin de outro layout aparece como INVÁLIDO (a recuperação também o descartaria).
// Códigos de saída: 0 inspeção feita (índice ou journal inválidos são achados, não erros);
// 1 não foi possível inspecionar; 2 uso incorreto.
#include "cache_inspect.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using swarmstream::detail::InspectFileState;

const char* state_name(InspectFileState s)
{
    switch (s) {
    case InspectFileState::Missing: return "ausente";
    case InspectFileState::Invalid: return "INVALIDO (a recuperacao o descartaria)";
    case InspectFileState::Ok: return "ok";
    }
    return "?";
}

unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

bool parse_u32(const char* text, uint32_t& out)
{
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v == 0 || v > 0xFFFFFFFFULL) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

int usage()
{
    std::fprintf(stderr, "uso: sw-cache-inspect <pasta-do-cache> [--list] "
                         "[--slots-per-file N] [--slot-size BYTES]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path dir;
    swarmstream::StoreLayout layout;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if ((a == "--slots-per-file" || a == "--slot-size") && i + 1 < argc) {
            uint32_t v = 0;
            if (!parse_u32(argv[++i], v)) return usage();
            (a == "--slots-per-file" ? layout.slots_per_file : layout.slot_size) = v;
        } else if (!a.empty() && a[0] != '-' && dir.empty()) {
            dir = a;
        } else {
            return usage();
        }
    }
    if (dir.empty() || layout.slot_size % 4096 != 0) return usage();

    auto r = swarmstream::detail::inspect_cache(dir, layout);
    if (!r) {
        std::fprintf(stderr, "sw-cache-inspect: nao foi possivel inspecionar '%s'\n",
                     dir.string().c_str());
        return 1;
    }
    const auto& c = *r;
    std::printf("cache: %s\n", dir.string().c_str());
    std::printf("layout: %u slots por arquivo, slot de %u bytes\n", layout.slots_per_file,
                layout.slot_size);
    std::printf("index.bin: %s", state_name(c.index));
    if (c.index == InspectFileState::Ok)
        std::printf(" (%llu entradas, lsn %llu)", ull(c.index_entries), ull(c.index_lsn));
    std::printf("\njournal.bin: %s", state_name(c.journal));
    if (c.journal == InspectFileState::Ok)
        std::printf(" (%llu registros, %llu aplicados sobre o indice, ultimo lsn %llu, "
                    "cauda invalida de %llu bytes)",
                    ull(c.journal_records), ull(c.journal_applied), ull(c.journal_last_lsn),
                    ull(c.journal_truncated_bytes));
    std::printf("\npecas: %zu, %llu bytes de conteudo\n", c.pieces.size(),
                ull(c.total_piece_bytes));
    if (c.pieces_in_missing_files != 0)
        std::printf("AVISO: %u peca(s) apontam para store-XXX.bin ausente (seriam descartadas)\n",
                    c.pieces_in_missing_files);
    for (const auto& f : c.store_files)
        std::printf("store-%03u.bin: %s, %llu bytes no arquivo, %u pecas, %llu bytes de conteudo\n",
                    f.id, f.present ? "presente" : "AUSENTE", ull(f.size_bytes), f.pieces,
                    ull(f.piece_bytes));
    if (list) {
        std::printf("arquivo slot tamanho blocos hash\n");
        for (const auto& p : c.pieces)
            std::printf("%u %u %u %u %s%s\n", p.store_file_id, p.slot_in_file, p.size,
                        p.block_count, p.piece.to_hex().c_str(),
                        p.store_file_present ? "" : " (arquivo ausente)");
    }
    return 0;
}
