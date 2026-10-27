// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Inicialização do libsodium do `sw-pack`: o ÚNICO ponto que chama `sodium_init()`.
// Toda entrada pública que usa criptografia (Ed25519, SHA-256, chunker, leitor e construtor do
// manifesto) confere `crypto_ready()` antes e devolve erro se ela for falsa; nada depende de um
// `sw_init` futuro. `sodium_init()` é idempotente e seguro entre threads.
#pragma once

#include <string_view>

namespace swarmstream::pack {

// true se o libsodium iniciou (`sodium_init() >= 0`). A primeira chamada o inicia.
[[nodiscard]] bool crypto_ready() noexcept;

// Versão do libsodium em uso (`sodium_version_string()`), para relatórios de medição.
[[nodiscard]] std::string_view crypto_version() noexcept;

namespace testing {

// SÓ PARA TESTES: com `true`, `crypto_ready()` passa a devolver false, como se o `sodium_init()`
// tivesse falhado. Não há outro jeito de provocar essa falha. Não construa `Sha256` enquanto
// estiver ligado (o construtor aborta se o libsodium não estiver pronto).
void force_crypto_failure(bool enabled) noexcept;

} // namespace testing

} // namespace swarmstream::pack
