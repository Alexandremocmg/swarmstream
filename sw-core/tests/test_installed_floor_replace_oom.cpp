// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-74: como o piso (commit_installed_floor, W5) trata o código que `replace_file` devolve.
//
// O helper só devolve `OutOfMemory` ANTES do rename, sem tocar o disco. Logo, no piso:
//  - replace => OutOfMemory: status OutOfMemory, SEM texto, `may_have_changed` FALSO, alvo
//    intacto, .tmp completo; uma nova gravação com as operações reais conclui;
//  - replace => qualquer outro erro (IoError): continua conservador (`may_have_changed`, "antigo
//    OU novo"), como nos testes do piso (test_installed_floor.cpp), que não são editados.
// O ramo que devolve OutOfMemory só existe de verdade no POSIX (no Windows o helper não aloca);
// por isso este teste injeta o código por uma operação de plataforma, e roda em todos os sistemas.
#include "floor_test_support.hpp"

#include <iostream>
#include <string>
#include <swarmstream/manifest_view.hpp>

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace sp = swarmstream::pack;
namespace fs = std::filesystem;
using namespace swarmstream::test::floor;

namespace {

const sp::Ed25519Seed kSeedA = seed_of(0);

std::span<const sp::Ed25519PublicKey> keys_of(const std::vector<sp::Ed25519PublicKey>& v)
{
    return std::span<const sp::Ed25519PublicKey>(v.data(), v.size());
}

// Tudo real, menos o replace: devolve `code` SEM trocar nada (o contrato do helper para
// OutOfMemory; para IoError, o caso "rename falhou").
class ReplaceReturns final : public det::FloorFileOps
{
public:
    explicit ReplaceReturns(sw::CacheErrorCode c) : code(c) {}
    sw::CacheErrorCode code;
    int replace_calls = 0;

    sw::CacheErrorCode create_directory(const fs::path& dir) noexcept override
    {
        return det::real_floor_file_ops().create_directory(dir);
    }
    sw::CacheErrorCode write_file_durable(const fs::path& file,
                                          std::span<const std::byte> data) noexcept override
    {
        return det::real_floor_file_ops().write_file_durable(file, data);
    }
    det::FileReadStatus read_file(const fs::path& file, std::size_t max_bytes,
                                  Bytes& out) noexcept override
    {
        return det::real_floor_file_ops().read_file(file, max_bytes, out);
    }
    sw::CacheErrorCode replace(const fs::path&, const fs::path&) noexcept override
    {
        ++replace_calls;
        return code;
    }
};

void run_case(bool with_old_floor)
{
    std::cout << "[Test] replace => OutOfMemory no W5: "
              << (with_old_floor ? "piso antigo existe" : "primeira instalacao") << std::endl;
    const fs::path dir = fresh_dir(with_old_floor ? "f174old" : "f174first");
    const std::vector<sp::Ed25519PublicKey> keys{public_key_of(kSeedA)};
    const Bytes old_m = make_manifest("jogo", 1, kSeedA);
    const Bytes new_m = make_manifest("jogo", 2, kSeedA);
    const fs::path target = det::installed_floor_path(dir, "jogo");
    const fs::path tmp = det::installed_tmp_path(dir, "jogo");
    if (with_old_floor) SW_CHECK(det::commit_installed_floor(dir, old_m, keys_of(keys)).wrote);

    ReplaceReturns oom_ops(sw::CacheErrorCode::OutOfMemory);
    const det::FloorWrite w = det::commit_installed_floor(dir, new_m, keys_of(keys), oom_ops);
    SW_CHECK(oom_ops.replace_calls == 1);
    SW_CHECK(w.status == det::FloorStatus::OutOfMemory);
    SW_CHECK(w.category == det::FloorCategory::None);
    SW_CHECK(!w.wrote);
    SW_CHECK(!w.may_have_changed); // nada foi trocado: o helper garante
    SW_CHECK(w.text().empty());    // sem texto (montá-lo alocaria)
    // Alvo intacto (antigo ou ausente) e .tmp completo com o manifesto novo.
    if (with_old_floor)
        SW_CHECK(same_bytes(read_all(target), old_m));
    else
        SW_CHECK(!fs::exists(target));
    SW_CHECK(same_bytes(read_all(tmp), new_m));
    const det::FloorRead after = det::read_installed_floor(dir, "jogo", keys_of(keys));
    if (with_old_floor) {
        SW_CHECK(after.status == det::FloorStatus::Ok);
        SW_CHECK(after.floor.has_value() && *after.floor == 1);
    } else {
        SW_CHECK(after.status == det::FloorStatus::Ok);
        SW_CHECK(!after.floor.has_value());
    }

    // Repetir com as operações reais conclui.
    const det::FloorWrite again = det::commit_installed_floor(dir, new_m, keys_of(keys));
    SW_CHECK(again.status == det::FloorStatus::Ok && again.wrote);
    SW_CHECK(same_bytes(read_all(target), new_m));

    // Controle: IoError (o `rename` falhou, ou o sync da pasta falhou DEPOIS dele) continua
    // conservador. Se o piso tratasse tudo como OutOfMemory, ou OutOfMemory como IoError, este
    // contraste (e os asserts acima) falharia.
    const fs::path dir2 = fresh_dir(with_old_floor ? "f174old_io" : "f174first_io");
    if (with_old_floor) SW_CHECK(det::commit_installed_floor(dir2, old_m, keys_of(keys)).wrote);
    ReplaceReturns io_ops(sw::CacheErrorCode::IoError);
    const det::FloorWrite wio = det::commit_installed_floor(dir2, new_m, keys_of(keys), io_ops);
    SW_CHECK(wio.status == det::FloorStatus::Io && wio.category == det::FloorCategory::Replace);
    SW_CHECK(!wio.wrote && wio.may_have_changed);
    SW_CHECK(wio.text().find("antigo ou novo") != std::string_view::npos);

    cleanup(dir);
    cleanup(dir2);
}

} // namespace

int main()
{
    SW_CHECK(sp::crypto_ready());
    run_case(/*with_old_floor=*/true);
    run_case(/*with_old_floor=*/false);
    std::cout << "TESTE DO W5 COM OutOfMemory DO replace (F1-74) PASSOU" << std::endl;
    return 0;
}
