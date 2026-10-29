// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-17: VFS (caminho + offset + tamanho -> trechos de blocos; listagem de diretório virtual).
//
// As fixtures são manifestos v0 ASSINADOS (vfs_fixture.hpp, gerado por
// sw-pack/tools/gen_vfs_fixture.cpp) abertos pelo MESMO leitor do cliente (ManifestView::open, com
// verificação da assinatura). Os bytes dos blocos vêm de vfs_fixture_data.hpp; o manifesto guarda o
// SHA-256 deles, e o primeiro teste confere isso, de modo que "os dados das fixtures" são
// exatamente os que o manifesto descreve.
//
// O teste de reconstrução usa SÓ os trechos que o Vfs devolve (bloco, deslocamento, comprimento)
// para juntar bytes dos blocos conhecidos e compara com o arquivo original: a referência não passa
// por nenhuma conta do Vfs.
//
// test_resolve_does_not_allocate conta as chamadas a `operator new` (substituído neste executável)
// durante stat/open/resolve/next. O controle positivo prova que o contador enxerga um `new` real.
#include "check.hpp"
#include "swarmstream/vfs.hpp"
#include "vfs_fixture.hpp"
#include "vfs_fixture_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace swarmstream;
namespace fx = swarmstream::test::vfs_fixture;

// ---------------------------------------------------------------- contagem de alocações
namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};
void* volatile g_sink = nullptr;
} // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

// ---------------------------------------------------------------- manifestos

pack::ManifestView open_manifest(std::span<const std::uint8_t> bytes)
{
    static const std::array<pack::Ed25519PublicKey, 1> keys = {[] {
        pack::Ed25519PublicKey k{};
        std::copy(fx::kPublicKey.begin(), fx::kPublicKey.end(), k.begin());
        return k;
    }()};
    pack::ReadOptions options;
    options.trusted_keys = keys;
    pack::ManifestView view;
    const pack::ManifestStatus st = pack::ManifestView::open(std::as_bytes(bytes), options, view);
    if (!st.ok()) {
        std::cerr << "ManifestView::open falhou: " << st.detail << "\n";
    }
    SW_CHECK(st.ok());
    return view;
}

// Os bytes dos manifestos são `inline constexpr` (duração estática): vivem mais que o Vfs.
Vfs build_vfs(std::span<const std::uint8_t> bytes)
{
    Vfs vfs;
    SW_CHECK(Vfs::build(open_manifest(bytes), vfs) == VfsBuildStatus::Ok);
    return vfs;
}

const Vfs& main_vfs()
{
    static const Vfs vfs = build_vfs(fx::kMainManifest);
    return vfs;
}

// ---------------------------------------------------------------- referência

// Reconstrói [offset, offset+size) do arquivo usando SÓ os trechos do Vfs e os blocos conhecidos.
// `block_cache` guarda os bytes de cada bloco (calculados uma vez).
struct Rebuilt
{
    VfsStatus status = VfsStatus::Ok;
    std::vector<std::uint8_t> bytes;
    std::vector<BlockSpan> spans;
    std::uint64_t total = 0;
};

const std::vector<std::vector<std::uint8_t>>& blocks()
{
    static const std::vector<std::vector<std::uint8_t>> all = [] {
        std::vector<std::vector<std::uint8_t>> v;
        for (std::uint32_t b = 0; b < fx::kBlockRawSize.size(); ++b) {
            v.push_back(fx::block_bytes(b));
        }
        return v;
    }();
    return all;
}

Rebuilt rebuild(const VfsFile& file, std::uint64_t offset, std::uint64_t size)
{
    Rebuilt r;
    VfsSpans spans;
    r.status = file.resolve(offset, size, spans);
    if (r.status != VfsStatus::Ok) {
        return r;
    }
    r.total = spans.total_bytes();
    BlockSpan s;
    while (spans.next(s)) {
        r.spans.push_back(s);
        SW_CHECK(s.length > 0);
        SW_CHECK(s.block < blocks().size());
        SW_CHECK(std::uint64_t{s.offset} + s.length <= blocks()[s.block].size());
        const auto& data = blocks()[s.block];
        r.bytes.insert(r.bytes.end(), data.begin() + s.offset, data.begin() + s.offset + s.length);
    }
    SW_CHECK(!spans.next(s)); // depois do fim continua sendo o fim
    return r;
}

