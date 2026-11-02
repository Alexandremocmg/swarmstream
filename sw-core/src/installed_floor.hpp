// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Piso anti-rollback por `game_id` (F1-69; docs/03 §8 item 10, F1-60 §6.2). Interno do sw-core:
// ainda não está ligado ao sw_init (isso é do F1-67), e nada aqui é exportado.
//
// O piso do `build_seq` de um jogo é o `build_seq` do manifesto assinado que ficou instalado
// na pasta de cache: uma CÓPIA dos bytes exatos do envelope (docs/03 §7) em
//   <pasta>/installed/<SHA-256(game_id) em hex minúsculo>.manifest
// Nenhum formato novo. O nome é só um índice: o `game_id` que vale é o de dentro do manifesto
// verificado. Cada jogo tem o seu arquivo; o sw-core só toca no arquivo do `game_id` que está
// abrindo e nunca apaga, move nem reescreve o de outro jogo.
//
// LEITURA (read_installed_floor, sob o lock da pasta, antes de aceitar o manifesto novo):
//   - arquivo ausente (ENOENT) => nenhum piso (primeira instalação deste `game_id`);
//   - presente => lido inteiro e verificado com as `trusted_keys` ATUAIS (assinatura e §8, sem
//     piso); o piso é o `build_seq` desse manifesto;
//   - POLÍTICA CONSERVADORA: ilegível, vazio, malformado, assinatura inválida, chave que saiu da
//     lista, `game_id` divergente ou `min_reader` não suportado => status Io com a categoria no
//     diagnóstico. NUNCA é tratado como primeira instalação, nunca é apagado nem reescrito.
//
// GRAVAÇÃO (commit_installed_floor, último passo do sw_init, passos W1-W5 do F1-60 §6.2):
//   grava se o arquivo estiver ausente, se o `build_seq` novo for MAIOR, ou IGUAL com bytes
//   diferentes (reassinatura / rotação de chave: o piso não muda). NUNCA grava valor menor.
//   W1 cria installed/; W2+W3 gravam `<hash>.manifest.tmp` (trunca, escreve e faz fsync no
//   mesmo descritor, cada retorno conferido); W4 relê o .tmp e confere byte a byte; W5
//   replace_file(.tmp, alvo).
//
// PROPRIEDADE (a promessa "falha não avança o valor" foi retirada): o piso SÓ SOBE, e só para o
// `build_seq` de um manifesto que passou na verificação completa; nunca desce e nunca fica
// parcial. Falha antes de W5 (W1-W4) não o altera. Erro de replace_file, ou queda depois de W4,
// pode deixá-lo ANTIGO OU NOVO: o helper não distingue "falhou antes da troca" de "falhou
// depois" (no POSIX faz rename(2) e depois fsync da pasta), então o resultado diz
// `may_have_changed` e a abertura seguinte lê o que existir e verifica.
//
// Limites (docs/04): a assinatura autentica o conteúdo, não impede quem tem escrita na pasta de
// APAGAR a cópia (indistinguível de primeira instalação) nem de SUBSTITUÍ-LA por outra cópia
// antiga também assinada. O sw-core não decide downgrade. Proteção real exige guardar o piso
// fora da pasta (launcher, Fase 4).
#pragma once

#include "swarmstream/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <swarmstream/ed25519_verify.hpp>
#include <vector>

