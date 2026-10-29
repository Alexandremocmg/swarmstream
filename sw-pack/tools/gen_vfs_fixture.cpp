// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-17: gera as fixtures do VFS (sw-core/tests/vfs_fixture.hpp): três manifestos v0 ASSINADOS com
// a chave de TESTE do ADR-0012 (semente 00..1f), a partir dos dados de
// sw-core/tests/vfs_fixture_data.hpp:
//   principal  12 arquivos, 9 blocos, 2 grupos (`core` e `extra`), 2 peças;
//   conflito   "a" é arquivo e também diretório ("a/b"): o §8 aceita, o Vfs::build recusa;
//   vazio      nenhum arquivo, nenhum bloco (a raiz existe e está vazia).
//
// Uso:  gen-vfs-fixture <saida.hpp>      (escreve o cabeçalho de fixtures)
//       gen-vfs-fixture --bin <dir>      (grava principal.bin, conflito.bin, vazio.bin)
// Ed25519 é determinístico: o mesmo libsodium gera os mesmos bytes.
#include "swarmstream/chunker.hpp"
#include "swarmstream/manifest.hpp"
#include "swarmstream/sha256.hpp"
#include "vfs_fixture_data.hpp"

#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sp = swarmstream::pack;
namespace fx = swarmstream::test::vfs_fixture;