VfsFile open_file(const Vfs& vfs, std::string_view path)
{
    VfsFile f;
    SW_CHECK(vfs.open(path, f) == VfsStatus::Ok);
    SW_CHECK(f.valid());
    return f;
}

// O que a leitura deve entregar, calculado direto dos bytes originais do arquivo.
std::vector<std::uint8_t> expected_slice(const std::vector<std::uint8_t>& original,
                                         std::uint64_t offset, std::uint64_t size)
{
    SW_CHECK(offset <= original.size());
    const std::uint64_t n = std::min<std::uint64_t>(size, original.size() - offset);
    return {original.begin() + static_cast<std::ptrdiff_t>(offset),
            original.begin() + static_cast<std::ptrdiff_t>(offset + n)};
}

class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next()
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------- testes

void test_fixture_is_what_the_manifest_describes()
{
    std::cout << "[Test] fixture: o manifesto assinado descreve exatamente os blocos e arquivos "
                 "conhecidos\n";
    const pack::ManifestView view = open_manifest(fx::kMainManifest);
    SW_CHECK(view.block_count() == fx::kBlockRawSize.size());
    for (std::uint32_t b = 0; b < view.block_count(); ++b) {
        SW_CHECK(view.block_raw_size(b) == fx::kBlockRawSize[b]);
        const auto& data = blocks()[b];
        SW_CHECK(view.block_hash(b) == pack::sha256(std::as_bytes(std::span(data))));
    }
    const auto files = fx::files();
    SW_CHECK(view.file_count() == files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        SW_CHECK(view.file_path(i) == files[i].path);
        SW_CHECK(view.file_size(i) == fx::file_size_of(files[i]));
        SW_CHECK(view.file_ref_count(i) == files[i].refs.size());
    }
}

void test_whole_file_reconstruction()
{
    std::cout
        << "[Test] reconstrução: o arquivo inteiro, de cada arquivo da fixture, bate byte a byte\n";
    for (const fx::FileDef& def : fx::files()) {
        const VfsFile file = open_file(main_vfs(), def.path);
        const auto original = fx::file_bytes(def);
        SW_CHECK(file.size() == original.size());
        const Rebuilt r = rebuild(file, 0, file.size());
        SW_CHECK(r.status == VfsStatus::Ok);
        SW_CHECK(r.total == original.size());
        SW_CHECK(r.bytes == original);
        // Um trecho por referência de bloco, na ordem do arquivo (os blocos repetidos aparecem 2x).
        SW_CHECK(r.spans.size() == def.refs.size());
        for (std::size_t i = 0; i < def.refs.size(); ++i) {
            SW_CHECK(r.spans[i] == (BlockSpan{def.refs[i], 0, fx::kBlockRawSize[def.refs[i]]}));
        }
    }
}

void test_reads_inside_one_block()
{
    std::cout << "[Test] reconstrução: leituras dentro de um bloco\n";
    const VfsFile file = open_file(main_vfs(), "maps/level1.bin"); // blocos 1,2,3,4
    const auto original = fx::file_bytes(fx::files()[2]);
    // Bloco 2 ocupa [4096, 8192) do arquivo.
    const Rebuilt r = rebuild(file, 4096 + 10, 100);
    SW_CHECK(r.status == VfsStatus::Ok);
    SW_CHECK(r.spans.size() == 1);
    SW_CHECK(r.spans[0] == (BlockSpan{2, 10, 100}));
    SW_CHECK(r.bytes == expected_slice(original, 4096 + 10, 100));
    // Um único byte, o primeiro e o último do arquivo.
    SW_CHECK(rebuild(file, 0, 1).bytes == expected_slice(original, 0, 1));
    SW_CHECK(rebuild(file, original.size() - 1, 1).bytes ==
             expected_slice(original, original.size() - 1, 1));
}

