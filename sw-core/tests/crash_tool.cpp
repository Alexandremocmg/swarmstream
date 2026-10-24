// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-11: ferramenta do teste de queda (crash_test.py). Critério de aceite: queda sem peça
// corrompida.
//
//   sw-core-crash-tool write <dir> <seed> <ponto> <vez> <ops>
//     Abre o cache em <dir> e faz <ops> Put/Remove aleatórios num universo de 24 peças (teto de
//     16 slots, checkpoint a cada 4 KiB de journal). Antes de cada operação imprime a intenção
//     ("p 5", "r 5") e, depois, a resposta confirmada ("P 5" gravada, "R 5" removida, "N 5"
//     não estava, "O 5" sem slot). Com <ponto> != 0, morre com _Exit(77) na <vez>-ésima
//     passagem por esse ponto (detail::CrashPoint: escrita, barreiras, journal pela metade,
//     etapas do checkpoint). Com <ponto> == 0, o driver mata o processo num instante aleatório.
//
//   sw-core-crash-tool verify <dir> <log>
//     Reabre o cache (recuperação) e confere as 24 peças contra o log de todas as rodadas:
//     - toda peça entregue tem exatamente os bytes certos, e nenhuma dá CorruptedChecksum
//       (uma queda não pode deixar indexado um slot sem o dado certo);
//     - peça com "P" confirmado e nenhuma intenção depois está lá; com "R"/"N"/"O" confirmado
//       (ou nunca tocada) e nenhuma intenção depois, não está;
//     - com uma intenção sem resposta (a operação em que o processo morreu), os dois valem.
//     Depois grava, fecha, reabre e lê de novo, para provar que o cache recuperado continua
//     funcionando.

#include "cache_owner_testing.hpp"
#include "swarmstream/cache_owner.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace swarmstream;
namespace fs = std::filesystem;

constexpr uint32_t kUniverse = 24;
constexpr StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};

CacheOwnerConfig config(const fs::path& dir)
{
    return CacheOwnerConfig{
        .dir = dir, .total_slots = 16, .layout = kLayout, .checkpoint_journal_bytes = 4096};
}

PieceId piece(uint32_t i)
{
    PieceHash h{};
    for (size_t b = 0; b < h.bytes.size(); ++b)
        h.bytes[b] = static_cast<uint8_t>(i * 29 + b + 3);
    return PieceId(i, h);
}

struct Content
{
    std::vector<std::byte> bytes;
    std::array<uint32_t, 3> blocks{};
};

Content content(uint32_t i)
{
    Content c;
    const uint32_t size = 2000 + (i * 7919) % 200000;
    c.blocks = {size / 4, size / 2, size - size / 4 - size / 2};
    c.bytes.resize(size);
    for (size_t k = 0; k < size; ++k)
        c.bytes[k] = static_cast<std::byte>((i * 151 + k * 11) & 0xFF);
    return c;
}

int g_point = 0;
int g_count = 0;

void crash_hook(detail::CrashPoint p, void*)
{
    if (static_cast<int>(p) == g_point && --g_count == 0) {
        std::fflush(stdout);
        std::_Exit(77);
    }
}

void say(char what, uint32_t id)
{
    std::printf("%c %u\n", what, id);
    std::fflush(stdout); // o driver lê o log mesmo se o processo morrer logo depois
}

int write_mode(const fs::path& dir, uint32_t seed, int point, int count, int ops)
{
    g_point = point;
    g_count = count;
    auto started = CacheOwnerTestHook::start_with_crash_hook(
        config(dir), point != 0 ? crash_hook : nullptr, nullptr);
    if (!started) {
        std::printf("START_FAIL %s\n", std::string(to_string(started.error())).c_str());
        return 2;
    }
    CacheOwner& owner = **started;
    std::mt19937 rng(seed);
    std::vector<std::byte> out(kLayout.slot_size);
    for (int op = 0; op < ops; ++op) {
        const uint32_t id = rng() % kUniverse;
        if (rng() % 3 == 0) {
            say('r', id);
            auto req = CacheRequest::remove(piece(id));
            auto r = owner.call(req);
            if (r.has_value()) {
                say('R', id);
            } else if (r.error() == CacheErrorCode::PieceNotFound) {
                say('N', id);
            } else {
                std::printf("E %u %s\n", id, std::string(to_string(r.error())).c_str());
                return 3;
            }
        } else {
            const Content c = content(id);
            say('p', id);
            auto req = CacheRequest::put(piece(id), c.bytes, c.blocks);
            auto r = owner.call(req);
            if (r.has_value()) {
                say('P', id);
            } else if (r.error() == CacheErrorCode::OutOfSlots) {
                say('O', id);
            } else {
                std::printf("E %u %s\n", id, std::string(to_string(r.error())).c_str());
                return 3;
            }
        }
    }
    if (auto s = owner.stop(); !s) {
        std::printf("STOP_FAIL %s\n", std::string(to_string(s.error())).c_str());
        return 4;
    }
    return 0;
}

