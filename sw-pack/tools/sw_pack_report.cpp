// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// sw-pack-report: relatório de compressão por tipo de arquivo (F1-03).
//   sw-pack-report <pasta> [--codec auto|none|zstd|lz4] [--zstd-level N]
//
// Percorre a pasta, corta cada arquivo em blocos (swarmstream-fastcdc-v0), comprime cada bloco
// com o codec do tipo (ou o forçado por --codec), descomprime de volta, confere que o resultado
// é idêntico e imprime, por extensão: arquivos, bytes brutos e empacotados, taxa e tempos.
// Sai com código 1 se algum bloco não voltar idêntico ou se houver erro de leitura.

#include "swarmstream/chunker.hpp"
#include "swarmstream/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace sp = swarmstream::pack;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats
{
    std::size_t files = 0;
    std::size_t blocks = 0;
    std::size_t blocks_none = 0;
    std::size_t blocks_zstd = 0;
    std::size_t blocks_lz4 = 0;
    std::uint64_t raw = 0;
    std::uint64_t packed = 0;
    double compress_s = 0;
    double decompress_s = 0;
};

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

std::string extension_of(const fs::path& p)
{
    std::string ext = p.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext.empty() ? std::string("(sem extensão)") : ext;
}

bool read_file(const fs::path& path, std::vector<std::byte>& data)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    data.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
    return static_cast<bool>(in);
}

int usage()
{
    std::fprintf(stderr,
                 "uso: sw-pack-report <pasta> [--codec auto|none|zstd|lz4] [--zstd-level N]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return usage();
    const fs::path root = argv[1];
    bool automatic = true;
    sp::Codec forced = sp::Codec::None;
    sp::CodecOptions options;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            const std::string value = argv[++i];
            automatic = value == "auto";
            if (value == "none")
                forced = sp::Codec::None;
            else if (value == "zstd")
                forced = sp::Codec::Zstd;
            else if (value == "lz4")
                forced = sp::Codec::Lz4;
            else if (!automatic)
                return usage();
        } else if (std::strcmp(argv[i], "--zstd-level") == 0 && i + 1 < argc) {
            options.zstd_level = std::atoi(argv[++i]);
        } else {
            return usage();
        }
    }

    const auto chunker = sp::Chunker::make();
    if (!chunker) {
        std::fprintf(stderr, "erro: parâmetros do chunker inválidos\n");
        return 1;
    }

    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec))
            files.push_back(it->path());
        else if (type_ec)
            ec = type_ec;
    }
    if (ec) {
        std::fprintf(stderr, "erro: não consegui percorrer %s: %s\n", root.string().c_str(),
                     ec.message().c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());

    std::map<std::string, Stats> by_ext;
    std::map<std::string, sp::Codec> ext_codec;
    int errors = 0;
    std::vector<std::byte> data, packed, back;

    for (const auto& file : files) {
        if (!read_file(file, data)) {
            std::fprintf(stderr, "erro: falha ao ler %s\n", file.string().c_str());
            ++errors;
            continue;
        }
        const std::string ext = extension_of(file);
        const sp::Codec wanted = automatic ? sp::codec_for_path(file.string()) : forced;
        ext_codec[ext] = wanted;
        Stats& st = by_ext[ext];
        ++st.files;

        std::size_t offset = 0;
        for (const std::size_t len : chunker->split(data)) {
            const std::span<const std::byte> block(data.data() + offset, len);
            offset += len;

            const auto t0 = Clock::now();
            const auto result = sp::compress_block(block, wanted, packed, options);
            const auto t1 = Clock::now();
            const bool ok = result.ok && sp::decompress_block(result.used, packed, len, back);
            const auto t2 = Clock::now();
            if (!ok || back.size() != len || std::memcmp(back.data(), block.data(), len) != 0) {
                std::fprintf(stderr, "erro: bloco de %s (offset %zu) não voltou idêntico\n",
                             file.string().c_str(), offset - len);
                ++errors;
                continue;
            }
            ++st.blocks;
            st.raw += len;
            st.packed += packed.size();
            st.compress_s += seconds(t1 - t0);
            st.decompress_s += seconds(t2 - t1);
            switch (result.used) {
            case sp::Codec::None: ++st.blocks_none; break;
            case sp::Codec::Zstd: ++st.blocks_zstd; break;
            case sp::Codec::Lz4: ++st.blocks_lz4; break;
            }
        }
    }

    std::printf("pasta: %s | arquivos: %zu | modo: %s | nível zstd: %d\n", root.string().c_str(),
                files.size(), automatic ? "auto" : std::string(sp::codec_name(forced)).c_str(),
                options.zstd_level);
    std::printf("%-16s %-5s %7s %7s %14s %14s %7s %10s %10s  %s\n", "tipo", "codec", "arqs",
                "blocos", "bruto(B)", "empac.(B)", "taxa", "comp MB/s", "desc MB/s",
                "blocos none/zstd/lz4");
    Stats total;
    for (const auto& [ext, st] : by_ext) {
        const double ratio =
            st.raw ? static_cast<double>(st.packed) / static_cast<double>(st.raw) : 1.0;
        const double mb = static_cast<double>(st.raw) / (1024.0 * 1024.0);
        std::printf("%-16s %-5s %7zu %7zu %14llu %14llu %6.1f%% %10.1f %10.1f  %zu/%zu/%zu\n",
                    ext.c_str(), std::string(sp::codec_name(ext_codec[ext])).c_str(), st.files,
                    st.blocks, static_cast<unsigned long long>(st.raw),
                    static_cast<unsigned long long>(st.packed), ratio * 100.0,
                    st.compress_s > 0 ? mb / st.compress_s : 0.0,
                    st.decompress_s > 0 ? mb / st.decompress_s : 0.0, st.blocks_none,
                    st.blocks_zstd, st.blocks_lz4);
        total.files += st.files;
        total.blocks += st.blocks;
        total.raw += st.raw;
        total.packed += st.packed;
        total.compress_s += st.compress_s;
        total.decompress_s += st.decompress_s;
    }
    const double mb = static_cast<double>(total.raw) / (1024.0 * 1024.0);
    std::printf("%-16s %-5s %7zu %7zu %14llu %14llu %6.1f%% %10.1f %10.1f\n", "TOTAL", "",
                total.files, total.blocks, static_cast<unsigned long long>(total.raw),
                static_cast<unsigned long long>(total.packed),
                total.raw
                    ? 100.0 * static_cast<double>(total.packed) / static_cast<double>(total.raw)
                    : 100.0,
                total.compress_s > 0 ? mb / total.compress_s : 0.0,
                total.decompress_s > 0 ? mb / total.decompress_s : 0.0);
    if (errors != 0) {
        std::fprintf(stderr, "FALHOU: %d erro(s)\n", errors);
        return 1;
    }
    return 0;
}