namespace swarmstream::detail {

inline constexpr const char* kInstalledDirName = "installed";
inline constexpr const char* kInstalledFileSuffix = ".manifest";
inline constexpr const char* kInstalledTmpSuffix = ".manifest.tmp";

enum class FloorStatus : std::uint8_t {
    Ok = 0,
    Io,          // => SW_E_IO com o diagnóstico (nunca primeira instalação, nunca piso reduzido)
    Rollback,    // commit: o manifesto a gravar tem build_seq MENOR que o instalado (recusado)
    OutOfMemory, // => SW_E_OUT_OF_MEMORY
};

// Categoria do diagnóstico: o que deu errado e em que etapa.
enum class FloorCategory : std::uint8_t {
    None = 0,
    // estado existente (leitura e verificação)
    Read,           // ilegível
    Format,         // vazio, truncado, malformado, grande demais
    Signature,      // assinatura inválida
    UnknownKey,     // key_id fora das trusted_keys atuais
    GameIdMismatch, // o manifesto de dentro é de outro game_id que o do nome
    Unsupported,    // min_reader / format_version que este leitor não suporta
    // manifesto novo recebido para gravar (não verificável: erro de uso do chamador)
    NewManifest,
    // passos da gravação
    CreateDir, // W1
    WriteTmp,  // W2 + W3
    VerifyTmp, // W4
    Replace,   // W5
    LowerSeq,  // build_seq menor que o instalado
};

[[nodiscard]] const char* to_string(FloorCategory category) noexcept;

enum class FileReadStatus : std::uint8_t {
    Ok = 0,
    NotFound, // ENOENT / ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND (e só estes)
    TooLarge, // passou de `max_bytes`
    Io,       // qualquer outro erro
    OutOfMemory,
};

// Operações de plataforma do piso (F1-60 §6.2 (a)): o padrão usa os helpers reais; o teste
// injeta comportamento, inclusive DEPOIS da troca (uma `replace` que faz o rename de verdade e
// só então devolve erro).
class FloorFileOps
{
public:
    virtual ~FloorFileOps() = default;
    // W1: cria `dir` se faltar (o pai tem de existir). Existir como pasta é sucesso.
    virtual CacheErrorCode create_directory(const std::filesystem::path& dir) noexcept = 0;
    // W2+W3: cria ou TRUNCA `file`, escreve tudo, faz fsync no mesmo descritor e fecha, cada
    // retorno conferido.
    virtual CacheErrorCode write_file_durable(const std::filesystem::path& file,
                                              std::span<const std::byte> data) noexcept = 0;
    // Lê o arquivo inteiro (até `max_bytes`) SEM criá-lo se faltar.
    virtual FileReadStatus read_file(const std::filesystem::path& file, std::size_t max_bytes,
                                     std::vector<std::byte>& out) noexcept = 0;
    // W5: troca `to` por `from` (replace_file: no POSIX rename + fsync da pasta).
    virtual CacheErrorCode replace(const std::filesystem::path& from,
                                   const std::filesystem::path& to) noexcept = 0;
};

// Implementação real (platform_file.hpp). Vive durante o processo.
[[nodiscard]] FloorFileOps& real_floor_file_ops() noexcept;

// installed/<hex>.manifest e o .tmp irmão. Podem lançar bad_alloc (os de produção chamam
// dentro de try).
[[nodiscard]] std::string installed_hash_hex(std::string_view game_id);
[[nodiscard]] std::filesystem::path installed_floor_path(const std::filesystem::path& cache_dir,
                                                         std::string_view game_id);
[[nodiscard]] std::filesystem::path installed_tmp_path(const std::filesystem::path& cache_dir,
                                                       std::string_view game_id);

struct FloorRead
{
    FloorStatus status = FloorStatus::Ok;
    FloorCategory category = FloorCategory::None;
    // Com status Ok: o piso (build_seq do manifesto instalado), ou nullopt se o arquivo não existe.
    std::optional<std::uint64_t> floor;
    // Só com floor == nullopt: a pasta já tem index.bin/journal.bin ("piso ausente em cache
    // existente": o chamador registra um WARN). Em dúvida (erro ao examinar) vale true.
    bool existing_cache_without_floor = false;
    // Diagnóstico para o log_fn: categoria, caminho, key_id, causa. Vazio com Ok.
    std::string detail;
};

[[nodiscard]] FloorRead read_installed_floor(const std::filesystem::path& cache_dir,
                                             std::string_view game_id,
                                             std::span<const pack::Ed25519PublicKey> trusted_keys,
                                             FloorFileOps& ops = real_floor_file_ops()) noexcept;

struct FloorWrite
{
    FloorStatus status = FloorStatus::Ok;
    FloorCategory category = FloorCategory::None;
    bool wrote = false; // true: o alvo foi trocado (replace devolveu sucesso)
    // true quando um erro de replace_file deixou o alvo ANTIGO OU NOVO. Falsa em W1-W4.
    bool may_have_changed = false;
    std::string detail;
};

// `manifest_bytes` = os bytes exatos que o sw_init aceitou. São verificados de novo aqui (com
// `trusted_keys`, sem piso): só se grava um manifesto que passou na verificação completa.
[[nodiscard]] FloorWrite
commit_installed_floor(const std::filesystem::path& cache_dir,
                       std::span<const std::byte> manifest_bytes,
                       std::span<const pack::Ed25519PublicKey> trusted_keys,
                       FloorFileOps& ops = real_floor_file_ops()) noexcept;

} // namespace swarmstream::detail
