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
#include <vector>

namespace swarmstream::pack {

using MessageParts = std::initializer_list<std::span<const std::byte>>;

namespace detail {

// A mensagem contígua que o libsodium exige. Com uma só parte não copia. Pode lançar
// std::bad_alloc na cópia (os chamadores capturam e tratam como falha).
struct ContiguousMessage
{
    std::vector<unsigned char> storage;
    const unsigned char* data = nullptr;
    unsigned long long size = 0;

    explicit ContiguousMessage(MessageParts parts)
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