void test_reads_crossing_blocks()
{
    std::cout << "[Test] reconstrução: leituras que cruzam 2 e 3+ blocos\n";
    const VfsFile file = open_file(main_vfs(), "maps/level1.bin"); // blocos 1,2,3,4
    const auto original = fx::file_bytes(fx::files()[2]);

    Rebuilt two = rebuild(file, 4090, 12); // fim do bloco 1 + começo do bloco 2
    SW_CHECK(two.spans.size() == 2);
    SW_CHECK(two.spans[0] == (BlockSpan{1, 4090, 6}));
    SW_CHECK(two.spans[1] == (BlockSpan{2, 0, 6}));
    SW_CHECK(two.bytes == expected_slice(original, 4090, 12));

    Rebuilt three = rebuild(file, 4000, 5000); // bloco 1 (resto), bloco 2 inteiro, começo do 3
    SW_CHECK(three.spans.size() == 3);
    SW_CHECK(three.spans[0] == (BlockSpan{1, 4000, 96}));
    SW_CHECK(three.spans[1] == (BlockSpan{2, 0, 4096}));
    SW_CHECK(three.spans[2] == (BlockSpan{3, 0, 808}));
    SW_CHECK(three.bytes == expected_slice(original, 4000, 5000));

    Rebuilt four =
        rebuild(file, 1, original.size() - 2); // os 4 blocos, sem o primeiro e o último byte
    SW_CHECK(four.spans.size() == 4);
    SW_CHECK(four.spans[0] == (BlockSpan{1, 1, 4095}));
    SW_CHECK(four.spans[3] == (BlockSpan{4, 0, 2999}));
    SW_CHECK(four.bytes == expected_slice(original, 1, original.size() - 2));

    // Bloco repetido no mesmo arquivo (textures/a.bin = blocos 2,2): o trecho vem do mesmo bloco
    // duas vezes.
    const VfsFile dup = open_file(main_vfs(), "textures/a.bin");
    const Rebuilt d = rebuild(dup, 4000, 200);
    SW_CHECK(d.spans.size() == 2);
    SW_CHECK(d.spans[0] == (BlockSpan{2, 4000, 96}));
    SW_CHECK(d.spans[1] == (BlockSpan{2, 0, 104}));
    SW_CHECK(d.bytes == expected_slice(fx::file_bytes(fx::files()[9]), 4000, 200));
}

void test_block_limit_boundaries()
{
    std::cout
        << "[Test] bordas: leitura que termina exatamente no limite de bloco e que começa nele\n";
    const VfsFile file =
        open_file(main_vfs(), "maps/level1.bin"); // blocos 1,2,3,4 (4096,4096,4096,3000)
    const auto original = fx::file_bytes(fx::files()[2]);

    Rebuilt ends = rebuild(file, 0, 4096); // termina no limite do bloco 1: sem trecho vazio depois
    SW_CHECK(ends.spans.size() == 1);
    SW_CHECK(ends.spans[0] == (BlockSpan{1, 0, 4096}));

    Rebuilt starts = rebuild(file, 4096, 1); // começa no limite do bloco 2
    SW_CHECK(starts.spans.size() == 1);
    SW_CHECK(starts.spans[0] == (BlockSpan{2, 0, 1}));

    Rebuilt exact2 = rebuild(file, 4096, 8192); // exatamente os blocos 2 e 3
    SW_CHECK(exact2.spans.size() == 2);
    SW_CHECK(exact2.spans[0] == (BlockSpan{2, 0, 4096}));
    SW_CHECK(exact2.spans[1] == (BlockSpan{3, 0, 4096}));

    Rebuilt tail = rebuild(file, 4096 * 3, 3000); // exatamente o último bloco, até o fim do arquivo
    SW_CHECK(tail.spans.size() == 1);
    SW_CHECK(tail.spans[0] == (BlockSpan{4, 0, 3000}));
    SW_CHECK(tail.bytes == expected_slice(original, 4096 * 3, 3000));
}