namespace {

sp::ManifestModel base_model(const char* game_id)
{
    sp::ManifestModel m;
    m.game_id = game_id;
    m.build_id = "0.1.0+vfs";
    m.build_seq = 1;
    m.created_at = 1'790'000'000;
    m.chunker = {32U * 1024U, 128U * 1024U, 256U * 1024U, std::string(sp::Chunker::kAlgorithm)};
    return m;
}

sp::Hash32 filled(std::uint8_t first)
{
    sp::Hash32 h{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<std::uint8_t>(first + i);
    }
    return h;
}

sp::GroupInfo group(const char* name, std::uint8_t priority, std::uint32_t first,
                    std::uint32_t count)
{
    sp::GroupInfo g;
    g.name = name;
    g.priority = priority;
    g.piece_first = first;
    g.piece_count = count;
    return g;
}

void add_file(sp::ManifestModel& m, const std::string& path, sp::FileClass cls, std::uint16_t grp,
              const std::vector<std::uint32_t>& refs, std::uint64_t size)
{
    m.file_path.push_back(path);
    m.file_size.push_back(size);
    m.file_class.push_back(cls);
    m.file_group.push_back(grp);
    m.file_first_ref.push_back(static_cast<std::uint32_t>(m.block_refs.size()));
    m.file_ref_count.push_back(static_cast<std::uint32_t>(refs.size()));
    m.block_refs.insert(m.block_refs.end(), refs.begin(), refs.end());
}

sp::ManifestModel main_model()
{
    sp::ManifestModel m = base_model("swarmstream.vfs");
    for (std::uint32_t b = 0; b < fx::kBlockRawSize.size(); ++b) {
        const auto bytes = fx::block_bytes(b);
        m.block_hash.push_back(sp::sha256(std::as_bytes(std::span(bytes))));
        m.block_raw_size.push_back(fx::kBlockRawSize[b]);
        m.block_packed_size.push_back(fx::kBlockRawSize[b]);
        m.block_codec.push_back(sp::Codec::None);
    }
    m.piece_hash = {filled(0xA0), filled(0xC0)};
    m.piece_size = {fx::kPackedCore, fx::kBlockRawSize[8]};
    m.piece_first_block = {0, static_cast<std::uint32_t>(fx::kCoreBlocks)};
    m.piece_block_count = {static_cast<std::uint32_t>(fx::kCoreBlocks), 1};
    m.piece_group = {0, 1};
    for (const fx::FileDef& f : fx::files()) {
        add_file(m, f.path, static_cast<sp::FileClass>(f.file_class), f.group, f.refs,
                 fx::file_size_of(f));
    }
    m.groups = {group("core", 0, 0, 1), group("extra", 1, 1, 1)};
    return m;
}

sp::ManifestModel conflict_model()
{
    sp::ManifestModel m = base_model("swarmstream.vfsconflict");
    const std::vector<std::uint8_t> bytes(10, 0x5A);
    m.block_hash = {sp::sha256(std::as_bytes(std::span(bytes)))};
    m.block_raw_size = {10};
    m.block_packed_size = {10};
    m.block_codec = {sp::Codec::None};
    m.piece_hash = {filled(0xA0)};
    m.piece_size = {10};
    m.piece_first_block = {0};
    m.piece_block_count = {1};
    m.piece_group = {0};
    // "a" < "a-1" < "a.x" < "a/b": o diretório "a/" só aparece depois de dois arquivos que não
    // são dele.
    for (const char* p : {"a", "a-1", "a.x", "a/b"}) {
        add_file(m, p, sp::FileClass::Core, 0, {0}, 10);
    }
    m.groups = {group("core", 0, 0, 1)};
    return m;
}

sp::ManifestModel empty_model()
{
    sp::ManifestModel m = base_model("swarmstream.vfsempty");
    m.groups = {group("core", 0, 0, 0)};
    return m;
}

std::string array_text(const char* name, const std::vector<std::byte>& bytes)
{
    std::string s = "inline constexpr std::array<std::uint8_t, " + std::to_string(bytes.size()) +
                    "> " + name + " = {";
    char buf[8];
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        s += (i % 16 == 0) ? "\n    " : " ";
        std::snprintf(buf, sizeof buf, "0x%02x,", static_cast<unsigned>(bytes[i]));
        s += buf;
    }
    return s + "\n};\n\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fputs("uso: gen-vfs-fixture <saida.hpp> | --bin <dir>\n", stderr);
        return 2;
    }
    sp::Ed25519Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::uint8_t>(i);
    }
    const auto key = sp::ed25519_public_key(seed);
    if (!key) {
        std::fputs("ed25519_public_key falhou\n", stderr);
        return 1;
    }
    const auto main_built = sp::build_manifest(main_model(), seed);
    const auto conflict_built = sp::build_manifest(conflict_model(), seed);
    const auto empty_built = sp::build_manifest(empty_model(), seed);
    for (const auto* b : {&main_built, &conflict_built, &empty_built}) {
        if (!b->status.ok()) {
            std::fprintf(stderr, "build_manifest falhou: %s\n", b->status.detail);
            return 1;
        }
    }

    if (std::string(argv[1]) == "--bin") {
        if (argc < 3) {
            return 2;
        }
        const std::string dir = argv[2];
        const std::pair<const char*, const std::vector<std::byte>*> outs[] = {
            {"principal.bin", &main_built.bytes},
            {"conflito.bin", &conflict_built.bytes},
            {"vazio.bin", &empty_built.bytes}};
        for (const auto& [name, bytes] : outs) {
            std::ofstream out(dir + "/" + name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                std::fprintf(stderr, "falha ao gravar %s\n", name);
                return 1;
            }
        }
        return 0;
    }

    std::string text =
        "// SPDX-License-Identifier: Apache-2.0\n"
        "// Copyright 2026 Alexandre Aparecido Rodrigues Peres\n\n"
        "// GERADO por sw-pack/tools/gen_vfs_fixture.cpp (F1-17); não edite à mão. Manifestos v0\n"
        "// assinados com a chave de TESTE do ADR-0012 (semente 00..1f), a partir de\n"
        "// vfs_fixture_data.hpp: principal (12 arquivos, 9 blocos, 2 grupos), conflito (\"a\" é\n"
        "// arquivo e diretório) e vazio (sem arquivos). Para regenerar: construir o alvo\n"
        "// `gen-vfs-fixture` e rodar `gen-vfs-fixture sw-core/tests/vfs_fixture.hpp`.\n"
        "#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
        "namespace swarmstream::test::vfs_fixture {\n\n";
    text += "inline constexpr std::array<std::uint8_t, 32> kPublicKey = {";
    char buf[8];
    for (std::size_t i = 0; i < key->size(); ++i) {
        text += (i % 16 == 0) ? "\n    " : " ";
        std::snprintf(buf, sizeof buf, "0x%02x,", static_cast<unsigned>((*key)[i]));
        text += buf;
    }
    text += "\n};\n\n";
    text += array_text("kMainManifest", main_built.bytes);
    text += array_text("kConflictManifest", conflict_built.bytes);
    text += array_text("kEmptyManifest", empty_built.bytes);
    text += "} // namespace swarmstream::test::vfs_fixture\n";

    std::ofstream out(argv[1], std::ios::binary);
    out << text;
    if (!out) {
        std::fprintf(stderr, "falha ao gravar %s\n", argv[1]);
        return 1;
    }
    return 0;
}
