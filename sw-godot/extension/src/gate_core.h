// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// GateCore: adaptador provisório do portão da F1-20 (ADR-0016).
//
// Enquanto o sw-core real não existe (swarmstream.h ainda é stub), o loader do
// Godot conversa com este adaptador, que imita a semântica CONGELADA do
// swarmstream.h v0 sobre os mocks do sw-core (MemoryContentSource como "CDN" e
// MemoryCacheStore como cache):
//   - read() com timeout_ms = 0: se algum byte não está no cache, devolve
//     SW_E_NOT_LOCAL na hora, sem copiar nada, mas o pedido de rede É FEITO;
//   - ensure_async(): garante o arquivo no cache, prende os bytes por lease até
//     release() (o papel do sw_close) e chama o callback exatamente uma vez,
//     numa thread de conclusão do adaptador, nunca dentro da própria chamada.
// Quando o sw-core real existir, o loader troca GateCore pelas funções sw_*.
//
// Este cabeçalho não depende do Godot nem de exceções: o godot-cpp compila com
// -fno-exceptions, e os mocks do sw-core usam exceções (Result::value).

#pragma once

#include "swarmstream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace swgate {

struct GateStats
{
    uint64_t reads = 0;           // chamadas a read()
    uint64_t reads_not_local = 0; // read() que devolveu SW_E_NOT_LOCAL
    uint64_t ensures = 0;
    uint64_t pieces_fetched = 0; // peças trazidas da "CDN" e gravadas no cache
};

// Callback de ensure_async: resultado, bytes locais do arquivo e o id do pedido
// (o mesmo que ensure_async devolveu; passar a release() depois de usar os bytes).
using EnsureCallback =
    std::function<void(sw_result result, uint64_t bytes_local, uint64_t ensure_id)>;

class GateCore
{
public:
    static GateCore& instance();

    // Lê os arquivos empacotados em pack_dir (simula o armazenamento da CDN),
    // semeia a fonte em memória com latência simulada e grava no cache só os
    // arquivos do grupo core (os de sufixo ".low.").
    sw_result init(const std::string& pack_dir, uint32_t latency_ms);
    void shutdown();
    bool initialized() const;

    sw_result file_size(const std::string& path, uint64_t* out_size) const;
    bool is_local(const std::string& path) const;

    // Semântica de sw_read com timeout_ms = 0 (tudo ou nada).
    sw_result read(const std::string& path, uint64_t offset, void* dst, uint64_t size,
                   uint64_t* out_bytes_read);

    // Semântica de sw_ensure_async sobre o arquivo inteiro. Devolve o id do
    // pedido (0 em erro; nesse caso o callback nunca é chamado).
    uint64_t ensure_async(const std::string& path, EnsureCallback cb);

    // Solta os leases de um ensure (papel do sw_close).
    void release(uint64_t ensure_id);

    GateStats stats() const;

private:
    GateCore();
    ~GateCore();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace swgate