enum class Expect : uint8_t { Absent, Present, Either };

int verify_mode(const fs::path& dir, const fs::path& log)
{
    // Estado esperado de cada peça a partir do log de todas as rodadas.
    std::array<bool, kUniverse> acked_present{};
    std::array<bool, kUniverse> pending{};
    std::ifstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        char what = 0;
        unsigned id = 0;
        if (std::sscanf(line.c_str(), "%c %u", &what, &id) != 2 || id >= kUniverse) continue;
        switch (what) {
        case 'p':
        case 'r': pending[id] = true; break;
        case 'P':
            acked_present[id] = true;
            pending[id] = false;
            break;
        case 'R':
        case 'N':
        case 'O':
            acked_present[id] = false;
            pending[id] = false;
            break;
        default: break;
        }
    }
    auto started = CacheOwner::start(config(dir));
    if (!started) {
        std::printf("FALHA: start() depois da queda devolveu %s\n",
                    std::string(to_string(started.error())).c_str());
        return 1;
    }
    CacheOwner& owner = **started;
    const CacheRecovery& rec = owner.recovery();
    int failures = 0;
    uint32_t present = 0;
    std::vector<std::byte> out(kLayout.slot_size);
    for (uint32_t id = 0; id < kUniverse; ++id) {
        const Content c = content(id);
        auto req = CacheRequest::get(piece(id), out, c.blocks);
        auto r = owner.call(req);
        bool is_present = false;
        if (r.has_value()) {
            is_present = true;
            if (*r != c.bytes.size() ||
                std::memcmp(out.data(), c.bytes.data(), c.bytes.size()) != 0) {
                std::printf("FALHA: peça %u entregue com bytes errados (CORROMPIDA)\n", id);
                ++failures;
            }
        } else if (r.error() == CacheErrorCode::CorruptedChecksum) {
            std::printf("FALHA: peça %u indexada sem o dado certo (CorruptedChecksum)\n", id);
            ++failures;
        } else if (r.error() != CacheErrorCode::PieceNotFound) {
            std::printf("FALHA: peça %u: %s\n", id, std::string(to_string(r.error())).c_str());
            ++failures;
        }
        present += is_present ? 1 : 0;
        const Expect want = pending[id]         ? Expect::Either
                            : acked_present[id] ? Expect::Present
                                                : Expect::Absent;
        if (want == Expect::Present && !is_present) {
            std::printf("FALHA: peça %u confirmada (P) e perdida\n", id);
            ++failures;
        }
        if (want == Expect::Absent && is_present) {
            std::printf("FALHA: peça %u removida/nunca gravada e presente\n", id);
            ++failures;
        }
    }
    // O cache recuperado continua funcionando: grava, fecha, reabre, lê.
    const uint32_t probe = kUniverse - 1;
    {
        auto rm = CacheRequest::remove(piece(probe));
        (void)owner.call(rm);
        const Content c = content(probe);
        auto p = CacheRequest::put(piece(probe), c.bytes, c.blocks);
        auto r = owner.call(p);
        if (!r && r.error() != CacheErrorCode::OutOfSlots) {
            std::printf("FALHA: Put depois da recuperação: %s\n",
                        std::string(to_string(r.error())).c_str());
            ++failures;
        }
    }
    if (auto s = owner.stop(); !s) {
        std::printf("FALHA: stop() depois da recuperação: %s\n",
                    std::string(to_string(s.error())).c_str());
        ++failures;
    }
    auto again = CacheOwner::start(config(dir));
    if (!again) {
        std::printf("FALHA: segunda abertura: %s\n", std::string(to_string(again.error())).c_str());
        ++failures;
    }
    std::printf("%s peças=%u índice=%d lsn=%llu aplicados=%llu truncados=%llu sem_arquivo=%u\n",
                failures == 0 ? "OK" : "ERRO", present, rec.index_loaded ? 1 : 0,
                static_cast<unsigned long long>(rec.index_lsn),
                static_cast<unsigned long long>(rec.journal_records_applied),
                static_cast<unsigned long long>(rec.journal_truncated_bytes),
                rec.dropped_missing_file);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 7 && std::strcmp(argv[1], "write") == 0) {
        return write_mode(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)),
                          std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]));
    }
    if (argc == 4 && std::strcmp(argv[1], "verify") == 0) {
        return verify_mode(argv[2], argv[3]);
    }
    std::fprintf(stderr, "uso: %s write <dir> <seed> <ponto> <vez> <ops> | verify <dir> <log>\n",
                 argv[0]);
    return 64;
}
