// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Constantes do envelope e do payload do manifesto v0 (docs/03 §7.1 e §7.2) que o leitor
// (`sw-format`) e o escritor (`sw-pack`) precisam concordar. Uma só definição, aqui.
#pragma once

#include <cstddef>
#include <string_view>

namespace swarmstream::pack {

inline constexpr std::string_view kMagic{"SWMANIF\0", 8};
inline constexpr std::string_view kFileIdentifier{"SWM0", 4};
// "SWARMSTREAM-MANIFEST-V0" + NUL: separação de domínio da assinatura (§7.1).
inline constexpr std::string_view kSignaturePrefix{"SWARMSTREAM-MANIFEST-V0\0", 24};
inline constexpr std::size_t kSignedHeaderSize = 40; // envelope[0..40) entra na mensagem assinada

// Posição do campo `min_reader` no vtable: id 4, uint16 (fixo para sempre, §7.2).
inline constexpr unsigned kMinReaderId = 4;

} // namespace swarmstream::pack