void test_every_file_against_random_reads()
{
    std::cout << "[Test] reconstrução: milhares de leituras (offset, size) por arquivo contra os "
                 "bytes originais\n";
    SplitMix64 rng(0xF117);
    for (const fx::FileDef& def : fx::files()) {
        const VfsFile file = open_file(main_vfs(), def.path);
        const auto original = fx::file_bytes(def);
        const std::uint64_t n = original.size();
        // Todas as combinações dos pontos "interessantes": limites de bloco e vizinhos.
        std::vector<std::uint64_t> marks = {0, 1, n, n ? n - 1 : 0};
        std::uint64_t edge = 0;
        for (const std::uint32_t b : def.refs) {
            edge += fx::kBlockRawSize[b];
            for (const std::int64_t d : {-1, 0, 1}) {
                const std::int64_t v = static_cast<std::int64_t>(edge) + d;
                if (v >= 0 && static_cast<std::uint64_t>(v) <= n)
                    marks.push_back(static_cast<std::uint64_t>(v));
            }
        }
        for (const std::uint64_t o : marks) {
            if (o > n) continue; // offset além do fim tem teste próprio (test_resolve_edges)
            for (const std::uint64_t s : marks) {
                const Rebuilt r = rebuild(file, o, s);
                SW_CHECK(r.status == VfsStatus::Ok);
                SW_CHECK(r.bytes == expected_slice(original, o, s));
                SW_CHECK(r.total == r.bytes.size());
            }
        }
        for (int i = 0; i < 3000; ++i) {
            const std::uint64_t o = n ? rng.next() % (n + 1) : 0;
            const std::uint64_t s = rng.next() % (n + 8192);
            const Rebuilt r = rebuild(file, o, s);
            SW_CHECK(r.status == VfsStatus::Ok);
            SW_CHECK(r.bytes == expected_slice(original, o, s));
        }
    }
}

void test_resolve_edges()
{
    std::cout
        << "[Test] bordas: EOF, offset além do fim, arquivo vazio, tamanho 0, estouro de uint64\n";
    const VfsFile file = open_file(main_vfs(), "config.ini"); // 1 bloco, 1000 bytes
    SW_CHECK(file.size() == 1000);
    const auto original = fx::file_bytes(fx::files()[0]);

    // offset == tamanho: Ok, 0 bytes, 0 trechos (para qualquer size).
    for (const std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{4096}, kMax}) {
        const Rebuilt r = rebuild(file, 1000, s);
        SW_CHECK(r.status == VfsStatus::Ok);
        SW_CHECK(r.total == 0);
        SW_CHECK(r.spans.empty());
    }
    // offset > tamanho: OutOfRange, também com size 0, e o cursor fica como estava.
    for (const std::uint64_t o : {std::uint64_t{1001}, std::uint64_t{1} << 40, kMax}) {
        for (const std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1}, kMax}) {
            VfsSpans spans;
            SW_CHECK(file.resolve(o, s, spans) == VfsStatus::OutOfRange);
            BlockSpan bs;
            SW_CHECK(!spans.next(bs));
            SW_CHECK(spans.total_bytes() == 0);
        }
    }
    // size 0 dentro do arquivo: Ok, 0 bytes.
    SW_CHECK(rebuild(file, 0, 0).total == 0);
    SW_CHECK(rebuild(file, 500, 0).spans.empty());

    // Passa do fim: leitura curta até o fim do arquivo (swarmstream.h: "leitura curta só no fim").
    const Rebuilt past = rebuild(file, 990, 100);
    SW_CHECK(past.status == VfsStatus::Ok);
    SW_CHECK(past.total == 10);
    SW_CHECK(past.bytes == expected_slice(original, 990, 100));

    // offset + size estoura uint64: nada de dar a volta.
    const Rebuilt huge = rebuild(file, 5, kMax);
    SW_CHECK(huge.status == VfsStatus::Ok);
    SW_CHECK(huge.total == 995);
    SW_CHECK(huge.bytes == expected_slice(original, 5, kMax));
    SW_CHECK(rebuild(file, 999, kMax).total == 1);
    SW_CHECK(rebuild(file, kMax - 1, 2).status == VfsStatus::OutOfRange);

    // Arquivo vazio (0 bytes, 0 referências).
    const VfsFile empty = open_file(main_vfs(), "empty.bin");
    SW_CHECK(empty.size() == 0);
    for (const std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1}, kMax}) {
        const Rebuilt r = rebuild(empty, 0, s);
        SW_CHECK(r.status == VfsStatus::Ok);
        SW_CHECK(r.total == 0);
        SW_CHECK(r.spans.empty());
    }
    SW_CHECK(rebuild(empty, 1, 0).status == VfsStatus::OutOfRange);
    SW_CHECK(rebuild(empty, 1, 1).status == VfsStatus::OutOfRange);
    SW_CHECK(rebuild(empty, kMax, kMax).status == VfsStatus::OutOfRange);
}

