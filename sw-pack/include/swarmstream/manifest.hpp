// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Escrita do manifesto v0 (docs/03 §7, ADR-0012). A LEITURA é do `sw-format`
// (`swarmstream/manifest_view.hpp`, incluído aqui para quem escreve e relê):
//
//   build_manifest()   modelo em memória -> bytes assinados (e se confere lendo de volta, com o
//                      MESMO leitor `ManifestView::open` do cliente: não há segunda implementação)
#pragma once

#include "swarmstream/ed25519.hpp"
#include "swarmstream/manifest_view.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swarmstream::pack {

// ---------------------------------------------------------------- escrita

// Só o envelope e o payload, sem conferir nada do modelo. Para testes que precisam de um
// manifesto assinado e inválido; o código de produção usa build_manifest.
[[nodiscard]] std::vector<std::byte> encode_manifest(const ManifestModel& model,
                                                     const Ed25519Seed& signing_seed);

// Envelope assinado em volta de um payload qualquer (sem olhar o conteúdo). Existe para os
// testes que precisam de payload malformado com assinatura VÁLIDA, o que só um atacante com a
// chave faria; o código de produção usa build_manifest.
[[nodiscard]] std::vector<std::byte> sign_payload(std::span<const std::uint8_t> payload,
                                                  const Ed25519Seed& signing_seed);

struct BuildResult
{
    ManifestStatus status;
    std::vector<std::byte> bytes; // vazio se status não for Ok
};

// Codifica, assina e LÊ DE VOLTA o resultado (com a chave pública derivada da semente), só
// devolvendo bytes que o leitor aceita e que reconstroem exatamente o modelo.
[[nodiscard]] BuildResult build_manifest(const ManifestModel& model,
                                         const Ed25519Seed& signing_seed);

} // namespace swarmstream::pack
