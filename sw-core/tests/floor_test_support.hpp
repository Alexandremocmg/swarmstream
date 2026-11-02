// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Apoio dos testes do piso anti-rollback (F1-69): manifestos assinados de verdade (sw-pack) com
// `game_id`, `build_seq`, chave e `build_id` à escolha, e operações de plataforma com FALHA
// INJETADA (antes e DEPOIS da troca). Só para testes.
#pragma once

#include "check.hpp"
#include "installed_floor.hpp"

#include <swarmstream/chunker.hpp>
#include <swarmstream/crypto.hpp>
#include <swarmstream/ed25519.hpp>
#include <swarmstream/manifest.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace swarmstream::test::floor {

namespace det = swarmstream::detail;
namespace sp = swarmstream::pack;
namespace fs = std::filesystem;

using Bytes = std::vector<std::byte>;

// true se o libsodium é o STUB de revisão local (verifica assinatura "sempre aceita"): os casos
// que dependem de rejeitar uma assinatura ruim só valem com o libsodium real.
inline bool is_sodium_stub()
{
    return sp::crypto_version().find("STUB") != std::string_view::npos;
}

inline sp::Ed25519Seed seed_of(std::uint8_t first)
{
    sp::Ed25519Seed s{};
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<std::uint8_t>(first + i);
    return s;
}

inline sp::Ed25519PublicKey public_key_of(const sp::Ed25519Seed& seed)
{
    const auto k = sp::ed25519_public_key(seed);
    SW_CHECK(k.has_value());
    return *k;
}

inline std::string hex_of(const sp::Ed25519PublicKey& key)
{
    return sp::to_hex(key);
}

inline sp::ManifestModel model_of(const std::string& game_id, std::uint64_t build_seq,
                                  const std::string& build_id = "b")
{
    sp::ManifestModel m;
    m.game_id = game_id;
    m.build_id = build_id;
    m.build_seq = build_seq;
    m.created_at = 1'790'000'000;
    m.chunker = {32U * 1024U, 128U * 1024U, 256U * 1024U, std::string(sp::Chunker::kAlgorithm)};
    sp::GroupInfo core;
    core.name = "core";
    m.groups = {core};
    return m;
}

// Manifesto assinado e válido (sem arquivos): o suficiente para o piso.
inline Bytes make_manifest(const std::string& game_id, std::uint64_t build_seq,
                           const sp::Ed25519Seed& seed, const std::string& build_id = "b")
{
    const sp::BuildResult r = sp::build_manifest(model_of(game_id, build_seq, build_id), seed);
    SW_CHECK(r.status.ok());
    return r.bytes;
}

inline fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_69p_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    return dir;
}

inline void cleanup(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code pec;
        fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, pec);
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    ec.clear();
    fs::remove_all(dir, ec);
    SW_CHECK(!ec);
}

inline Bytes read_all(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    SW_CHECK(static_cast<bool>(f));
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    SW_CHECK(!f.bad());
    Bytes out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

inline void write_all(const fs::path& p, const Bytes& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    SW_CHECK(static_cast<bool>(f));
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    SW_CHECK(static_cast<bool>(f));
}

inline bool same_bytes(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Pastas e arquivos sob `dir` (caminho relativo -> tamanho), para provar que algo NÃO mudou.
inline std::vector<std::pair<std::string, std::uintmax_t>> listing(const fs::path& dir)
{
    std::vector<std::pair<std::string, std::uintmax_t>> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        out.emplace_back(fs::relative(e.path(), dir).generic_string(),
                         e.is_directory() ? 0 : e.file_size());
    }
    SW_CHECK(!ec);
    std::sort(out.begin(), out.end());
    return out;
}

// Operações de plataforma com injeção. Sem nada ligado, repassa às reais. Conta chamadas.
class FaultOps final : public det::FloorFileOps
{
public:
    enum class WriteFault { None, FailBeforeWriting, FailAfterPartialWrite };
    enum class ReplaceFault {
        None,
        FailBeforeSwap, // devolve erro SEM trocar
        FailAfterSwap,  // faz a troca de VERDADE (replace_file) e só então devolve erro
    };

    // injeção
    bool fail_create_dir = false;
    WriteFault write_fault = WriteFault::None;
    bool fail_readback_io = false;      // a releitura do .tmp falha
    bool fail_readback_oom = false;     // idem, falta de memória
    bool corrupt_readback = false;      // a releitura do .tmp volta com um byte trocado
    bool truncate_readback = false;     // a releitura do .tmp volta curta
    det::FileReadStatus read_status_for_target = det::FileReadStatus::Ok; // != Ok: força o status
    ReplaceFault replace_fault = ReplaceFault::None;

    // observação
    int create_dir_calls = 0;
    int write_calls = 0;
    int read_calls = 0;
    int replace_calls = 0;
    bool swapped = false; // o replace REAL rodou e devolveu sucesso

    det::FloorFileOps& real() { return det::real_floor_file_ops(); }

    CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        ++create_dir_calls;
        if (fail_create_dir) return CacheErrorCode::IoError;
        return real().create_directory(dir);
    }

    CacheErrorCode write_file_durable(const fs::path& file,
                                      std::span<const std::byte> data) noexcept override
    {
        ++write_calls;
        switch (write_fault) {
        case WriteFault::None: return real().write_file_durable(file, data);
        case WriteFault::FailBeforeWriting: return CacheErrorCode::IoError;
        case WriteFault::FailAfterPartialWrite: {
            const CacheErrorCode rc = real().write_file_durable(file, data.first(data.size() / 2));
            SW_CHECK(rc == CacheErrorCode::Success);
            return CacheErrorCode::DiskFull; // como um ENOSPC no meio da escrita
        }
        }
        return CacheErrorCode::IoError;
    }

    det::FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                                  Bytes& out) noexcept override
    {
        ++read_calls;
        const bool is_tmp = file.extension() == ".tmp";
        if (is_tmp && fail_readback_io) return det::FileReadStatus::Io;
        if (is_tmp && fail_readback_oom) return det::FileReadStatus::OutOfMemory;
        if (!is_tmp && read_status_for_target != det::FileReadStatus::Ok)
            return read_status_for_target;
        const det::FileReadStatus st = real().read_file(file, max_bytes, out);
        if (is_tmp && st == det::FileReadStatus::Ok && !out.empty()) {
            if (corrupt_readback) out[out.size() / 2] ^= std::byte{0x01};
            if (truncate_readback) out.pop_back();
        }
        return st;
    }

    CacheErrorCode replace(const fs::path& from, const fs::path& to) noexcept override
    {
        ++replace_calls;
        switch (replace_fault) {
        case ReplaceFault::None: {
            const CacheErrorCode rc = real().replace(from, to);
            if (rc == CacheErrorCode::Success) swapped = true;
            return rc;
        }
        case ReplaceFault::FailBeforeSwap: return CacheErrorCode::IoError;
        case ReplaceFault::FailAfterSwap: {
            const CacheErrorCode rc = real().replace(from, to); // troca de verdade
            if (rc == CacheErrorCode::Success) swapped = true;
            return CacheErrorCode::IoError; // ...e SÓ ENTÃO o erro
        }
        }
        return CacheErrorCode::IoError;
    }
};

} // namespace swarmstream::test::floor