void test_paths()
{
    std::cout << "[Test] caminhos: inexistente, vazio, diretório, sem normalização\n";
    const Vfs& vfs = main_vfs();
    VfsFile f;
    VfsStat info;

    // Inexistente.
    SW_CHECK(vfs.open("nao/existe.bin", f) == VfsStatus::NotFound);
    SW_CHECK(!f.valid());
    SW_CHECK(vfs.stat("nao/existe.bin", info) == VfsStatus::NotFound);
    SW_CHECK(info.kind == VfsKind::None);

    // Sem normalização: só o caminho exato do manifesto existe.
    for (const char* p :
         {"/config.ini", "config.ini/", "Config.ini", "./config.ini", "maps//level1.bin",
          "maps/../config.ini", "maps\\level1.bin", "maps/", "/maps", "/", "//", "maps/level1",
          "maps/level1.bin/", "map", "maps/sub/", "texture"}) {
        SW_CHECK(vfs.open(p, f) == VfsStatus::NotFound);
        SW_CHECK(vfs.stat(p, info) == VfsStatus::NotFound);
    }
    // NUL no meio do caminho não é um caminho do manifesto.
    SW_CHECK(vfs.open(std::string_view("config.ini\0x", 12), f) == VfsStatus::NotFound);

    // Caminho vazio é a raiz: diretório.
    SW_CHECK(vfs.stat("", info) == VfsStatus::Ok);
    SW_CHECK(info.kind == VfsKind::Directory);
    SW_CHECK(info.size == 0);
    SW_CHECK(vfs.open("", f) == VfsStatus::IsDirectory);
    SW_CHECK(!f.valid());

    // Diretórios (existem por conterem arquivos).
    for (const char* p : {"maps", "maps/sub", "textures"}) {
        SW_CHECK(vfs.stat(p, info) == VfsStatus::Ok);
        SW_CHECK(info.kind == VfsKind::Directory);
        SW_CHECK(info.size == 0);
        SW_CHECK(vfs.open(p, f) == VfsStatus::IsDirectory);
        SW_CHECK(!f.valid());
    }
    // "sub" existe como diretório de maps, não na raiz.
    SW_CHECK(vfs.stat("sub", info) == VfsStatus::NotFound);

    // Prefixo de nome não é diretório: "maps/su" só prefixa nomes.
    SW_CHECK(vfs.stat("maps/su", info) == VfsStatus::NotFound);

    // Arquivos: tamanho, classe e grupo.
    SW_CHECK(vfs.stat("maps/level1.bin", info) == VfsStatus::Ok);
    SW_CHECK(info.kind == VfsKind::File);
    SW_CHECK(info.size == 4096 * 3 + 3000);
    SW_CHECK(info.file_class == pack::FileClass::Core && info.group == 0);
    SW_CHECK(vfs.stat("textures/g.bin", info) == VfsStatus::Ok);
    SW_CHECK(info.file_class == pack::FileClass::Group && info.group == 1 && info.size == 700);
    SW_CHECK(vfs.stat("textures/opt.bin", info) == VfsStatus::Ok);
    SW_CHECK(info.file_class == pack::FileClass::Optional && info.group == 1 && info.size == 1700);
    SW_CHECK(vfs.stat("empty.bin", info) == VfsStatus::Ok);
    SW_CHECK(info.kind == VfsKind::File && info.size == 0);
    // Caminho com byte não-ASCII (U+00E1 em UTF-8).
    SW_CHECK(vfs.stat("maps/\xC3\xA1"
                      "baco.bin",
                      info) == VfsStatus::Ok);
    SW_CHECK(info.kind == VfsKind::File && info.size == 4096);

    // open devolve o arquivo certo.
    SW_CHECK(vfs.open("maps/sub/deep.bin", f) == VfsStatus::Ok);
    SW_CHECK(f.valid() && f.path() == "maps/sub/deep.bin" && f.size() == 1000 + 1 + 4096);
}

