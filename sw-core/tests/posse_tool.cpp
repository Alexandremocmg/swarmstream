// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Executável auxiliar dos testes da posse da pasta de cache (F1-69). Roda como PROCESSO FILHO
// do teste, para provar o lock e a queda entre processos de verdade.
//
//   posse-tool hold <pasta> <pronto>       pega o lock, grava <pronto> ("ACQUIRED"/"LOCKED"/"IO") e
//                                          segura até ser morto (sai com 3 depois de 60 s)
//   posse-tool try <pasta>                 tenta o lock uma vez: 0 pegou, 10 bloqueado, 11 E/S, 12 memória
//   posse-tool sleep <pronto>              grava <pronto> e dorme (para o teste do exec sem herança)
//   posse-tool floor-commit <pasta> <manifesto> <chave_hex> <modo>
//                                          commit_installed_floor com uma queda injetada:
//                                          ok | die-before-write | die-after-write |
//                                          die-before-replace | die-after-replace
//                                          (a queda é um kill à força do próprio processo)
#include "cache_dir_lock.hpp"
#include "installed_floor.hpp"
#include "test_process.hpp"

#include <swarmstream/manifest_view.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace sw = swarmstream;
namespace fs = std::filesystem;

namespace {

constexpr int kExitAcquired = 0;
constexpr int kExitLocked = 10;
constexpr int kExitIo = 11;
constexpr int kExitOom = 12;
constexpr int kExitUsage = 2;
constexpr int kExitTimeout = 3;

int exit_of(sw::detail::LockCode code)
{
    switch (code) {
    case sw::detail::LockCode::Ok: return kExitAcquired;
    case sw::detail::LockCode::Locked: return kExitLocked;
    case sw::detail::LockCode::Io: return kExitIo;
    case sw::detail::LockCode::OutOfMemory: return kExitOom;
    }
    return kExitIo;
}

// O arquivo "pronto" fica FORA da pasta de cache: a pasta não pode mudar.
// Escreve num arquivo ao lado e troca por rename: o pai que espera o arquivo aparecer nunca o vê
// vazio ou pela metade.
bool write_marker(const fs::path& path, const char* text)
{
    fs::path part = path;
    part += ".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(part, path, ec);
    return !ec;
}

void sleep_until_killed()
{
    for (int i = 0; i < 600; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int cmd_hold(const fs::path& dir, const fs::path& ready)
{
    sw::detail::CacheDirLock lock;
    const sw::detail::LockStatus st = sw::detail::CacheDirLock::acquire(dir, lock);
    if (!st.ok()) {
        std::fprintf(stderr, "hold: %s\n", st.detail.c_str());
        if (!write_marker(ready, st.code == sw::detail::LockCode::Locked ? "LOCKED" : "IO")) return kExitUsage;
        return exit_of(st.code);
    }
    if (!write_marker(ready, "ACQUIRED")) return kExitUsage;
    sleep_until_killed();
    return kExitTimeout;
}

int cmd_try(const fs::path& dir)
{
    sw::detail::CacheDirLock lock;
    const sw::detail::LockStatus st = sw::detail::CacheDirLock::acquire(dir, lock);
    if (!st.ok()) {
        std::fprintf(stderr, "try: %s\n", st.detail.c_str());
        return exit_of(st.code);
    }
    const sw::detail::LockStatus rel = lock.release();
    if (!rel.ok()) {
        std::fprintf(stderr, "try: release: %s\n", rel.detail.c_str());
        return kExitIo;
    }
    return kExitAcquired;
}

int cmd_sleep(const fs::path& ready)
{
    if (!write_marker(ready, "SLEEPING")) return kExitUsage;
    sleep_until_killed();
    return kExitTimeout;
}

// Operações reais com a queda injetada num ponto exato (kill à força do próprio processo).
class DyingOps final : public sw::detail::FloorFileOps
{
public:
    explicit DyingOps(std::string mode) : mode_(std::move(mode)) {}

    sw::CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        return real().create_directory(dir);
    }
    sw::CacheErrorCode write_file_durable(const fs::path& file,
                                          std::span<const std::byte> data) noexcept override
    {
        if (mode_ == "die-before-write") sw::test::hard_kill_self();
        const sw::CacheErrorCode rc = real().write_file_durable(file, data);
        if (mode_ == "die-after-write") sw::test::hard_kill_self();
        return rc;
    }
    sw::detail::FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                                         std::vector<std::byte>& out) noexcept override
    {
        return real().read_file(file, max_bytes, out);
    }
    sw::CacheErrorCode replace(const fs::path& from, const fs::path& to) noexcept override
    {
        if (mode_ == "die-before-replace") sw::test::hard_kill_self();
        const sw::CacheErrorCode rc = real().replace(from, to); // a troca ocorre DE VERDADE
        if (mode_ == "die-after-replace") sw::test::hard_kill_self();
        return rc;
    }

private:
    static sw::detail::FloorFileOps& real() { return sw::detail::real_floor_file_ops(); }
    std::string mode_;
};

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int cmd_floor_commit(const fs::path& dir, const fs::path& manifest_file, const std::string& key_hex,
                     const std::string& mode)
{
    sw::pack::Ed25519PublicKey key{};
    if (key_hex.size() != 64) return kExitUsage;
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = hex_digit(key_hex[2 * i]);
        const int lo = hex_digit(key_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return kExitUsage;
        key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    std::ifstream in(manifest_file, std::ios::binary);
    if (!in) return kExitUsage;
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return kExitUsage;
    std::vector<std::byte> bytes(raw.size());
    if (!raw.empty()) std::memcpy(bytes.data(), raw.data(), raw.size());

    DyingOps ops(mode);
    const sw::pack::Ed25519PublicKey keys[] = {key};
    const sw::detail::FloorWrite w = sw::detail::commit_installed_floor(dir, bytes, keys, ops);
    if (w.status != sw::detail::FloorStatus::Ok) {
        std::fprintf(stderr, "floor-commit: %s\n", w.detail.c_str());
        return kExitIo;
    }
    return w.wrote ? 0 : 1; // 0 gravou; 1 nada a gravar
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return kExitUsage;
    const std::string cmd = argv[1];
    if (cmd == "hold" && argc == 4) return cmd_hold(argv[2], argv[3]);
    if (cmd == "try" && argc == 3) return cmd_try(argv[2]);
    if (cmd == "sleep" && argc == 3) return cmd_sleep(argv[2]);
    if (cmd == "floor-commit" && argc == 6) return cmd_floor_commit(argv[2], argv[3], argv[4], argv[5]);
    std::fprintf(stderr, "uso invalido\n");
    return kExitUsage;
}
