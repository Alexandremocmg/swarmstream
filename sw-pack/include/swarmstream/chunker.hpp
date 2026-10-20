// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Chunker "swarmstream-fastcdc-v0" (docs/03 §11.1, definição aprovada; só muda por ADR).
//
// Corta um arquivo em blocos de tamanho definido pelo conteúdo: inserir ou mudar poucos bytes
// altera só os blocos vizinhos, então versões diferentes do jogo compartilham quase todos os
// blocos. Dois builds só compartilham blocos se os cortes forem idênticos, por isso o
// algoritmo, a tabela gear e as máscaras são fixos e conferidos pelos vetores em
// sw-pack/testdata/vectors-v0.json. NÃO é compatível bit a bit com bibliotecas públicas de FastCDC.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace swarmstream::pack {

// Tabela gear: gear[i] = LE64(SHA-256("swarmstream-gear-v0" ‖ byte(i))[0..8]).
using GearTable = std::array<std::uint64_t, 256>;

struct ChunkerParams
{
    std::uint32_t min_size = 32U * 1024U;  // até este tamanho o trecho restante vira um bloco só
    std::uint32_t avg_size = 128U * 1024U; // ponto de normalização (potência de 2), não a média
    std::uint32_t max_size = 256U * 1024U; // limite rígido de um bloco

    friend bool operator==(const ChunkerParams&, const ChunkerParams&) = default;
};

class Chunker
{
public:
    // Identificador gravado em `Chunker.algorithm` no manifesto.
    static constexpr std::string_view kAlgorithm = "swarmstream-fastcdc-v0";

    // Devolve nullopt se os parâmetros forem inválidos: é preciso 1 <= min <= avg <= max e avg
    // potência de 2 maior ou igual a 8 (as máscaras usam log2(avg) ± 2 bits).
    [[nodiscard]] static std::optional<Chunker> make(ChunkerParams params = {}) noexcept;

    [[nodiscard]] const ChunkerParams& params() const noexcept { return params_; }

    // Tamanho do primeiro bloco de `data` (0 se `data` for vazio). Sem alocação.
    // `data` precisa conter pelo menos `max_size` bytes, ou ser o fim do arquivo: com menos que
    // isso no meio do arquivo o corte pode sair diferente do que sairia com o arquivo inteiro.
    [[nodiscard]] std::size_t cut(std::span<const std::byte> data) const noexcept;

    // Tamanhos dos blocos de um buffer inteiro, em ordem. A soma é `data.size()`.
    [[nodiscard]] std::vector<std::size_t> split(std::span<const std::byte> data) const;

    // A tabela gear (calculada uma vez, na primeira chamada).
    [[nodiscard]] static const GearTable& gear_table() noexcept;

private:
    Chunker(ChunkerParams params, std::uint64_t mask_small, std::uint64_t mask_large) noexcept
        : params_(params), mask_small_(mask_small), mask_large_(mask_large)
    {
    }

    ChunkerParams params_;
    std::uint64_t mask_small_; // log2(avg)+2 bits mais significativos; vale antes do ponto avg
    std::uint64_t mask_large_; // log2(avg)-2 bits mais significativos; vale a partir de avg
};

} // namespace swarmstream::pack