struct Expected
{
    std::string name;
    VfsKind kind;
    std::uint64_t size;
};

// Listagem esperada, calculada à parte (std::map ordena por bytes) a partir da lista de arquivos.
std::vector<Expected> expected_listing(const std::string& dir)
{
    std::map<std::string, Expected> children;
    const std::string prefix = dir.empty() ? "" : dir + "/";
    for (const fx::FileDef& f : fx::files()) {
        if (f.path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = f.path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            children[rest] = {rest, VfsKind::File, fx::file_size_of(f)};
        } else {
            const std::string name = rest.substr(0, slash);
            children[name] = {name, VfsKind::Directory, 0};
        }
    }
    std::vector<Expected> out;
    for (auto& [name, e] : children)
        out.push_back(e);
    return out;
}

void test_directory_listing()
{
    std::cout << "[Test] diretório virtual: ordem por bytes UTF-8, só o nome, raiz \"\", fim\n";
    const Vfs& vfs = main_vfs();
    for (const char* dir : {"", "maps", "maps/sub", "textures"}) {
        VfsDir d;
        SW_CHECK(vfs.open_dir(dir, d) == VfsStatus::Ok);
        const auto want = expected_listing(dir);
        SW_CHECK(d.entry_count() == want.size());
        VfsDirEntry e;
        for (const Expected& w : want) {
            SW_CHECK(d.next(e));
            SW_CHECK(e.name == w.name);
            SW_CHECK(e.kind == w.kind);
            SW_CHECK(e.size == w.size);
        }
        SW_CHECK(!d.next(e)); // SW_DONE
        SW_CHECK(!d.next(e)); // e continua
    }

    // A ordem pedida no cartão, escrita à mão (não só a do oráculo): "sub" < "sub-1.bin" <
    // "sub.bin", embora no manifesto venham "sub-1.bin", "sub.bin" e só depois "sub/deep.bin"; o
    // nome com U+00E1 (bytes C3 A1) fica depois de "zeta.bin".
    VfsDir maps;
    SW_CHECK(vfs.open_dir("maps", maps) == VfsStatus::Ok);
    const std::vector<std::string> names = {"level1.bin",
                                            "level2.bin",
                                            "sub",
                                            "sub-1.bin",
                                            "sub.bin",
                                            "zeta.bin",
                                            "\xC3\xA1"
                                            "baco.bin"};
    VfsDirEntry e;
    for (const std::string& n : names) {
        SW_CHECK(maps.next(e));
        SW_CHECK(e.name == n);
        SW_CHECK((e.kind == VfsKind::Directory) == (n == "sub"));
    }
    SW_CHECK(!maps.next(e));

    // Raiz: dois arquivos e dois diretórios, só nomes (sem "maps/...").
    VfsDir root;
    SW_CHECK(vfs.open_dir("", root) == VfsStatus::Ok);
    const std::vector<std::pair<std::string, VfsKind>> top = {{"config.ini", VfsKind::File},
                                                              {"empty.bin", VfsKind::File},
                                                              {"maps", VfsKind::Directory},
                                                              {"textures", VfsKind::Directory}};
    for (const auto& [n, k] : top) {
        SW_CHECK(root.next(e));
        SW_CHECK(e.name == n && e.kind == k);
    }
    SW_CHECK(!root.next(e));

    // Classe e grupo das entradas.
    VfsDir tex;
    SW_CHECK(vfs.open_dir("textures", tex) == VfsStatus::Ok);
    SW_CHECK(tex.next(e) && e.name == "a.bin" && e.file_class == pack::FileClass::Core);
    SW_CHECK(tex.next(e) && e.name == "g.bin" && e.file_class == pack::FileClass::Group &&
             e.group == 1);
    SW_CHECK(tex.next(e) && e.name == "opt.bin" && e.file_class == pack::FileClass::Optional);
    SW_CHECK(!tex.next(e));

    // Erros: arquivo, inexistente, barra final, diretório que só existe como nome parcial.
    VfsDir bad;
    SW_CHECK(vfs.open_dir("config.ini", bad) == VfsStatus::NotDirectory);
    SW_CHECK(vfs.open_dir("maps/level1.bin", bad) == VfsStatus::NotDirectory);
    SW_CHECK(vfs.open_dir("nada", bad) == VfsStatus::NotFound);
    SW_CHECK(vfs.open_dir("maps/", bad) == VfsStatus::NotFound);
    SW_CHECK(vfs.open_dir("/maps", bad) == VfsStatus::NotFound);
    SW_CHECK(vfs.open_dir("map", bad) == VfsStatus::NotFound);
    SW_CHECK(vfs.open_dir("maps/su", bad) == VfsStatus::NotFound);
    SW_CHECK(bad.entry_count() == 0);
}

