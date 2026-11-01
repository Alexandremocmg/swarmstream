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
// F1-63: o mesmo `operator new` ganha injeção de falha (`g_fail_countdown`): enquanto a contagem
// está ligada, a k-ésima alocação (k = 0, 1, ...) lança std::bad_alloc. Os testes de falha de
// `Vfs::build` varrem todos os k de cada manifesto (tabela de índices, pilha de prefixos) com `out`
// vazio e com `out` já válido, e provam que `out` fica exatamente como estava.
#include "check.hpp"
#include "swarmstream/vfs.hpp"
#include "vfs_fixture.hpp"
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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include <cstring>

using namespace swarmstream;
namespace fx = swarmstream::test::vfs_fixture;

// ---------------------------------------------------------------- contagem de alocações
namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};
// < 0: sem injeção. >= 0: a alocação contada em que este valor chega a 0 lança bad_alloc.
std::atomic<std::int64_t> g_fail_countdown{-1};
void* volatile g_sink = nullptr;
std::atomic<bool> g_trace{false}; // DIAGNOSTICO: imprime a pilha de cada alocacao contada
} // namespace

#ifdef _MSC_VER
// DIAGNOSTICO DESCARTAVEL (F1-63): pilha simbolizada (dbghelp + PDB do Debug) no stderr.
static void diag_print_stack(const char* why, long long k, std::size_t n)
{
    static bool inited = false;
    HANDLE proc = GetCurrentProcess();
    if (!inited) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        const BOOL ok = SymInitialize(proc, nullptr, TRUE);
        std::fprintf(stderr, "[diag] SymInitialize=%d\n", int(ok));
        inited = true;
    }
    void* frames[48];
    const USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
    std::fprintf(stderr, "[diag] %s idx/k=%lld size=%zu frames=%u\n", why, k, n, unsigned(count));
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    for (USHORT i = 0; i < count; ++i) {
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 500;
        DWORD64 disp = 0;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        if (SymFromAddr(proc, addr, &disp, sym)) {
            IMAGEHLP_LINE64 line;
            std::memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(line);
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &ldisp, &line)) {
                std::fprintf(stderr, "[diag]  #%u %s +0x%llx (%s:%lu)\n", unsigned(i), sym->Name,
                             static_cast<unsigned long long>(disp), line.FileName,
                             static_cast<unsigned long>(line.LineNumber));
            } else {
                std::fprintf(stderr, "[diag]  #%u %s +0x%llx\n", unsigned(i), sym->Name,
                             static_cast<unsigned long long>(disp));
            }
        } else {
            std::fprintf(stderr, "[diag]  #%u %p\n", unsigned(i), frames[i]);
        }
    }
    std::fflush(stderr);
}
#endif

void* operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed)) {
        const std::uint64_t idx = g_allocations.fetch_add(1, std::memory_order_relaxed);
#ifdef _MSC_VER
        if (g_trace.load(std::memory_order_relaxed)) {
            g_counting.store(false); // o diagnostico nao e contado nem injetado
            diag_print_stack("ALLOC", static_cast<long long>(idx), n);
            g_counting.store(true);
        }
#else
        (void)idx;
#endif
        if (g_fail_countdown.fetch_sub(1, std::memory_order_relaxed) == 0) {
#ifdef _MSC_VER
            g_counting.store(false);
            diag_print_stack("INJETADO (lanca bad_alloc aqui)", static_cast<long long>(idx), n);
#endif
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
#ifdef _MSC_VER
        g_trace.store(&src == &main_src);
#endif
        SW_CHECK(build_with_failure(view, probe, -1, total) == src.expected);
#ifdef _MSC_VER
        g_trace.store(false);
#endif
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
#ifdef _MSC_VER
                std::fprintf(stderr, "[diag] sweep src=%s k=%lld\n", src.name,
                             static_cast<long long>(k));
                std::fflush(stderr);
#endif
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

// A etapa "cópia da visão" só tem ponto de falha se a std::string `chunker.algorithm`
// ("swarmstream-fastcdc-v0", 22 bytes) NÃO couber no SSO da STL (libstdc++ e MSVC: 15 bytes, aloca;
// libc++: 22 bytes, não aloca). Aqui a injeção é DIRETA na cópia, com controle positivo, e depois o
// mesmo k é injetado no build. Onde a cópia não aloca, o teste diz isso e não finge cobrir.
void test_build_view_copy_stage()
{
    std::cout << "[Test] build: falha de alocação na cópia da visão (injeção direta, com controle "
                 "positivo)\n";
    const pack::ManifestView view = open_manifest(fx::kMainManifest);
    pack::ManifestView target;
    g_allocations.store(0);
    g_fail_countdown.store(-1);
    g_counting.store(true);
    target = view;
    g_counting.store(false);
    const std::uint64_t copy_allocs = g_allocations.load();
    std::cout << "        a cópia do ManifestView faz " << copy_allocs
              << " alocação(ões) nesta STL\n";
    if (copy_allocs == 0) {
        std::cout
            << "        nesta STL a cópia da visão não aloca (algorithm cabe no SSO): a etapa "
               "não tem ponto de falha; nada a injetar\n";
        return;
    }
    for (std::int64_t k = 0; k < static_cast<std::int64_t>(copy_allocs); ++k) {
        // Controle positivo: a injeção dispara DENTRO da cópia da visão.
        pack::ManifestView victim;
        bool threw = false;
        g_allocations.store(0);
        g_fail_countdown.store(k);
        g_counting.store(true);
        try {
            victim = view;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        g_counting.store(false);
        g_fail_countdown.store(-1);
        SW_CHECK(threw);

        // O mesmo k no build: out vazio e out já válido ficam como estavam.
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

} // namespace

int main()
{
    // Uma exceção que escape de uma função noexcept (ex.: bad_alloc injetado fora de um try em
    // `Vfs::build`) vira std::terminate -> abort(). No MSVC Debug o abort() abre uma caixa de
    // diálogo que trava o CI até o timeout; aqui termina na hora, com mensagem, como o SW_CHECK.
    std::set_terminate([] {
        std::fputs("std::terminate: exceção escapou de uma função noexcept\n", stderr);
#ifdef _MSC_VER
        diag_print_stack("TERMINATE", -1, 0);
#endif
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
    test_moved_from_vfs_allowed_operations();
    test_resolve_does_not_allocate();
    test_concurrent_resolves();
    std::cout << "VFS: todos os testes passaram\n";
    return 0;
}
