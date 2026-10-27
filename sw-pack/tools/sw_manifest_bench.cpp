// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Medição do manifesto v0 (F1-04, ADR-0012): tamanho do arquivo, custo de abrir e memória
// residente. Dois processos, para que o pico de memória de quem gerou não contamine quem abre:
//
//   sw-manifest-bench generate --blocks N [--blocks-per-file K] --out manifest.bin
//   sw-manifest-bench open manifest.bin [--runs R]
//
// Usa a chave de TESTE (a de sw-pack/testdata): é uma ferramenta de desenvolvimento, não entra
// em nenhum build de release do sw-core, do launcher nem do jogo (ADR-0012, item 4).
// O manifesto é sintético (tests/manifest_fixture.hpp), com a forma do mapa da F0-09.
//
// "Memória residente" aqui é o conjunto de trabalho do processo (Windows) ou o RSS (Linux)
// depois de ler o arquivo inteiro e abrir o manifesto. A devolução das páginas ao sistema que o
// ADR-0012 prevê (mapeamento + madvise) é do sw-core e não está medida aqui.

#include "manifest_fixture.hpp"
#include "swarmstream/manifest.hpp"
#include "swarmstream/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h depois de windows.h
#include <psapi.h>
#endif

namespace sp = swarmstream::pack;
namespace st = swarmstream::test;

namespace {

struct Memory
{
    double current_mib = 0;
    double peak_mib = 0;
};

Memory process_memory()
{
    Memory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        m.current_mib = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        m.peak_mib = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        double kib = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %lf kB", &kib) == 1) {
            m.current_mib = kib / 1024.0;
        } else if (std::sscanf(line.c_str(), "VmHWM: %lf kB", &kib) == 1) {
            m.peak_mib = kib / 1024.0;
        }
    }
#endif
    return m;
}

double seconds_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

sp::Ed25519Seed test_seed()
{
    sp::Ed25519Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::uint8_t>(i);
    }
    return seed;
}

int usage()
{
    std::fprintf(stderr,
                 "uso: sw-manifest-bench generate --blocks N [--blocks-per-file K] --out ARQUIVO\n"
                 "     sw-manifest-bench open ARQUIVO [--runs R]\n");
    return 2;
}

int generate(int argc, char** argv)
{
    std::size_t blocks = 24576;
    std::uint32_t per_file = 12;
    std::string out;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--blocks") == 0) {
            blocks = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        } else if (std::strcmp(argv[i], "--blocks-per-file") == 0) {
            per_file = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        } else if (std::strcmp(argv[i], "--out") == 0) {
            out = argv[i + 1];
        } else {
            return usage();
        }
    }
    if (out.empty() || blocks == 0 || per_file == 0) {
        return usage();
    }

    auto start = std::chrono::steady_clock::now();
    const sp::ManifestModel model = st::make_synthetic_model(blocks, per_file, 20260101);
    const double t_model = seconds_since(start);
    std::uint64_t raw_bytes = 0;
    std::uint64_t packed_bytes = 0;
    for (std::size_t i = 0; i < model.block_hash.size(); ++i) {
        raw_bytes += model.block_raw_size[i];
        packed_bytes += model.block_packed_size[i];
    }

    start = std::chrono::steady_clock::now();
    const sp::BuildResult built = sp::build_manifest(model, test_seed());
    const double t_build = seconds_since(start);
    if (!built.status.ok()) {
        std::fprintf(stderr, "build_manifest falhou: %s (%s)\n", sp::to_string(built.status.code),
                     built.status.detail);
        return 1;
    }
    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char*>(built.bytes.data()),
               static_cast<std::streamsize>(built.bytes.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "nao consegui gravar %s\n", out.c_str());
        return 1;
    }

    const double size_mib = static_cast<double>(built.bytes.size()) / (1024.0 * 1024.0);
    std::printf("blocos            %zu\n", model.block_hash.size());
    std::printf("arquivos          %zu\n", model.file_path.size());
    std::printf("pecas             %zu\n", model.piece_hash.size());
    std::printf("conteudo cru      %.2f GiB (comprimido %.2f GiB)\n",
                static_cast<double>(raw_bytes) / (1024.0 * 1024.0 * 1024.0),
                static_cast<double>(packed_bytes) / (1024.0 * 1024.0 * 1024.0));
    std::printf("manifesto         %zu bytes = %.3f MiB (%.1f bytes por bloco)\n",
                built.bytes.size(), size_mib,
                static_cast<double>(built.bytes.size()) /
                    static_cast<double>(model.block_hash.size()));
    std::printf("tempo: gerar modelo %.2f s, codificar+assinar+reler %.2f s\n", t_model, t_build);
    return 0;
}