void test_empty_manifest_and_conflict()
{
    std::cout
        << "[Test] manifesto sem arquivos; conflito arquivo/diretório recusado na construção\n";
    const Vfs empty = build_vfs(fx::kEmptyManifest);
    VfsStat info;
    VfsFile f;
    VfsDir d;
    VfsDirEntry e;
    SW_CHECK(empty.stat("", info) == VfsStatus::Ok && info.kind == VfsKind::Directory);
    SW_CHECK(empty.open_dir("", d) == VfsStatus::Ok);
    SW_CHECK(d.entry_count() == 0 && !d.next(e));
    SW_CHECK(empty.open("x", f) == VfsStatus::NotFound);
    SW_CHECK(empty.open_dir("x", d) == VfsStatus::NotFound);

    Vfs conflict;
    SW_CHECK(Vfs::build(open_manifest(fx::kConflictManifest), conflict) ==
             VfsBuildStatus::FileDirectoryConflict);
    // O manifesto principal tem "maps/sub.bin" e "maps/sub/deep.bin", mas nenhum "maps/sub": sem
    // conflito.
    Vfs ok;
    SW_CHECK(Vfs::build(open_manifest(fx::kMainManifest), ok) == VfsBuildStatus::Ok);
}

void test_resolve_does_not_allocate()
{
    std::cout << "[Test] sem alocação: stat, open, resolve e next (com controle positivo)\n";
    const Vfs& vfs = main_vfs();
    (void)blocks(); // calcula os blocos antes de contar
    const VfsFile big = open_file(vfs, "maps/level1.bin");
    const VfsFile dedup = open_file(vfs, "textures/a.bin");
    const VfsFile empty = open_file(vfs, "empty.bin");

    g_allocations.store(0);
    g_counting.store(true);
    std::uint64_t checksum = 0;
    for (int i = 0; i < 2000; ++i) {
        VfsStat info;
        VfsFile f;
        SW_CHECK(vfs.stat("maps/sub/deep.bin", info) == VfsStatus::Ok);
        SW_CHECK(vfs.open("maps/level2.bin", f) == VfsStatus::Ok);
        SW_CHECK(vfs.open("maps/sub", f) == VfsStatus::IsDirectory);
        SW_CHECK(vfs.open("nao/existe", f) == VfsStatus::NotFound);
        for (const VfsFile* file : {&big, &dedup, &empty}) {
            VfsSpans spans;
            const std::uint64_t offset = static_cast<std::uint64_t>(i) % (file->size() + 1);
            SW_CHECK(file->resolve(offset, 9000, spans) == VfsStatus::Ok);
            BlockSpan s;
            while (spans.next(s)) {
                checksum += s.block + s.offset + s.length;
            }
        }
        VfsSpans bad;
        SW_CHECK(big.resolve(kMax, 1, bad) == VfsStatus::OutOfRange);
    }
    g_counting.store(false);
    const std::uint64_t during = g_allocations.load();
    std::cout << "        alocações durante o caminho quente: " << during << " (checksum "
              << checksum << ")\n";
    SW_CHECK(checksum != 0);
    SW_CHECK(during == 0);

    // Controle positivo: se o contador enxerga um `new` de verdade, o zero acima significa algo.
    g_allocations.store(0);
    g_counting.store(true);
    g_sink = ::operator new(16);
    g_counting.store(false);
    SW_CHECK(g_allocations.load() == 1);
    ::operator delete(g_sink);
    g_sink = nullptr;

    // E um open_dir (que aloca a lista de filhos) é visto pelo contador: não é caminho quente.
    g_allocations.store(0);
    g_counting.store(true);
    VfsDir d;
    SW_CHECK(vfs.open_dir("maps", d) == VfsStatus::Ok);
    g_counting.store(false);
    SW_CHECK(g_allocations.load() > 0);
}

