// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Baixa peças de um servidor HTTP com a fonte HttpRangeSource (F1-27) e grava cada uma em
// <saida>/<nome>.bin. Usado por http_netlab_test.py contra o servidor da bancada (sw-netlab).
//   http_fetch_tool <url-base> <pasta-de-saida> <conexoes> <nome-hex-64> [<nome-hex-64> ...]
// O nome de cada peça é o seu SHA-256 em hexa (64 dígitos); o servidor a serve em <base>/p/<nome>.

#include "swarmstream/http_range_source.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace sw = swarmstream;

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "uso: %s <url-base> <saida> <conexoes> <nome> [<nome> ...]\n",
                     argv[0]);
        return 2;
    }
    sw::HttpSourceConfig config;
    config.base_url = argv[1];
    config.connections = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    config.io_timeout = std::chrono::seconds(30);
    config.request_timeout = std::chrono::seconds(120);
    auto created = sw::HttpRangeSource::create(config);
    if (!created.has_value()) {
        std::fprintf(stderr, "configuracao invalida: %s\n", created.error().c_str());
        return 2;
    }
    auto source = std::move(created).value();

    const auto started = std::chrono::steady_clock::now();
    std::vector<sw::RequestTicket> tickets;
    std::vector<std::string> names;
    for (int i = 4; i < argc; ++i) {
        names.emplace_back(argv[i]);
        const sw::PieceId piece(static_cast<std::uint32_t>(i - 4),
                                sw::PieceHash::from_hex(names.back()));
        tickets.push_back(source->request(piece, started + std::chrono::seconds(10)));
    }

    int failures = 0;
    for (std::size_t i = 0; i < tickets.size(); ++i) {
        const sw::PieceResponse response = tickets[i].future.get();
        if (!response.ok()) {
            std::fprintf(stderr, "FALHA %s: %s (%s)\n", names[i].c_str(),
                         std::string(sw::to_string(response.error)).c_str(),
                         response.error_message.c_str());
            ++failures;
            continue;
        }
        const std::string path = std::string(argv[2]) + "/" + names[i] + ".bin";
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(response.data.data()),
                  static_cast<std::streamsize>(response.data.size()));
        out.close();
        if (!out) {
            std::fprintf(stderr, "FALHA ao gravar %s\n", path.c_str());
            ++failures;
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const sw::SourceStats stats = source->stats();
    const sw::HttpSourceDiagnostics diag = source->diagnostics();
    std::printf("RESULT seconds=%.3f pieces=%zu bytes=%llu failed=%d connections=%llu reused=%llu "
                "retries=%llu\n",
                seconds, names.size(), static_cast<unsigned long long>(stats.bytes_received),
                failures, static_cast<unsigned long long>(diag.connections_opened),
                static_cast<unsigned long long>(diag.requests_on_reused_connection),
                static_cast<unsigned long long>(diag.retries));
    return failures == 0 ? 0 : 1;
}
