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
//
// F1-64: reforço (fim do arquivo): byte >= 0x80 depois do prefixo de diretório, bloco de 262144
// bytes e offsets em torno de 2^32 (arquivos lógicos de mais de 4 GiB descritos só pelo manifesto,
// vfs_fixture_big.hpp). Os resultados esperados são literais calculados à mão ou saem de um oráculo
// linear que não usa nada do Vfs; o verificador fora do C++ é
// sw-pack/testdata/verify_manifest_independent.py (ver o cartão F1-64).
//
// F1-63: o mesmo `operator new` ganha injeção de falha (`g_fail_countdown`): enquanto a contagem
// está ligada, a k-ésima alocação (k = 0, 1, ...) lança std::bad_alloc. Os testes de falha de
// `Vfs::build` varrem todos os k de cada manifesto (tabela de índices, pilha de prefixos) com `out`
// vazio e com `out` já válido, e provam que `out` fica exatamente como estava.
#include "check.hpp"
#include "swarmstream/vfs.hpp"
#include "vfs_fixture.hpp"
#include "vfs_fixture_big.hpp"
#include "vfs_fixture_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <crtdbg.h>
#include <windows.h>
#endif

using namespace swarmstream;
namespace fx = swarmstream::test::vfs_fixture;

// ---------------------------------------------------------------- contagem de alocações
namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};
// < 0: sem injeção. >= 0: a alocação contada em que este valor chega a 0 lança bad_alloc.
std::atomic<std::int64_t> g_fail_countdown{-1};
void* volatile g_sink = nullptr;
// Chamada opaca (ponteiro volátil): o objeto passado para ela existe de verdade, então o compilador
// não pode elidir a alocação de um objeto construído e logo destruído (visto em Release).
void observe_object(const void*) {}
void (*volatile g_escape)(const void*) = &observe_object;
// Tamanho das primeiras alocações contadas (preenchido sem alocar; só threads de um teste por vez).
constexpr std::size_t kTrackedAllocs = 64;
std::size_t g_alloc_sizes[kTrackedAllocs];
} // namespace

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed)) {
        const std::uint64_t idx = g_allocations.fetch_add(1, std::memory_order_relaxed);
        if (idx < kTrackedAllocs) {
            g_alloc_sizes[idx] = n;
        }
        if (g_fail_countdown.fetch_sub(1, std::memory_order_relaxed) == 0) {
            throw std::bad_alloc();
        }
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
    SW_CHECK(vfs.index_size() == vfs.manifest().block_ref_count());
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
    // Só para leituras DENTRO do arquivo (offset + size <= tamanho; escrito sem somar).
    SW_CHECK(offset <= original.size() && size <= original.size() - offset);
    return {original.begin() + static_cast<std::ptrdiff_t>(offset),
            original.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

// O que resolve() deve responder (decisão do dono, 2026-10-29): offset > tamanho -> OutOfRange;
// offset == tamanho -> Ok com 0 bytes (qualquer size); antes do fim, size > tamanho - offset ->
// OutOfRange (tudo ou nada); senão Ok. Calculado sem somar offset + size.
VfsStatus expected_status(std::uint64_t file_size, std::uint64_t offset, std::uint64_t size)
{
    if (offset > file_size) return VfsStatus::OutOfRange;
    if (offset == file_size) return VfsStatus::Ok;
    return size > file_size - offset ? VfsStatus::OutOfRange : VfsStatus::Ok;
}

// Bytes esperados quando o status é Ok.
std::vector<std::uint8_t> expected_bytes(const std::vector<std::uint8_t>& original,
                                         std::uint64_t offset, std::uint64_t size)
{
    if (offset == original.size()) return {};
    return expected_slice(original, offset, size);
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
        // Cada (offset, size) responde como manda a decisão: Ok com os bytes certos, ou OutOfRange
        // sem nenhum trecho (inclusive quando size passa do fim por 1 byte).
        const auto check = [&](std::uint64_t o, std::uint64_t sz) {
            const Rebuilt r = rebuild(file, o, sz);
            SW_CHECK(r.status == expected_status(n, o, sz));
            if (r.status == VfsStatus::Ok) {
                SW_CHECK(r.bytes == expected_bytes(original, o, sz));
                SW_CHECK(r.total == r.bytes.size());
            } else {
                SW_CHECK(r.spans.empty() && r.bytes.empty());
            }
        };
        for (const std::uint64_t o : marks) {
            for (const std::uint64_t sz : marks) {
                check(o, sz);
            }
            check(o, n - std::min(o, n)); // exatamente até o fim: Ok
            check(o,
                  n - std::min(o, n) + 1); // 1 byte além do fim: OutOfRange (se offset < tamanho)
        }
        for (int i = 0; i < 3000; ++i) {
            const std::uint64_t o = n ? rng.next() % (n + 1) : 0;
            check(o, rng.next() % (n + 8192));
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
    // Um cursor já preenchido por uma leitura boa NÃO pode ser tocado por uma leitura que falha
    // ("destino intocado"): é o que o F1-60 vê quando recebe OutOfRange.
    VfsSpans kept;
    SW_CHECK(file.resolve(10, 100, kept) == VfsStatus::Ok);
    const auto still_intact = [&] {
        VfsSpans copy = kept;
        BlockSpan bs;
        SW_CHECK(kept.total_bytes() == 100);
        SW_CHECK(copy.next(bs) && bs == (BlockSpan{0u, 10, 100}));
        SW_CHECK(!copy.next(bs));
    };
    still_intact();

    // offset > tamanho: OutOfRange, também com size 0, e o cursor fica como estava.
    for (const std::uint64_t o : {std::uint64_t{1001}, std::uint64_t{1} << 40, kMax}) {
        for (const std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1}, kMax}) {
            SW_CHECK(file.resolve(o, s, kept) == VfsStatus::OutOfRange);
            still_intact();
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

    // Passa do fim com offset < tamanho: OutOfRange, nada é entregue (decisão do dono, 2026-10-29).
    for (const auto& [o, s] : {std::pair<std::uint64_t, std::uint64_t>{990, 100},
                               {5, kMax},
                               {999, 2},
                               {0, 1001},
                               {1, 1000},
                               {999, kMax},
                               {kMax - 1, 2}}) {
        SW_CHECK(rebuild(file, o, s).status == VfsStatus::OutOfRange);
        SW_CHECK(file.resolve(o, s, kept) == VfsStatus::OutOfRange);
        still_intact();
    }
    // O limite exato é Ok: size == tamanho - offset (e um byte a mais já não é).
    for (const std::uint64_t o :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{500}, std::uint64_t{999}}) {
        const Rebuilt exact = rebuild(file, o, 1000 - o);
        SW_CHECK(exact.status == VfsStatus::Ok && exact.total == 1000 - o);
        SW_CHECK(exact.bytes == expected_slice(original, o, 1000 - o));
        SW_CHECK(rebuild(file, o, 1000 - o + 1).status == VfsStatus::OutOfRange);
    }
    SW_CHECK(rebuild(file, 990, 10).bytes == expected_slice(original, 990, 10));
    // offset == tamanho continua Ok com 0 bytes, mesmo com size enorme (nunca soma offset + size).
    SW_CHECK(rebuild(file, 1000, kMax).status == VfsStatus::Ok);

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

// Definição: há conflito arquivo/diretório se existem, no manifesto, um caminho P e outro caminho
// que começa com P seguido de "/" (P é arquivo e também diretório). Prefixo SEM barra ("a" / "ab",
// "a" / "a-1") não é conflito. Todos os manifestos abaixo passam pelo ManifestView::open (o §8 não
// recusa nenhum deles).
void test_file_directory_conflict_definition()
{
    std::cout << "[Test] conflito arquivo/diretório: \"a\" vs \"a/b\" é conflito; \"a\" vs \"ab\", "
                 "\"a-1\", \"a.x\", \"a0\" não\n";
    Vfs vfs;
    // "a" < "a-1" < "a.x" < "a/b": os vizinhos que ficam entre "a" e "a/b" na ordem bytewise não
    // escondem o conflito.
    SW_CHECK(Vfs::build(open_manifest(fx::kConflictManifest), vfs) ==
             VfsBuildStatus::FileDirectoryConflict);
    // O mesmo conflito num subdiretório: "x/a" e "x/a/b" (com "x/a-1" no meio e "x/b" depois).
    SW_CHECK(Vfs::build(open_manifest(fx::kNestedConflictManifest), vfs) ==
             VfsBuildStatus::FileDirectoryConflict);

    // Sem conflito: "a" é prefixo de "a-1", "a.x", "a0" e "ab" sem barra; "d/e" de "d/e-1" e
    // "d/ef". O manifesto é aceito e "a" continua sendo só arquivo.
    Vfs prefix;
    SW_CHECK(Vfs::build(open_manifest(fx::kPrefixManifest), prefix) == VfsBuildStatus::Ok);
    VfsStat info;
    for (const char* p : {"a", "a-1", "a.x", "a0", "ab", "d/e", "d/e-1", "d/ef"}) {
        SW_CHECK(prefix.stat(p, info) == VfsStatus::Ok && info.kind == VfsKind::File);
    }
    SW_CHECK(prefix.stat("d", info) == VfsStatus::Ok && info.kind == VfsKind::Directory);
    VfsDir dir;
    SW_CHECK(prefix.open_dir("a", dir) == VfsStatus::NotDirectory);
    SW_CHECK(prefix.open_dir("d/e", dir) == VfsStatus::NotDirectory);
    SW_CHECK(prefix.stat("a/", info) == VfsStatus::NotFound);
    SW_CHECK(prefix.open_dir("", dir) == VfsStatus::Ok);
    const std::vector<std::pair<std::string, VfsKind>> root = {
        {"a", VfsKind::File},  {"a-1", VfsKind::File}, {"a.x", VfsKind::File},
        {"a0", VfsKind::File}, {"ab", VfsKind::File},  {"d", VfsKind::Directory}};
    VfsDirEntry e;
    for (const auto& [name, kind] : root) {
        SW_CHECK(dir.next(e) && e.name == name && e.kind == kind);
    }
    SW_CHECK(!dir.next(e));
}

// ---------------------------------------------------------------- F1-63: garantia de build

// Tudo o que o Vfs responde sobre os caminhos de TODAS as fixtures, em texto: stat, open, resolve
// (inteiro, a partir de 1 e no fim) com os trechos, open_dir com as entradas. Dois Vfs com o mesmo
// texto respondem igual a estas consultas. Só chamar com um Vfs coerente (ver `coherent`).
std::string observe(const Vfs& vfs)
{
    static const char* const kProbes[] = {"",
                                          "config.ini",
                                          "empty.bin",
                                          "maps",
                                          "maps/level1.bin",
                                          "maps/level2.bin",
                                          "maps/sub",
                                          "maps/sub.bin",
                                          "maps/sub/deep.bin",
                                          "textures",
                                          "textures/a.bin",
                                          "a",
                                          "a-1",
                                          "a.x",
                                          "a0",
                                          "ab",
                                          "a/b",
                                          "d",
                                          "d/e",
                                          "d/e-1",
                                          "d/ef",
                                          "x",
                                          "x/a",
                                          "x/a/b",
                                          "x/b",
                                          "nada",
                                          "maps/"};
    std::ostringstream o;
    o << "files=" << vfs.manifest().file_count() << " refs=" << vfs.manifest().block_ref_count()
      << "\n";
    for (const char* path : kProbes) {
        VfsStat info;
        const VfsStatus st = vfs.stat(path, info);
        o << "[" << path << "] stat=" << int(st) << "/" << int(info.kind) << "/" << info.size << "/"
          << int(info.file_class) << "/" << info.group;
        VfsFile f;
        const VfsStatus os = vfs.open(path, f);
        o << " open=" << int(os);
        if (os == VfsStatus::Ok) {
            const std::uint64_t n = f.size();
            o << " size=" << n << " path=" << f.path();
            for (const std::uint64_t off : {std::uint64_t{0}, std::uint64_t{1}, n}) {
                VfsSpans spans;
                const VfsStatus rs = f.resolve(off, off <= n ? n - off : 0, spans);
                o << " r" << off << "=" << int(rs);
                if (rs == VfsStatus::Ok) {
                    o << ":" << spans.total_bytes();
                    BlockSpan s;
                    while (spans.next(s)) {
                        o << "(" << s.block << "," << s.offset << "," << s.length << ")";
                    }
                }
            }
        }
        VfsDir dir;
        const VfsStatus ds = vfs.open_dir(path, dir);
        o << " dir=" << int(ds);
        if (ds == VfsStatus::Ok) {
            VfsDirEntry e;
            while (dir.next(e)) {
                o << " " << e.name << ":" << int(e.kind) << ":" << e.size;
            }
        }
        o << "\n";
    }
    return o.str();
}

// A visão e a tabela de `vfs` batem com as de `reference`? Conferido SEM consultar o caminho
// (`observe`), para que um estado incoerente (visão nova com tabela vazia) seja pego aqui e não por
// um acesso fora dos limites lá dentro.
bool coherent(const Vfs& vfs, const Vfs& reference)
{
    // Invariante do Vfs: uma entrada de índice por referência de bloco da visão, nos dois.
    return vfs.index_size() == vfs.manifest().block_ref_count() &&
           reference.index_size() == reference.manifest().block_ref_count() &&
           vfs.index_size() == reference.index_size() &&
           vfs.manifest().file_count() == reference.manifest().file_count() &&
           vfs.manifest().block_ref_count() == reference.manifest().block_ref_count() &&
           vfs.manifest().block_count() == reference.manifest().block_count();
}

struct Source
{
    const char* name;
    std::span<const std::uint8_t> bytes;
    VfsBuildStatus expected;
};

const std::array<Source, 5>& sources()
{
    static const std::array<Source, 5> all = {{
        {"principal", fx::kMainManifest, VfsBuildStatus::Ok},
        {"prefixos", fx::kPrefixManifest, VfsBuildStatus::Ok},
        {"vazio", fx::kEmptyManifest, VfsBuildStatus::Ok},
        {"conflito", fx::kConflictManifest, VfsBuildStatus::FileDirectoryConflict},
        {"conflito aninhado", fx::kNestedConflictManifest, VfsBuildStatus::FileDirectoryConflict},
    }};
    return all;
}

// Roda `Vfs::build` com falha de alocação na k-ésima alocação (k < 0: sem falha) e devolve o status
// e quantas alocações o build fez.
VfsBuildStatus build_with_failure(const pack::ManifestView& view, Vfs& out, std::int64_t k,
                                  std::uint64_t& allocations)
{
    g_allocations.store(0);
    g_fail_countdown.store(k);
    g_counting.store(true);
    const VfsBuildStatus st = Vfs::build(view, out);
    g_counting.store(false);
    g_fail_countdown.store(-1);
    allocations = g_allocations.load();
    return st;
}

// Os trechos de um arquivo, de ponta a ponta, como texto.
std::string spans_text(const VfsFile& f)
{
    VfsSpans spans;
    SW_CHECK(f.resolve(0, f.size(), spans) == VfsStatus::Ok);
    std::ostringstream o;
    BlockSpan s;
    while (spans.next(s)) {
        o << "(" << s.block << "," << s.offset << "," << s.length << ")";
    }
    return o.str();
}

void test_build_failure_keeps_out_untouched()
{
    std::cout << "[Test] build: em QUALQUER falha (conflito, alocação em cada etapa) `out` fica "
                 "exatamente como estava (vazio ou já válido)\n";
    static_assert(std::is_nothrow_move_assignable_v<Vfs>,
                  "a publicação em `out` não pode falhar depois da validação");
    const Source& main_src = sources()[0];
    const Source& prefix_src = sources()[1];
    // Estado inicial do `out`: nullptr = vazio (Vfs{}); senão, já construído a partir do manifesto.
    const std::array<const Source*, 3> initials = {nullptr, &main_src, &prefix_src};

    std::uint64_t oom_seen = 0;
    std::uint64_t conflict_seen = 0;
    for (const Source& src : sources()) {
        const pack::ManifestView view = open_manifest(src.bytes);
        // O que um build bem-sucedido deste manifesto responde.
        Vfs fresh;
        std::string fresh_text;
        if (src.expected == VfsBuildStatus::Ok) {
            SW_CHECK(Vfs::build(view, fresh) == VfsBuildStatus::Ok);
            SW_CHECK(fresh.index_size() == view.block_ref_count()); // antes de consultar
            fresh_text = observe(fresh);
        }

        // Quantas alocações este build faz até terminar (ou até achar o conflito).
        Vfs probe;
        std::uint64_t total = 0;
        SW_CHECK(build_with_failure(view, probe, -1, total) == src.expected);
        std::cout << "        " << src.name << ": " << total << " alocações no build\n";
        // O varrimento de k abaixo falha CADA alocação que o build fez de fato nesta STL (a cópia
        // da visão, se a std::string `chunker.algorithm` não couber no SSO; a tabela de índices; o
        // crescimento da pilha de prefixos; o que mais a STL alocar). Nenhum mínimo depende da
        // implementação, só o que é inevitável: guardar N > 0 entradas de 64 bits exige memória
        // dinâmica, então a tabela de um manifesto com referências de bloco é pelo menos 1
        // alocação.
        if (view.block_ref_count() > 0) {
            SW_CHECK(total >= 1);
        }

        for (const Source* init : initials) {
            for (std::int64_t k = 0; k <= static_cast<std::int64_t>(total); ++k) {
                Vfs out;
                Vfs reference;
                if (init != nullptr) {
                    SW_CHECK(Vfs::build(open_manifest(init->bytes), out) == VfsBuildStatus::Ok);
                    SW_CHECK(Vfs::build(open_manifest(init->bytes), reference) ==
                             VfsBuildStatus::Ok);
                }
                const std::string before = observe(out);

                // Arquivo, cursor e diretório abertos ANTES da tentativa (só com `out` válido).
                VfsFile held;
                VfsSpans cursor;
                VfsDir held_dir;
                std::vector<BlockSpan> cursor_rest;
                std::string held_text;
                if (init != nullptr) {
                    held = open_file(out, init == &main_src ? "maps/level1.bin" : "a");
                    held_text = spans_text(held);
                    SW_CHECK(held.resolve(0, held.size(), cursor) == VfsStatus::Ok);
                    BlockSpan first;
                    SW_CHECK(cursor.next(first)); // o cursor já andou um trecho
                    VfsSpans ahead = cursor;
                    BlockSpan s;
                    while (ahead.next(s)) {
                        cursor_rest.push_back(s);
                    }
                    SW_CHECK(out.open_dir("", held_dir) == VfsStatus::Ok);
                }

                std::uint64_t used = 0;
                const VfsBuildStatus st = build_with_failure(view, out, k, used);

                if (static_cast<std::uint64_t>(k) < total) {
                    SW_CHECK(st == VfsBuildStatus::OutOfMemory); // a k-ésima alocação existe
                    ++oom_seen;
                } else {
                    SW_CHECK(st == src.expected);
                    if (st == VfsBuildStatus::FileDirectoryConflict) {
                        ++conflict_seen;
                    }
                }

                if (st == VfsBuildStatus::Ok) {
                    // Sucesso: `out` passa a responder como um build novo do manifesto.
                    SW_CHECK(coherent(out, fresh));
                    SW_CHECK(observe(out) == fresh_text);
                    continue;
                }

                // Falha: `out` é o que era. Primeiro os contadores, depois as consultas completas.
                SW_CHECK(coherent(out, reference));
                SW_CHECK(observe(out) == before);
                if (init != nullptr) {
                    // Os handles abertos antes continuam valendo, com os mesmos resultados.
                    SW_CHECK(spans_text(held) == held_text);
                    std::vector<BlockSpan> rest;
                    BlockSpan s;
                    while (cursor.next(s)) {
                        rest.push_back(s);
                    }
                    SW_CHECK(rest == cursor_rest);
                    VfsDirEntry e;
                    std::size_t listed = 0;
                    while (held_dir.next(e)) {
                        ++listed;
                    }
                    SW_CHECK(listed == held_dir.entry_count() && listed > 0);
                }
            }
        }
    }
    SW_CHECK(oom_seen > 0 && conflict_seen > 0);
}

// `build(out.manifest(), out)`: a visão de origem é a do próprio `out`. Em falha, nada muda; em
// sucesso, o Vfs responde igual (a tabela foi refeita).
void test_build_failure_when_source_is_the_destination()
{
    std::cout << "[Test] build: reconstruir `out` a partir do próprio manifesto (alias), com e sem "
                 "falha de alocação\n";
    std::uint64_t total = 0;
    Vfs probe;
    SW_CHECK(build_with_failure(open_manifest(fx::kMainManifest), probe, -1, total) ==
             VfsBuildStatus::Ok);
    SW_CHECK(total >= 1);
    for (std::int64_t k = 0; k <= static_cast<std::int64_t>(total); ++k) {
        Vfs out = build_vfs(fx::kMainManifest);
        const std::string before = observe(out);
        std::uint64_t used = 0;
        const VfsBuildStatus st = build_with_failure(out.manifest(), out, k, used);
        SW_CHECK(st == (static_cast<std::uint64_t>(k) < total ? VfsBuildStatus::OutOfMemory
                                                              : VfsBuildStatus::Ok));
        SW_CHECK(observe(out) == before);
    }
}

// Reconstrução BEM-SUCEDIDA sobre um Vfs com handles abertos (desenho do F1-63, vfs.hpp):
//  - VfsFile e VfsSpans abertos antes ficam INVÁLIDOS (apontam para o Vfs, cujo conteúdo foi
//    trocado): não se usam depois, e `valid()` NÃO detecta isso. O teste só faz o que é definido:
//    reabrir depois e conferir que o novo estado é o do manifesto novo.
//  - VfsDir NÃO depende do Vfs: guarda cópias das entradas, que apontam para os bytes do manifesto
//    ANTIGO, e continua valendo enquanto esses bytes viverem.
//  - Handles pertencem ao OBJETO Vfs onde foram abertos: uma cópia do Vfs tem os seus.
void test_successful_rebuild_effect_on_open_handles()
{
    std::cout << "[Test] build: efeito de uma reconstrução bem-sucedida sobre handles abertos "
                 "(VfsDir segue valendo; VfsFile/VfsSpans antigos são descartados e reabertos)\n";
    Vfs out = build_vfs(fx::kMainManifest);
    const Vfs main_reference = build_vfs(fx::kMainManifest);
    const Vfs prefix_reference = build_vfs(fx::kPrefixManifest);

    VfsDir dir_before;
    SW_CHECK(out.open_dir("maps", dir_before) == VfsStatus::Ok);
    const std::size_t entries_before = dir_before.entry_count();
    SW_CHECK(entries_before == 7);
    VfsDirEntry first_before;
    SW_CHECK(dir_before.next(first_before)); // o cursor de diretório já andou uma entrada
    const std::string first_name(first_before.name);
    const VfsFile old_file = open_file(out, "maps/level1.bin");

    SW_CHECK(Vfs::build(open_manifest(fx::kPrefixManifest), out) == VfsBuildStatus::Ok);

    // O novo estado é o do manifesto novo, e só dele.
    SW_CHECK(observe(out) == observe(prefix_reference));
    SW_CHECK(observe(out) != observe(main_reference));
    VfsFile f;
    SW_CHECK(out.open("maps/level1.bin", f) == VfsStatus::NotFound);
    SW_CHECK(!f.valid());
    SW_CHECK(open_file(out, "d/e").valid());

    // VfsDir{} default é uma listagem vazia (definido no header).
    VfsDir none;
    VfsDirEntry none_entry;
    SW_CHECK(none.entry_count() == 0 && !none.next(none_entry));

    // VfsDir: independente do Vfs. Continua de onde estava, com as mesmas entradas.
    SW_CHECK(dir_before.entry_count() == entries_before);
    std::size_t remaining = 0;
    VfsDirEntry e;
    while (dir_before.next(e)) {
        SW_CHECK(!e.name.empty() && e.name != first_name);
        ++remaining;
    }
    SW_CHECK(remaining == entries_before - 1);

    // `valid()` só diz que o handle foi preenchido por um open; NÃO diz que o Vfs não foi
    // reconstruído desde então. Por isso o handle antigo é descartado e reaberto.
    SW_CHECK(old_file.valid());

    // De volta ao principal: handles reabertos respondem como no Vfs de referência.
    SW_CHECK(Vfs::build(open_manifest(fx::kMainManifest), out) == VfsBuildStatus::Ok);
    SW_CHECK(observe(out) == observe(main_reference));
    SW_CHECK(spans_text(open_file(out, "maps/level1.bin")) ==
             spans_text(open_file(main_reference, "maps/level1.bin")));

    // Cópia do Vfs: handles abertos na cópia valem enquanto a CÓPIA viver, mesmo depois que o
    // original é reconstruído e destruído; os do original não migram para a cópia.
    auto* original = new Vfs;
    SW_CHECK(Vfs::build(open_manifest(fx::kMainManifest), *original) == VfsBuildStatus::Ok);
    const Vfs copy = *original;
    const VfsFile on_copy = open_file(copy, "maps/level1.bin");
    SW_CHECK(Vfs::build(open_manifest(fx::kPrefixManifest), *original) == VfsBuildStatus::Ok);
    delete original;
    SW_CHECK(spans_text(on_copy) == spans_text(open_file(main_reference, "maps/level1.bin")));
}

// Cópia da visão POR CONSTRUÇÃO (o desenho de `Vfs::build`: `view_(view)` no construtor privado).
// A alocação da `std::string chunker.algorithm` ("swarmstream-fastcdc-v0", 22 bytes) depende da
// STL: aloca na libstdc++ e no MSVC (SSO de 15 bytes; no MSVC Debug ainda há o proxy do iterador),
// não aloca na libc++ (SSO de 22). Onde aloca, a falha TEM de ser demonstrada, e o k é medido
// DENTRO do build (não vale o da cópia isolada: as contagens do MSVC Debug são outras): as
// primeiras alocações do build têm de ter os mesmos tamanhos, na mesma ordem, que as da cópia
// por construção. Onde não aloca, o teste DECLARA e não finge cobrir.
void test_build_view_copy_stage()
{
    std::cout
        << "[Test] build: falha de alocação na cópia da visão por construção (injeção direta, "
           "tamanhos conferidos DENTRO do build)\n";
    const pack::ManifestView view = open_manifest(fx::kMainManifest);

    // Alocações da cópia por construção, isoladas.
    std::size_t copy_sizes[kTrackedAllocs] = {};
    std::uint64_t copy_allocs = 0;
    {
        g_allocations.store(0);
        g_fail_countdown.store(-1);
        g_counting.store(true);
        alignas(pack::ManifestView) unsigned char storage[sizeof(pack::ManifestView)];
        auto* copy = new (storage) pack::ManifestView(view);
        g_escape(copy);
        g_counting.store(false);
        copy->~ManifestView();
        copy_allocs = g_allocations.load();
        for (std::uint64_t i = 0; i < copy_allocs && i < kTrackedAllocs; ++i) {
            copy_sizes[i] = g_alloc_sizes[i];
        }
    }
    std::cout << "        a cópia do ManifestView por construção faz " << copy_allocs
              << " alocação(ões) nesta STL";
    for (std::uint64_t i = 0; i < copy_allocs && i < kTrackedAllocs; ++i) {
        std::cout << (i == 0 ? " (tamanhos: " : ", ") << copy_sizes[i];
    }
    std::cout << (copy_allocs > 0 ? ")\n" : "\n");
    if (copy_allocs == 0) {
        std::cout
            << "        nesta STL a cópia da visão não aloca (algorithm cabe no SSO): a etapa "
               "não tem ponto de falha; DECLARADO, nada a injetar\n";
        return;
    }
    SW_CHECK(copy_allocs <= kTrackedAllocs);

    // Dentro do build: as primeiras `copy_allocs` alocações são as da cópia (mesmos tamanhos).
    std::uint64_t total = 0;
    Vfs probe;
    SW_CHECK(build_with_failure(view, probe, -1, total) == VfsBuildStatus::Ok);
    SW_CHECK(total > copy_allocs); // depois da cópia ainda há a tabela e a pilha
    for (std::uint64_t i = 0; i < copy_allocs; ++i) {
        SW_CHECK(g_alloc_sizes[i] == copy_sizes[i]);
    }

    for (std::int64_t k = 0; k < static_cast<std::int64_t>(copy_allocs); ++k) {
        // Controle positivo: a injeção dispara DENTRO da cópia por construção.
        bool threw = false;
        g_allocations.store(0);
        g_fail_countdown.store(k);
        g_counting.store(true);
        try {
            alignas(pack::ManifestView) unsigned char storage[sizeof(pack::ManifestView)];
            auto* victim = new (storage) pack::ManifestView(view);
            g_escape(victim);
            victim->~ManifestView(); // não deve chegar aqui
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        g_counting.store(false);
        g_fail_countdown.store(-1);
        SW_CHECK(threw);

        // O mesmo k no build: OutOfMemory, sem terminate, com out vazio e com out já válido.
        std::uint64_t used = 0;
        Vfs empty_out;
        SW_CHECK(build_with_failure(view, empty_out, k, used) == VfsBuildStatus::OutOfMemory);
        SW_CHECK(empty_out.manifest().file_count() == 0 && empty_out.index_size() == 0);
        Vfs valid_out = build_vfs(fx::kMainManifest);
        const std::string before = observe(valid_out);
        SW_CHECK(build_with_failure(view, valid_out, k, used) == VfsBuildStatus::OutOfMemory);
        SW_CHECK(observe(valid_out) == before);
    }
}

// A publicação final de `build` é `out = std::move(next)`. Ela não pode lançar: o static_assert
// garante a declaração; aqui se mede o que a STL de fato faz (alocações durante essa atribuição) e
// se injeta falha em todas as alocações que ocorressem. Valor observado declarado na saída.
void test_publication_does_not_allocate()
{
    std::cout
        << "[Test] build: a publicação `out = std::move(next)` não aloca (medido) e não lança\n";
    static_assert(std::is_nothrow_move_assignable_v<Vfs>);
    const std::array<const Source*, 3> targets = {nullptr, &sources()[0], &sources()[1]};
    for (const Source* target : targets) {
        for (const Source& src : sources()) {
            if (src.expected != VfsBuildStatus::Ok) continue;
            Vfs out;
            if (target != nullptr) {
                SW_CHECK(Vfs::build(open_manifest(target->bytes), out) == VfsBuildStatus::Ok);
            }
            Vfs next;
            SW_CHECK(Vfs::build(open_manifest(src.bytes), next) == VfsBuildStatus::Ok);
            const std::string expected = observe(next);
            g_allocations.store(0);
            g_fail_countdown.store(0); // se houver QUALQUER alocação na publicação, ela lança
            g_counting.store(true);
            bool threw = false;
            try {
                out = std::move(next);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            g_counting.store(false);
            g_fail_countdown.store(-1);
            const std::uint64_t allocs = g_allocations.load();
            std::cout << "        publicação de \"" << src.name << "\" sobre "
                      << (target == nullptr ? "out vazio" : target->name) << ": " << allocs
                      << " alocação(ões)\n";
            SW_CHECK(!threw);
            SW_CHECK(allocs == 0);
            SW_CHECK(observe(out) == expected);
        }
    }
}

// O que a STL deste build declara noexcept, impresso (no MSVC Debug é o que a pilha do CI mostrou:
// o construtor padrão de string/vector é noexcept e aloca o proxy). É a ASSINATURA do header
// instalado, avaliada pelo compilador, não o texto do header.
void print_stl_facts()
{
    std::cout << "[Info] STL: ";
#ifdef _MSC_FULL_VER
    std::cout << "_MSC_FULL_VER=" << _MSC_FULL_VER << " ";
#endif
#ifdef _ITERATOR_DEBUG_LEVEL
    std::cout << "_ITERATOR_DEBUG_LEVEL=" << _ITERATOR_DEBUG_LEVEL << " ";
#endif
#ifdef __GLIBCXX__
    std::cout << "__GLIBCXX__=" << __GLIBCXX__ << " ";
#endif
#ifdef _LIBCPP_VERSION
    std::cout << "_LIBCPP_VERSION=" << _LIBCPP_VERSION << " ";
#endif
    std::cout << "\n        noexcept: string()="
              << std::is_nothrow_default_constructible_v<std::string> << " vector<u64>()="
              << std::is_nothrow_default_constructible_v<
                     std::vector<std::uint64_t>> << " string(const string&)="
              << std::is_nothrow_copy_constructible_v<std::string> << " vector<u64>(n,v)="
              << std::is_nothrow_constructible_v<std::vector<std::uint64_t>, std::size_t,
                                                 const std::uint64_t&> << " vector<string_view>(n)="
              << std::is_nothrow_constructible_v<std::vector<std::string_view>,
                                                 std::size_t> << " ManifestView(const&)="
              << std::is_nothrow_copy_constructible_v<pack::ManifestView> << " Vfs()="
              << std::is_nothrow_default_constructible_v<Vfs> << " Vfs=Vfs&&="
              << std::is_nothrow_move_assignable_v<Vfs> << "\n";
}

// Texto com tudo o que o ManifestView expõe SEM passar pelo modelo (`to_model`): é o que um move do
// Vfs preserva (membros trivialmente copiáveis; as strings string_view apontam para os bytes do
// manifesto, que continuam vivos).
std::string view_digest(const pack::ManifestView& v)
{
    std::ostringstream o;
    const auto hex = [&o](const auto& h) {
        for (const auto b : h)
            o << std::hex << int(b) << std::dec << ",";
    };
    o << v.game_id() << "|" << v.build_id() << "|" << v.build_seq() << "|" << v.created_at() << "|"
      << v.min_reader() << "|" << v.payload_size() << "|";
    hex(v.manifest_id());
    o << "\nblocks=" << v.block_count() << " refs=" << v.block_ref_count()
      << " pieces=" << v.piece_count() << " files=" << v.file_count()
      << " groups=" << v.group_count() << "\n";
    for (std::size_t i = 0; i < v.block_count(); ++i) {
        o << "b" << i << ":" << v.block_raw_size(i) << "/" << v.block_packed_size(i) << "/"
          << int(v.block_codec(i)) << "/";
        hex(v.block_hash(i));
        o << "\n";
    }
    for (std::size_t i = 0; i < v.block_ref_count(); ++i)
        o << v.block_ref(i) << ",";
    o << "\n";
    for (std::size_t i = 0; i < v.file_count(); ++i) {
        o << v.file_path(i) << ":" << v.file_size(i) << "/" << int(v.file_class(i)) << "/"
          << v.file_group(i) << "/" << v.file_first_ref(i) << "/" << v.file_ref_count(i) << "\n";
    }
    return o.str();
}

// Estado de um Vfs DE QUE SE MOVEU (move gerado pelo compilador, sem move próprio), membro a
// membro:
//   preservado (garantido, membros trivialmente copiáveis): tudo o que `view_digest` lê;
//   tabela de índices: vazia após o construtor de move (garantia do std::vector); após a atribuição
//     de move é "válida, não especificada" (o teste só chama, não confere o valor);
//   NÃO garantido: `manifest().to_model().chunker.algorithm` (std::string movida: válida, valor
//     não especificado; vazia na prática) e, portanto, todo o `to_model()` além das strings.
// Os handles abertos antes do move ficam inválidos e não são tocados aqui.
void test_moved_from_vfs_allowed_operations()
{
    std::cout << "[Test] Vfs movido-de: o que é preservado, o que não é garantido e as operações "
                 "permitidas (sem tocar handles invalidados)\n";
    const Vfs reference = build_vfs(fx::kMainManifest);
    const std::string reference_digest = view_digest(reference.manifest());
    const std::string reference_text = observe(reference);
    SW_CHECK(!reference.manifest().to_model().chunker.algorithm.empty());

    // Construtor de move.
    Vfs a = build_vfs(fx::kMainManifest);
    Vfs b(std::move(a));
    SW_CHECK(a.index_size() == 0);                           // garantido pelo move do std::vector
    SW_CHECK(view_digest(a.manifest()) == reference_digest); // chamada segura E valor preservado
    // Chamada segura, valor NÃO garantido: só se chama; o valor não é conferido.
    const std::string moved_algorithm = a.manifest().to_model().chunker.algorithm;
    std::cout << "        chunker.algorithm do movido-de tem " << moved_algorithm.size()
              << " byte(s) nesta STL (não garantido)\n";
    SW_CHECK(b.index_size() == b.manifest().block_ref_count());
    SW_CHECK(observe(b) == reference_text); // o destino responde como a referência

    // Permitido: atribuir por cima (cópia), e o Vfs volta a ser completo.
    a = reference;
    SW_CHECK(observe(a) == reference_text);

    // Permitido: usar um movido-de como `out` de um build novo.
    Vfs c(std::move(a));
    SW_CHECK(Vfs::build(open_manifest(fx::kPrefixManifest), a) == VfsBuildStatus::Ok);
    SW_CHECK(observe(a) == observe(build_vfs(fx::kPrefixManifest)));
    SW_CHECK(observe(c) == reference_text);

    // Atribuição de move: o destino é completo; o movido-de preserva a visão e só pode ser
    // consultado nas leituras seguras (o valor da tabela não é conferido).
    Vfs d;
    d = std::move(c);
    SW_CHECK(observe(d) == reference_text);
    SW_CHECK(view_digest(c.manifest()) == reference_digest);
    (void)c.index_size();
    (void)c.manifest().to_model();
    // Permitido: destruir (fim do escopo) e atribuir por cima.
    c = reference;
    SW_CHECK(observe(c) == reference_text);
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
            // do offset até o fim do arquivo: o maior size que ainda é Ok
            SW_CHECK(file->resolve(offset, file->size() - offset, spans) == VfsStatus::Ok);
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
                const std::uint64_t s = rng.next() % (n - o + 1); // sempre dentro do arquivo
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

// ---------------------------------------------------------------- F1-64: reforço dos testes

// Os três testes abaixo cobrem o que o F1-17 deixou de fora (achados 2 e 3 da revisão do F1-17):
//  - byte >= 0x80 logo depois do nome de um diretório (mata o mutante unsigned char -> signed char
//    em `before_dir`);
//  - blocos de exatamente 262144 bytes (limite do §8 item 5);
//  - arquivos lógicos de mais de 4 GiB, descritos só pelo manifesto (nenhum dado em disco).
// Os resultados esperados NUNCA vêm do Vfs: ou são literais calculados à mão (com a conta nos
// comentários e `static_assert`s sobre as constantes), ou saem do oráculo linear abaixo, que soma
// os tamanhos dos blocos um a um e recorta a interseção de cada bloco com a janela lida (sem busca
// binária, sem tabela de inícios, sem nada do Vfs).

using Spans = std::vector<BlockSpan>;

// Arquivo descrito por `refs` (índices de bloco) e pela tabela de tamanhos `raw`. `starts[i]` é a
// soma dos tamanhos das referências anteriores a `i`, calculada aqui, uma vez, por soma simples.
struct OracleFile
{
    const std::vector<std::uint32_t>& refs;
    const std::uint32_t* raw;
    std::vector<std::uint64_t> starts;

    OracleFile(const std::vector<std::uint32_t>& r, const std::uint32_t* sizes)
        : refs(r), raw(sizes)
    {
        std::uint64_t sum = 0;
        for (const std::uint32_t b : refs) {
            starts.push_back(sum);
            sum += raw[b];
        }
    }
};

// Trechos esperados de [offset, offset + size): varredura linear das referências, recortando a
// interseção de cada bloco com a janela. Só para janelas DENTRO do arquivo.
Spans oracle_spans(const OracleFile& file, std::uint64_t offset, std::uint64_t size)
{
    Spans out;
    const std::uint64_t end = offset + size;
    // Pula, de 256 em 256 referências e SEM busca binária, as que terminam antes de `offset` (só
    // para o Debug não varrer 16 mil referências a cada sondagem; o recorte abaixo é o mesmo).
    std::size_t first = 0;
    while (first + 256 < file.refs.size() && file.starts[first + 256] <= offset) {
        first += 256;
    }
    for (std::size_t i = first; i < file.refs.size() && file.starts[i] < end; ++i) {
        const std::uint32_t b = file.refs[i];
        const std::uint64_t stop = file.starts[i] + file.raw[b];
        if (stop <= offset) {
            continue;
        }
        const std::uint64_t lo = std::max(offset, file.starts[i]);
        const std::uint64_t hi = std::min(end, stop);
        if (lo < hi) {
            out.push_back(BlockSpan{b, static_cast<std::uint32_t>(lo - file.starts[i]),
                                    static_cast<std::uint32_t>(hi - lo)});
        }
    }
    return out;
}

struct Got
{
    VfsStatus status = VfsStatus::Ok;
    std::uint64_t total = 0;
    Spans spans;
};

Got collect(const VfsFile& file, std::uint64_t offset, std::uint64_t size)
{
    Got g;
    VfsSpans spans;
    g.status = file.resolve(offset, size, spans);
    if (g.status != VfsStatus::Ok) {
        SW_CHECK(spans.total_bytes() == 0); // destino intocado
        BlockSpan none;
        SW_CHECK(!spans.next(none));
        return g;
    }
    g.total = spans.total_bytes();
    BlockSpan s;
    while (spans.next(s)) {
        SW_CHECK(s.length > 0);
        g.spans.push_back(s);
    }
    SW_CHECK(!spans.next(s)); // depois do fim continua sendo o fim
    return g;
}

// Leitura que deve dar Ok com exatamente os trechos `want` (literais calculados à mão).
void expect_read(const VfsFile& file, std::uint64_t offset, std::uint64_t size, const Spans& want)
{
    const Got g = collect(file, offset, size);
    SW_CHECK(g.status == VfsStatus::Ok);
    std::uint64_t sum = 0;
    for (const BlockSpan& s : want) {
        sum += s.length;
    }
    SW_CHECK(g.total == sum);
    SW_CHECK(g.spans == want);
}

void expect_out_of_range(const VfsFile& file, std::uint64_t offset, std::uint64_t size)
{
    SW_CHECK(collect(file, offset, size).status == VfsStatus::OutOfRange);
}

// offset == tamanho: Ok com 0 bytes e 0 trechos, para qualquer size.
void expect_empty_read(const VfsFile& file, std::uint64_t offset, std::uint64_t size)
{
    const Got g = collect(file, offset, size);
    SW_CHECK(g.status == VfsStatus::Ok && g.total == 0 && g.spans.empty());
}

// Compara resolve + next com o oráculo para cada (offset, size) de `offsets` x `sizes` (mais o
// "até o fim" e o "um byte além do fim" de cada offset).
void sweep_against_oracle(const VfsFile& file, const std::vector<std::uint32_t>& refs,
                          const std::uint32_t* raw, const std::vector<std::uint64_t>& offsets,
                          const std::vector<std::uint64_t>& sizes)
{
    const OracleFile oracle(refs, raw);
    const std::uint64_t n = file.size();
    std::uint64_t reads = 0;
    const auto check = [&](std::uint64_t o, std::uint64_t s) {
        const Got g = collect(file, o, s);
        SW_CHECK(g.status == expected_status(n, o, s));
        ++reads;
        if (g.status != VfsStatus::Ok) {
            SW_CHECK(g.spans.empty());
        } else if (o == n) {
            SW_CHECK(g.total == 0 && g.spans.empty());
        } else {
            SW_CHECK(g.total == s);
            SW_CHECK(g.spans == oracle_spans(oracle, o, s));
        }
    };
    for (const std::uint64_t o : offsets) {
        for (const std::uint64_t s : sizes) {
            check(o, s);
        }
        // Exatamente até o fim (Ok) e um byte além (OutOfRange). Em arquivos de 16 mil trechos só
        // a partir de perto do fim (<= 64 blocos; a leitura inteira tem teste próprio): cada
        // leitura longa devolve todos os trechos e, em Debug, dezenas delas pesariam no tempo do
        // teste.
        if (o <= n && n - o <= 64 * 262144) {
            check(o, n - o);
            check(o, n - o + 1);
        }
    }
    SW_CHECK(reads > 0);
}

// Os caminhos de uma fixture, dado o diretório, na ordem de bytes: calculado à parte (std::map
// ordena std::string por bytes sem sinal) a partir da lista de caminhos, todos de `file_size`.
std::vector<Expected> expected_listing_of(const std::vector<std::string>& paths,
                                          std::uint64_t file_size, const std::string& dir)
{
    std::map<std::string, Expected> children;
    const std::string prefix = dir.empty() ? "" : dir + "/";
    for (const std::string& p : paths) {
        if (p.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = p.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            children[rest] = {rest, VfsKind::File, file_size};
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

const Vfs& high_vfs()
{
    static const Vfs vfs = build_vfs(fx::kHighBytesManifest);
    return vfs;
}

const Vfs& max_block_vfs()
{
    static const Vfs vfs = build_vfs(fx::kMaxBlockManifest);
    return vfs;
}

const Vfs& huge_vfs()
{
    static const Vfs vfs = build_vfs(fx::kHugeManifest);
    return vfs;
}

// ---- (a) byte alto depois do prefixo de diretório

void test_high_byte_after_directory_prefix()
{
    std::cout << "[Test] bytes altos: irmãos de diretório com byte >= 0x80 logo após o nome "
                 "(mata o mutante unsigned char -> signed char em before_dir)\n";
    const std::vector<std::string> paths = fx::high_byte_paths();
    const pack::ManifestView view = open_manifest(fx::kHighBytesManifest);
    SW_CHECK(view.file_count() == paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        SW_CHECK(view.file_path(i) == paths[i]);
        SW_CHECK(view.file_size(i) == fx::kHighByteFileSize && view.file_ref_count(i) == 1);
    }
    // Precondições da fixture, conferidas à mão e não supostas: a lista está em ordem de BYTES SEM
    // SINAL (a do manifesto) e, para cada diretório abaixo, há irmão com byte >= 0x80 logo depois
    // do nome (que vem DEPOIS de "dir/..." nessa ordem).
    const auto unsigned_less = [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
                return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
            });
    };
    SW_CHECK(std::is_sorted(paths.begin(), paths.end(), unsigned_less));
    const std::vector<std::string> dirs = {"a", "maps", "zeta", "\xC3\xB1"};
    std::size_t high_siblings = 0;
    for (const std::string& dir : dirs) {
        for (const std::string& p : paths) {
            if (p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 &&
                static_cast<unsigned char>(p[dir.size()]) >= 0x80) {
                ++high_siblings;
            }
        }
    }
    SW_CHECK(high_siblings >= 6); // maps: 3; zeta: 1; ñ: 2; a: 0
    // Nenhum caminho da fixture tem byte isolado inválido: UTF-8 bem formado (regra do dono).
    for (const std::string& p : paths) {
        for (std::size_t i = 0; i < p.size();) {
            const auto c = static_cast<unsigned char>(p[i]);
            const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            SW_CHECK(c < 0x80 || (c >> 5) == 0x6 || (c >> 4) == 0xE || (c >> 3) == 0x1E);
            SW_CHECK(i + len <= p.size());
            for (std::size_t k = 1; k < len; ++k) {
                SW_CHECK((static_cast<unsigned char>(p[i + k]) & 0xC0) == 0x80);
            }
            i += len;
        }
    }

    const Vfs& vfs = high_vfs();
    VfsStat info;
    VfsFile f;

    // Diretórios: existem (por conterem arquivos), apesar do irmão de byte alto depois do nome.
    for (const std::string& dir : dirs) {
        SW_CHECK(vfs.stat(dir, info) == VfsStatus::Ok);
        SW_CHECK(info.kind == VfsKind::Directory && info.size == 0);
        SW_CHECK(vfs.open(dir, f) == VfsStatus::IsDirectory);
        SW_CHECK(!f.valid());
    }
    // O conteúdo de cada diretório, escrito à mão (só os filhos de verdade, nenhum irmão alto).
    const std::vector<std::pair<std::string, std::vector<std::string>>> listing = {
        {"a", {"x.bin"}},
        {"maps", {"a.bin", "b.bin"}},
        {"zeta", {"q.bin"}},
        {"\xC3\xB1", {"x.bin"}}};
    for (const auto& [dir, names] : listing) {
        VfsDir d;
        SW_CHECK(vfs.open_dir(dir, d) == VfsStatus::Ok);
        SW_CHECK(d.entry_count() == names.size());
        VfsDirEntry e;
        for (const std::string& name : names) {
            SW_CHECK(d.next(e) && e.name == name && e.kind == VfsKind::File &&
                     e.size == fx::kHighByteFileSize);
        }
        SW_CHECK(!d.next(e));
    }
    // A raiz, na ordem de bytes: "maps" < "maps-1.bin" < "maps0.bin" < "mapsá.bin" < "mapsあ.bin" <
    // "maps😀.bin" (0x2D < 0x30 < 0xC3 < 0xE3 < 0xF0 depois de "maps"); os de byte alto vêm depois
    // do DIRETÓRIO "maps", e "ñ" (C3 B1) vem depois de todo nome que começa em ASCII.
    const std::vector<std::pair<std::string, VfsKind>> root = {{"a", VfsKind::Directory},
                                                               {"maps", VfsKind::Directory},
                                                               {"maps-1.bin", VfsKind::File},
                                                               {"maps0.bin", VfsKind::File},
                                                               {"maps\xC3\xA1"
                                                                ".bin",
                                                                VfsKind::File},
                                                               {"maps\xE3\x81\x82"
                                                                ".bin",
                                                                VfsKind::File},
                                                               {"maps\xF0\x9F\x98\x80"
                                                                ".bin",
                                                                VfsKind::File},
                                                               {"zeta", VfsKind::Directory},
                                                               {"zeta\xC3\xA9"
                                                                ".bin",
                                                                VfsKind::File},
                                                               {"\xC3\xB1", VfsKind::Directory},
                                                               {"\xC3\xB1\xC3\xA1"
                                                                ".bin",
                                                                VfsKind::File},
                                                               {"\xC3\xB1\xE3\x81\x82"
                                                                ".bin",
                                                                VfsKind::File}};
    {
        VfsDir d;
        SW_CHECK(vfs.open_dir("", d) == VfsStatus::Ok);
        SW_CHECK(d.entry_count() == root.size());
        VfsDirEntry e;
        for (const auto& [name, kind] : root) {
            SW_CHECK(d.next(e) && e.name == name && e.kind == kind);
        }
        SW_CHECK(!d.next(e));
        // E o mesmo, calculado à parte a partir da lista de caminhos (std::map por bytes).
        const std::vector<Expected> want = expected_listing_of(paths, fx::kHighByteFileSize, "");
        SW_CHECK(want.size() == root.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            SW_CHECK(want[i].name == root[i].first && want[i].kind == root[i].second);
        }
        for (const auto& [dir, names] : listing) {
            const std::vector<Expected> sub =
                expected_listing_of(paths, fx::kHighByteFileSize, dir);
            SW_CHECK(sub.size() == names.size());
            for (std::size_t i = 0; i < sub.size(); ++i) {
                SW_CHECK(sub[i].name == names[i]);
            }
        }
    }
    // Todo arquivo existe, com o tamanho certo, e resolve como um bloco de 10 bytes.
    for (const std::string& p : paths) {
        SW_CHECK(vfs.stat(p, info) == VfsStatus::Ok);
        SW_CHECK(info.kind == VfsKind::File && info.size == fx::kHighByteFileSize);
        SW_CHECK(vfs.open(p, f) == VfsStatus::Ok && f.path() == p);
        expect_read(f, 0, 10, {BlockSpan{0, 0, 10}});
        expect_read(f, 3, 4, {BlockSpan{0, 3, 4}});
    }
    // Nomes que NÃO existem: prefixos de nomes de arquivo (sem a extensão) nunca são diretórios.
    for (const char* p : {"maps\xC3\xA1", "maps\xE3\x81\x82", "maps\xF0\x9F\x98\x80",
                          "zeta\xC3\xA9", "\xC3\xB1\xC3\xA1", "\xC3\xB1\xE3\x81\x82", "maps/c.bin",
                          "map", "m", "\xC3\xB1/y.bin", "maps\xC3\xA1/a.bin", "maps/"}) {
        SW_CHECK(vfs.stat(p, info) == VfsStatus::NotFound);
        SW_CHECK(vfs.open(p, f) == VfsStatus::NotFound);
        VfsDir d;
        SW_CHECK(vfs.open_dir(p, d) == VfsStatus::NotFound);
    }
    // Arquivo não é diretório.
    VfsDir d;
    SW_CHECK(vfs.open_dir("maps\xC3\xA1"
                          ".bin",
                          d) == VfsStatus::NotDirectory);
    SW_CHECK(vfs.open_dir("\xC3\xB1\xC3\xA1"
                          ".bin",
                          d) == VfsStatus::NotDirectory);
}

// Os bytes que o gerador usa (block_bytes_sized, uma palavra por cálculo) são os mesmos da função
// por byte block_byte.
void check_sized_block_bytes(std::uint32_t block, const std::vector<std::uint8_t>& data)
{
    SW_CHECK(!data.empty());
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8},
                                std::size_t{9}, data.size() / 2, data.size() - 1}) {
        if (i < data.size()) {
            SW_CHECK(data[i] == fx::block_byte(block, static_cast<std::uint32_t>(i)));
        }
    }
}

// ---- (b) bloco de exatamente 262144 bytes

void test_max_block_262144()
{
    std::cout << "[Test] bloco de 262144 bytes (limite do §8 item 5): fim exato de bloco, início, "
                 "leitura entre blocos e último bloco curto\n";
    static_assert(fx::kMaxBlockRawSize[0] == 262144 && fx::kMaxBlockRawSize[1] == 262144);
    const pack::ManifestView view = open_manifest(fx::kMaxBlockManifest);
    SW_CHECK(view.block_count() == fx::kMaxBlockRawSize.size());
    for (std::uint32_t b = 0; b < view.block_count(); ++b) {
        SW_CHECK(view.block_raw_size(b) == fx::kMaxBlockRawSize[b]);
        const auto data = fx::block_bytes_sized(b, fx::kMaxBlockRawSize[b]);
        SW_CHECK(data.size() == fx::kMaxBlockRawSize[b]);
        check_sized_block_bytes(b, data);
        SW_CHECK(view.block_hash(b) == pack::sha256(std::as_bytes(std::span(data))));
    }
    const auto defs = fx::max_block_files();
    SW_CHECK(view.file_count() == defs.size());
    for (std::size_t i = 0; i < defs.size(); ++i) {
        SW_CHECK(view.file_path(i) == defs[i].path);
        SW_CHECK(view.file_size(i) == fx::file_size_of(defs[i], fx::kMaxBlockRawSize.data()));
        SW_CHECK(view.file_ref_count(i) == defs[i].refs.size());
    }

    const Vfs& vfs = max_block_vfs();
    const VfsFile exact = open_file(vfs, "max/exact.bin"); // [b0]
    const VfsFile pair = open_file(vfs, "max/pair.bin");   // [b0, b1]
    const VfsFile mix = open_file(vfs, "max/mix.bin");     // [b3, b0, b2, b1, b5]
    const VfsFile tail = open_file(vfs, "max/tail.bin");   // [b4, b0, b5]
    // Tamanhos calculados à mão: 262144 | 2 x 262144 = 524288 | 262143 + 262144 + 1 + 262144 + 100
    // = 786532 | 131072 + 262144 + 100 = 393316.
    SW_CHECK(exact.size() == 262144);
    SW_CHECK(pair.size() == 524288);
    SW_CHECK(mix.size() == 786532);
    SW_CHECK(tail.size() == 393316);

    // exact.bin: um único bloco de 262144 bytes.
    expect_read(exact, 0, 262144, {{0, 0, 262144}}); // o bloco inteiro, num trecho só
    expect_read(exact, 0, 1, {{0, 0, 1}});
    expect_read(exact, 262143, 1, {{0, 262143, 1}}); // o último byte do bloco
    expect_read(exact, 1, 262143, {{0, 1, 262143}}); // termina exatamente no fim do bloco
    expect_read(exact, 131072, 131072, {{0, 131072, 131072}});
    expect_empty_read(exact, 262144, 0); // EOF
    expect_empty_read(exact, 262144, 1);
    expect_out_of_range(exact, 262143, 2); // passa do fim do bloco e do arquivo
    expect_out_of_range(exact, 1, 262144);
    expect_out_of_range(exact, 0, 262145);
    expect_out_of_range(exact, 262145, 0);

    // pair.bin: b0 em [0, 262144), b1 em [262144, 524288).
    expect_read(pair, 0, 262144, {{0, 0, 262144}}); // termina no limite: nenhum trecho vazio depois
    expect_read(pair, 262144, 262144, {{1, 0, 262144}}); // começa no limite: nenhum trecho antes
    expect_read(pair, 262143, 2,
                {{0, 262143, 1}, {1, 0, 1}}); // cruza o limite por 1 byte de cada lado
    expect_read(pair, 262144, 1, {{1, 0, 1}});
    expect_read(pair, 262143, 1, {{0, 262143, 1}});
    expect_read(pair, 262142, 4, {{0, 262142, 2}, {1, 0, 2}});
    expect_read(pair, 1, 524287, {{0, 1, 262143}, {1, 0, 262144}});
    expect_read(pair, 0, 524288, {{0, 0, 262144}, {1, 0, 262144}});
    expect_read(pair, 524287, 1, {{1, 262143, 1}});
    expect_empty_read(pair, 524288, 0);
    expect_empty_read(pair, 524288, 7);
    expect_out_of_range(pair, 524287, 2);
    expect_out_of_range(pair, 0, 524289);
    expect_out_of_range(pair, 524289, 0);

    // mix.bin: b3 em [0, 262143), b0 em [262143, 524287), b2 (1 byte) em [524287, 524288),
    // b1 em [524288, 786432), b5 (100 bytes, o bloco curto da cauda) em [786432, 786532).
    expect_read(mix, 262142, 3, {{3, 262142, 1}, {0, 0, 2}});
    expect_read(mix, 262143, 262144, {{0, 0, 262144}}); // exatamente o bloco máximo b0
    expect_read(mix, 262143, 262145, {{0, 0, 262144}, {2, 0, 1}});
    expect_read(mix, 524286, 3,
                {{0, 262143, 1}, {2, 0, 1}, {1, 0, 1}}); // atravessa o bloco de 1 byte
    expect_read(mix, 524287, 1, {{2, 0, 1}});
    expect_read(mix, 524288, 1, {{1, 0, 1}});
    expect_read(mix, 786431, 2, {{1, 262143, 1}, {5, 0, 1}});
    expect_read(mix, 786432, 100, {{5, 0, 100}}); // o último bloco, inteiro
    expect_read(mix, 786531, 1, {{5, 99, 1}});
    expect_read(mix, 0, 786532,
                {{3, 0, 262143}, {0, 0, 262144}, {2, 0, 1}, {1, 0, 262144}, {5, 0, 100}});
    expect_empty_read(mix, 786532, 0);
    expect_out_of_range(mix, 786432, 101);
    expect_out_of_range(mix, 786531, 2);

    // tail.bin: b4 (131072) em [0, 131072), b0 em [131072, 393216), b5 (100) em [393216, 393316).
    expect_read(tail, 131070, 4, {{4, 131070, 2}, {0, 0, 2}});
    expect_read(tail, 131072, 262144,
                {{0, 0, 262144}}); // leitura de 262144 bytes alinhada ao bloco
    expect_read(tail, 131073, 262144, {{0, 1, 262143}, {5, 0, 1}}); // 262144 bytes não alinhados
    expect_read(tail, 393214, 4, {{0, 262142, 2}, {5, 0, 2}});
    expect_read(tail, 393216, 100, {{5, 0, 100}});
    expect_read(tail, 0, 131072, {{4, 0, 131072}});
    expect_out_of_range(tail, 393216, 101);

    // Varredura contra o oráculo: cada limite de bloco e os vizinhos, com tamanhos em torno de 1,
    // 262143, 262144 e 262145.
    for (const fx::FileDef& def : defs) {
        const VfsFile file = open_file(vfs, def.path);
        const std::uint64_t n = file.size();
        std::vector<std::uint64_t> offsets = {0, 1, 2, n, n + 1};
        if (n >= 2) {
            offsets.push_back(n - 1);
            offsets.push_back(n - 2);
        }
        std::uint64_t edge = 0;
        for (const std::uint32_t b : def.refs) {
            edge += fx::kMaxBlockRawSize[b];
            for (const std::int64_t d : {-2, -1, 0, 1, 2}) {
                const std::int64_t v = static_cast<std::int64_t>(edge) + d;
                if (v >= 0 && static_cast<std::uint64_t>(v) <= n) {
                    offsets.push_back(static_cast<std::uint64_t>(v));
                }
            }
        }
        sweep_against_oracle(file, def.refs, fx::kMaxBlockRawSize.data(), offsets,
                             {0, 1, 2, 3, 99, 100, 101, 131071, 131072, 131073, 262142, 262143,
                              262144, 262145, 262146, 524287, 524288, 524289, kMax});
    }
}

// ---- (c) offsets em torno de 2^32 (arquivos lógicos de mais de 4 GiB, só no manifesto)

constexpr std::uint64_t k2p32 = std::uint64_t{1} << 32;
constexpr std::uint64_t kBlk = 262144;
// 16384 blocos de 262144 bytes somam exatamente 2^32; 16383 deles, 2^32 - 262144.
static_assert(16384 * kBlk == k2p32);
static_assert(16383 * kBlk + 262143 == k2p32 - 1);
// huge/boundary.bin = B0 x 16383, B1 (262143), B2 (1), B2, B2, B0: os B2 começam em 2^32 - 1, 2^32
// e 2^32 + 1; o último B0, em 2^32 + 2. Tamanho = 2^32 + 2 + 262144 = 4295229442.
constexpr std::uint64_t kBoundarySize = 4295229442ULL;
static_assert(kBoundarySize == k2p32 + 2 + kBlk);
// huge/mid.bin = B3 (1000), B0 x 16384, B1 (262143). O 16384º B0 começa em 1000 + 16383 * 262144 =
// 4294706152 e o byte 2^32 cai no deslocamento 261144 dele. Tamanho = 1000 + 2^32 + 262143.
constexpr std::uint64_t kMidSize = 4295230439ULL;
static_assert(kMidSize == 1000 + k2p32 + 262143);
constexpr std::uint64_t kMidLastB0Start = 4294706152ULL;
static_assert(kMidLastB0Start == 1000 + 16383 * kBlk);
static_assert(k2p32 - kMidLastB0Start == 261144);

void test_offsets_around_4gib()
{
    std::cout
        << "[Test] offsets acima de 4 GiB: abaixo, em e acima de 2^32, leitura que atravessa "
           "a fronteira e ref_start_ em 2^32 - 1, 2^32 e 2^32 + 1 (arquivos só no manifesto)\n";
    const auto defs = fx::huge_files();
    const std::uint32_t* raw = fx::kHugeBlockRawSize.data();
    const pack::ManifestView view = open_manifest(fx::kHugeManifest);
    SW_CHECK(view.block_count() == fx::kHugeBlockRawSize.size());
    for (std::uint32_t b = 0; b < view.block_count(); ++b) {
        SW_CHECK(view.block_raw_size(b) == raw[b]);
        const auto data = fx::block_bytes_sized(b, raw[b]);
        check_sized_block_bytes(b, data);
        SW_CHECK(view.block_hash(b) == pack::sha256(std::as_bytes(std::span(data))));
    }
    SW_CHECK(view.file_count() == 2 && defs.size() == 2);
    SW_CHECK(view.file_path(0) == "huge/boundary.bin" && view.file_path(1) == "huge/mid.bin");
    SW_CHECK(view.file_size(0) == kBoundarySize && view.file_size(1) == kMidSize);
    SW_CHECK(view.file_ref_count(0) == 16388 && view.file_ref_count(1) == 16386);
    SW_CHECK(view.block_ref_count() == 16388 + 16386);
    for (std::size_t f = 0; f < defs.size(); ++f) {
        SW_CHECK(defs[f].refs.size() == view.file_ref_count(f));
        for (std::size_t k = 0; k < defs[f].refs.size(); ++k) {
            SW_CHECK(view.block_ref(view.file_first_ref(f) + k) == defs[f].refs[k]);
        }
    }

    const Vfs& vfs = huge_vfs();
    VfsStat info;
    SW_CHECK(vfs.stat("huge/boundary.bin", info) == VfsStatus::Ok && info.size == kBoundarySize);
    SW_CHECK(vfs.stat("huge", info) == VfsStatus::Ok && info.kind == VfsKind::Directory);
    const VfsFile boundary = open_file(vfs, "huge/boundary.bin");
    const VfsFile mid = open_file(vfs, "huge/mid.bin");
    SW_CHECK(boundary.size() == kBoundarySize && mid.size() == kMidSize);

    // --- boundary.bin. B0 = bloco 0 (262144), B1 = bloco 1 (262143), B2 = bloco 2 (1 byte).
    //     B1 ocupa [2^32 - 262143 - 1, 2^32 - 1) = [4294705152, 4294967295).
    expect_read(boundary, k2p32 - 2, 1, {{1, 262142, 1}}); // último byte de B1, 2 abaixo de 2^32
    expect_read(boundary, k2p32 - 1, 1, {{2, 0, 1}});      // imediatamente ABAIXO: 1º B2
    expect_read(boundary, k2p32, 1, {{2, 0, 1}});          // EXATAMENTE EM 2^32: 2º B2
    expect_read(boundary, k2p32 + 1, 1, {{2, 0, 1}});      // imediatamente ACIMA: 3º B2
    expect_read(boundary, k2p32 + 2, 1, {{0, 0, 1}});      // primeiro byte do último B0
    // Leituras que ATRAVESSAM a fronteira.
    expect_read(boundary, k2p32 - 2, 4, {{1, 262142, 1}, {2, 0, 1}, {2, 0, 1}, {2, 0, 1}});
    expect_read(boundary, k2p32 - 1, 2, {{2, 0, 1}, {2, 0, 1}});
    expect_read(boundary, k2p32 - 1, 3, {{2, 0, 1}, {2, 0, 1}, {2, 0, 1}});
    expect_read(boundary, k2p32, 2, {{2, 0, 1}, {2, 0, 1}});
    expect_read(boundary, k2p32 + 1, 3, {{2, 0, 1}, {0, 0, 2}});
    // 999 bytes finais de B1 (a partir do deslocamento 261144), os três B2 e mais 5 bytes de B0.
    expect_read(boundary, k2p32 - 1000, 1002, {{1, 261144, 999}, {2, 0, 1}, {2, 0, 1}, {2, 0, 1}});
    expect_read(boundary, k2p32 - 1000, 1007,
                {{1, 261144, 999}, {2, 0, 1}, {2, 0, 1}, {2, 0, 1}, {0, 0, 5}});
    // Cauda: o último B0 inteiro, o último byte, EOF e o que passa dele.
    expect_read(boundary, kBoundarySize - 1, 1, {{0, 262143, 1}});
    expect_read(boundary, kBoundarySize - 262144, 262144, {{0, 0, 262144}});
    expect_read(boundary, k2p32, kBoundarySize - k2p32, {{2, 0, 1}, {2, 0, 1}, {0, 0, 262144}});
    expect_empty_read(boundary, kBoundarySize, 0);
    expect_empty_read(boundary, kBoundarySize, 99);
    expect_empty_read(boundary, kBoundarySize, kMax);
    expect_out_of_range(boundary, kBoundarySize - 1, 2);
    expect_out_of_range(boundary, kBoundarySize + 1, 0);
    expect_out_of_range(boundary, 2 * k2p32, 1);
    expect_out_of_range(boundary, kMax, 0);
    expect_out_of_range(boundary, 0, kBoundarySize + 1); // size > 2^32, ainda tudo-ou-nada
    expect_out_of_range(boundary, 1, kBoundarySize);
    // Começo do arquivo, e leitura LONGA (> 2^32 bytes) de ponta a ponta e do 2º byte ao fim.
    expect_read(boundary, 0, 1, {{0, 0, 1}});
    expect_read(boundary, 262143, 2, {{0, 262143, 1}, {0, 0, 1}});
    {
        const Got whole = collect(boundary, 0, kBoundarySize);
        SW_CHECK(whole.status == VfsStatus::Ok && whole.total == kBoundarySize);
        SW_CHECK(whole.spans.size() == 16388);
        for (std::size_t i = 0; i < 16383; ++i) {
            SW_CHECK(whole.spans[i] == (BlockSpan{0, 0, 262144}));
        }
        SW_CHECK(whole.spans[16383] == (BlockSpan{1, 0, 262143}));
        SW_CHECK(whole.spans[16384] == (BlockSpan{2, 0, 1}));
        SW_CHECK(whole.spans[16385] == (BlockSpan{2, 0, 1}));
        SW_CHECK(whole.spans[16386] == (BlockSpan{2, 0, 1}));
        SW_CHECK(whole.spans[16387] == (BlockSpan{0, 0, 262144}));
        std::uint64_t sum = 0;
        for (const BlockSpan& s : whole.spans) {
            sum += s.length;
        }
        SW_CHECK(sum == kBoundarySize);

        const Got from_second = collect(boundary, 1, kBoundarySize - 1);
        SW_CHECK(from_second.status == VfsStatus::Ok && from_second.total == kBoundarySize - 1);
        SW_CHECK(from_second.spans.size() == 16388);
        SW_CHECK(from_second.spans.front() == (BlockSpan{0, 1, 262143}));
        SW_CHECK(from_second.spans.back() == (BlockSpan{0, 0, 262144}));
    }

    // --- mid.bin. B3 = bloco 3 (1000), B0 = bloco 0 (262144), B1 = bloco 1 (262143).
    //     O 16384º B0 ocupa [4294706152, 4294968296); 2^32 está no deslocamento 261144.
    expect_read(mid, 0, 1001, {{3, 0, 1000}, {0, 0, 1}});
    expect_read(mid, 999, 2, {{3, 999, 1}, {0, 0, 1}});
    expect_read(mid, 1000, 1, {{0, 0, 1}});
    expect_read(mid, k2p32 - 1, 1, {{0, 261143, 1}}); // imediatamente ABAIXO
    expect_read(mid, k2p32, 1, {{0, 261144, 1}});     // EXATAMENTE EM 2^32 (meio do bloco)
    expect_read(mid, k2p32 + 1, 1, {{0, 261145, 1}}); // imediatamente ACIMA
    expect_read(mid, k2p32 - 1, 3, {{0, 261143, 3}}); // atravessa 2^32 DENTRO de um bloco: 1 trecho
    // Do byte 2^32 - 1 até o fim do bloco (1001 bytes) e 1 byte do bloco seguinte (B1).
    expect_read(mid, k2p32 - 1, 1002, {{0, 261143, 1001}, {1, 0, 1}});
    expect_read(mid, k2p32 + 999, 2,
                {{0, 262143, 1}, {1, 0, 1}}); // o limite de bloco em 2^32 + 1000
    // Atravessa 2^32 passando por 3 blocos: resto do penúltimo B0 (a partir do deslocamento 10),
    // o último B0 inteiro e 5 bytes de B1. 262134 + 262144 + 5 = 524283.
    expect_read(mid, kMidLastB0Start - 262144 + 10, 524283,
                {{0, 10, 262134}, {0, 0, 262144}, {1, 0, 5}});
    expect_read(mid, kMidSize - 1, 1, {{1, 262142, 1}});
    expect_read(mid, kMidSize - 262143, 262143, {{1, 0, 262143}});
    expect_empty_read(mid, kMidSize, 0);
    expect_empty_read(mid, kMidSize, 5);
    expect_out_of_range(mid, kMidSize - 1, 2);
    expect_out_of_range(mid, kMidSize + 1, 0);
    expect_out_of_range(mid, 0, kMidSize + 1);
    expect_out_of_range(mid, k2p32, kMidSize - k2p32 + 1);
    expect_out_of_range(mid, 2 * k2p32, 0);
    {
        const Got whole = collect(mid, 0, kMidSize);
        SW_CHECK(whole.status == VfsStatus::Ok && whole.total == kMidSize);
        SW_CHECK(whole.spans.size() == 16386);
        SW_CHECK(whole.spans.front() == (BlockSpan{3, 0, 1000}));
        for (std::size_t i = 1; i <= 16384; ++i) {
            SW_CHECK(whole.spans[i] == (BlockSpan{0, 0, 262144}));
        }
        SW_CHECK(whole.spans.back() == (BlockSpan{1, 0, 262143}));
        std::uint64_t sum = 0;
        for (const BlockSpan& s : whole.spans) {
            sum += s.length;
        }
        SW_CHECK(sum == kMidSize);
    }

    // --- Varredura contra o oráculo linear: cada início de referência perto de 2^32 (e vizinhos),
    //     2^32 +- 2, o começo e o fim do arquivo, com tamanhos em torno de 1 byte, de um bloco e
    //     de vários blocos (inclusive maiores que 2^32 - 1, via "até o fim").
    for (std::size_t f = 0; f < defs.size(); ++f) {
        const VfsFile file = open_file(vfs, defs[f].path);
        const std::uint64_t n = file.size();
        std::vector<std::uint64_t> offsets = {0,         1,         2,     999,       1000,
                                              1001,      n - 2,     n - 1, n,         n + 1,
                                              k2p32 - 2, k2p32 - 1, k2p32, k2p32 + 1, k2p32 + 2};
        std::uint64_t start = 0;
        for (const std::uint32_t b : defs[f].refs) {
            // Inícios de referência a menos de 3 blocos de 2^32 (e os vizinhos de cada um).
            if (start + 3 * kBlk >= k2p32 && start <= k2p32 + 3 * kBlk) {
                for (const std::int64_t d : {-1, 0, 1}) {
                    offsets.push_back(
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(start) + d));
                }
            }
            start += raw[b];
        }
        SW_CHECK(start == n);
        sweep_against_oracle(file, defs[f].refs, raw, offsets,
                             {0, 1, 3, 1001, 262143, 262144, 262145, 1000000, 8 * kBlk + 7, kMax});
    }
}

} // namespace

int main()
{
    // Uma exceção que escape de uma função noexcept (ex.: bad_alloc injetado fora de um try em
    // `Vfs::build`) vira std::terminate -> abort(). No MSVC Debug o abort() abre uma caixa de
    // diálogo que trava o CI até o timeout; aqui termina na hora, com mensagem, como o SW_CHECK.
    std::set_terminate([] {
        std::fputs("std::terminate: exceção escapou de uma função noexcept\n", stderr);
        std::fflush(nullptr);
        std::_Exit(1);
    });
#ifdef _MSC_VER
    // Mesmo cuidado de sw-core/tests/main.cpp: no Debug, avisos do CRT/STL (verificação de
    // iteradores, abort) abrem uma caixa de diálogo e esperam; no CI o teste ficaria pendurado.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    print_stl_facts();
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
    test_file_directory_conflict_definition();
    test_build_failure_keeps_out_untouched();
    test_build_failure_when_source_is_the_destination();
    test_successful_rebuild_effect_on_open_handles();
    test_build_view_copy_stage();
    test_publication_does_not_allocate();
    test_moved_from_vfs_allowed_operations();
    test_resolve_does_not_allocate();
    test_concurrent_resolves();
    test_high_byte_after_directory_prefix();
    test_max_block_262144();
    test_offsets_around_4gib();
    std::cout << "VFS: todos os testes passaram\n";
    return 0;
}