void test_concurrent_resolves()
{
    std::cout << "[Test] concorrência: 8 threads resolvendo e reconstruindo ao mesmo tempo\n";
    const Vfs& vfs = main_vfs();
    (void)blocks();
    const auto defs = fx::files();
    std::vector<std::vector<std::uint8_t>> originals;
    for (const auto& d : defs)
        originals.push_back(fx::file_bytes(d));
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            SplitMix64 rng(1000 + t);
            for (int i = 0; i < 3000; ++i) {
                const std::size_t k = rng.next() % defs.size();
                VfsFile f;
                if (vfs.open(defs[k].path, f) != VfsStatus::Ok) {
                    ++failures;
                    continue;
                }
                const std::uint64_t n = originals[k].size();
                const std::uint64_t o = rng.next() % (n + 1);
                const std::uint64_t s = rng.next() % (n + 100);
                VfsSpans spans;
                if (f.resolve(o, s, spans) != VfsStatus::Ok) {
                    ++failures;
                    continue;
                }
                std::vector<std::uint8_t> got;
                BlockSpan bs;
                while (spans.next(bs)) {
                    const auto& data = blocks()[bs.block];
                    got.insert(got.end(), data.begin() + bs.offset,
                               data.begin() + bs.offset + bs.length);
                }
                if (got != expected_slice(originals[k], o, s)) ++failures;
                VfsDir d;
                if (vfs.open_dir("maps", d) != VfsStatus::Ok || d.entry_count() != 7) ++failures;
            }
        });
    }
    for (auto& th : threads)
        th.join();
    SW_CHECK(failures.load() == 0);
}

} // namespace

int main()
{
    test_fixture_is_what_the_manifest_describes();
    test_whole_file_reconstruction();
    test_reads_inside_one_block();
    test_reads_crossing_blocks();
    test_block_limit_boundaries();
    test_every_file_against_random_reads();
    test_resolve_edges();
    test_paths();
    test_directory_listing();
    test_empty_manifest_and_conflict();
    test_resolve_does_not_allocate();
    test_concurrent_resolves();
    std::cout << "VFS: todos os testes passaram\n";
    return 0;
}
