// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// A mensagem de uma assinatura Ed25519 como lista de partes (prefixo de domínio, cabeçalho,
// payload). O libsodium assina/verifica uma mensagem contígua: com mais de uma parte monta-se uma
// cópia temporária. Usado pela verificação (`sw-format`) e pela assinatura (`sw-pack`): uma só
// definição, para que as duas montem a mesma mensagem.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <vector>

namespace swarmstream::pack {

using MessageParts = std::initializer_list<std::span<const std::byte>>;

namespace detail {

// A mensagem contígua que o libsodium exige. Com uma só parte não copia. Pode lançar
// std::bad_alloc (os chamadores capturam e tratam como falha).
//
// F1-73: `storage` nasce por `vector(n)`, que NÃO é `noexcept`, e nunca pelo construtor padrão. No
// MSVC em Debug todo construtor de std::vector aloca o proxy dos iteradores; no construtor padrão
// (`noexcept`) a falta de memória viraria std::terminate antes do `catch` do chamador. Assim ela é
// um std::bad_alloc comum. Nas outras STLs `vector(0)` não aloca: uma só parte segue sem alocar.
struct ContiguousMessage
{
    std::vector<unsigned char> storage;
    const unsigned char* data = nullptr;
    unsigned long long size = 0;

    static_assert(!std::is_nothrow_constructible_v<std::vector<unsigned char>, std::size_t>,
                  "vector(n) precisa poder lançar bad_alloc (ver o comentário acima)");

    explicit ContiguousMessage(MessageParts parts) : storage(std::size_t{0})
    {
        if (parts.size() == 1) {
            data = reinterpret_cast<const unsigned char*>(parts.begin()->data());
            size = parts.begin()->size();
            return;
        }
        std::size_t total = 0;
        for (const auto part : parts) {
            total += part.size();
        }
        storage.reserve(total);
        for (const auto part : parts) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(part.data());
            storage.insert(storage.end(), bytes, bytes + part.size());
        }
        data = storage.data();
        size = storage.size();
    }
};

} // namespace detail

} // namespace swarmstream::pack