int open_file(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    int runs = 3;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--runs") == 0) {
            runs = std::max(1, std::atoi(argv[i + 1]));
        } else {
            return usage();
        }
    }

    const Memory baseline = process_memory();
    std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "nao consegui abrir %s\n", argv[2]);
        return 1;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        std::fprintf(stderr, "leitura incompleta de %s\n", argv[2]);
        return 1;
    }
    const Memory after_read = process_memory();

    const std::array<sp::Ed25519PublicKey, 1> trusted = {sp::ed25519_public_key(test_seed())};
    sp::ReadOptions options;
    options.trusted_keys = trusted;

    // Custo de cada parte da abertura (a assinatura e o hash dominam e não dependem do formato).
    std::vector<double> t_sig, t_hash, t_open;
    sp::ManifestView view;
    for (int run = 0; run < runs; ++run) {
        auto start = std::chrono::steady_clock::now();
        sp::Ed25519Signature signature{};
        std::memcpy(signature.data(), bytes.data() + 40, signature.size());
        static constexpr char kPrefix[] = "SWARMSTREAM-MANIFEST-V0";
        const std::span<const std::byte> prefix(reinterpret_cast<const std::byte*>(kPrefix), 24);
        const bool verified =
            sp::ed25519_verify(trusted[0], signature,
                               {prefix, std::span<const std::byte>(bytes).first(40),
                                std::span<const std::byte>(bytes).subspan(sp::kEnvelopeSize)});
        t_sig.push_back(seconds_since(start));
        if (!verified) {
            std::fprintf(stderr, "assinatura nao confere\n");
            return 1;
        }

        start = std::chrono::steady_clock::now();
        const sp::Hash32 id = sp::sha256(bytes);
        t_hash.push_back(seconds_since(start));

        start = std::chrono::steady_clock::now();
        sp::ManifestView opened;
        const sp::ManifestStatus status = sp::ManifestView::open(bytes, options, opened);
        t_open.push_back(seconds_since(start));
        if (!status.ok() || opened.manifest_id() != id) {
            std::fprintf(stderr, "open falhou: %s (%s)\n", sp::to_string(status.code),
                         status.detail);
            return 1;
        }
        view = std::move(opened);
    }
    const Memory after_open = process_memory();

    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double size_mib = static_cast<double>(bytes.size()) / (1024.0 * 1024.0);
    std::printf("arquivo           %.3f MiB, %zu blocos, %zu arquivos, %zu pecas\n", size_mib,
                view.block_count(), view.file_count(), view.piece_count());
    std::printf(
        "memoria (MiB)     base %.1f -> depois de ler %.1f -> depois de abrir %.1f (pico %.1f)\n",
        baseline.current_mib, after_read.current_mib, after_open.current_mib, after_open.peak_mib);
    std::printf("residente do manifesto (aberto - base): %.1f MiB\n",
                after_open.current_mib - baseline.current_mib);
    std::printf("abertura completa (mediana de %d): %.1f ms\n", runs, median(t_open) * 1000.0);
    std::printf("  so a assinatura Ed25519 (SHA-512 sobre tudo): %.1f ms\n",
                median(t_sig) * 1000.0);
    std::printf("  so o SHA-256 do manifest_id: %.1f ms\n", median(t_hash) * 1000.0);
    std::printf("  resto (verificador + validacoes do 03 secao 8): %.1f ms\n",
                (median(t_open) - median(t_sig) - median(t_hash)) * 1000.0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        return usage();
    }
    if (std::strcmp(argv[1], "generate") == 0) {
        return generate(argc, argv);
    }
    if (std::strcmp(argv[1], "open") == 0) {
        return open_file(argc, argv);
    }
    return usage();
}
