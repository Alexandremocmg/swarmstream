// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Testes da fonte HTTP (F1-27) contra um servidor HTTP falso em processo (loopback). O teste com
// o servidor da bancada nos três perfis fica em http_netlab_test.py.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
// windows.h precisa vir antes de tlhelp32.h; o clang-format reordenaria.
#include <windows.h>
#include <tlhelp32.h>
// clang-format on
#else
#include <dirent.h>
#endif

#include "../src/tcp_socket.hpp"
#include "check.hpp"
#include "swarmstream/http_range_source.hpp"

namespace swarmstream {
namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

Bytes make_bytes(std::size_t size, std::uint8_t seed)
{
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>(i * 31 + seed);
    }
    return out;
}

PieceId make_piece(std::uint32_t index)
{
    PieceHash hash;
    hash.bytes[0] = static_cast<std::uint8_t>(index);
    hash.bytes[1] = static_cast<std::uint8_t>(index >> 8);
    return PieceId(index, hash);
}

std::string path_of(const PieceId& piece) { return "/p/" + piece.hash.to_hex(); }

// ------------------------------------------------------------------ servidor HTTP falso

struct FakeServerOptions
{
    int drop_first_connections = 0;    // aceita e fecha sem responder
    std::size_t truncate_first_at = 0; // 1ª resposta completa de cada caminho corta o corpo aqui
    int error_status_first = 0;        // status devolvido nas primeiras `error_count` requisições
    int error_count = 0;
    bool close_without_header = false;  // fecha a conexão depois de cada resposta, sem avisar
    std::chrono::milliseconds delay{0}; // espera antes de responder
    bool ignore_range = false;          // responde 200 com o objeto inteiro mesmo com Range
};

class FakeServer
{
public:
    explicit FakeServer(FakeServerOptions options = {}) : options_(options)
    {
        std::string error;
        auto listener = net::TcpListener::listen_loopback(0, error);
        SW_CHECK(listener.has_value());
        listener_ = std::move(*listener);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~FakeServer()
    {
        stop_.store(true);
        accept_thread_.join();
        for (auto& handler : handlers_) {
            handler.join();
        }
    }

    void add(const std::string& path, Bytes data)
    {
        std::lock_guard lock(mutex_);
        objects_[path] = std::move(data);
    }

    [[nodiscard]] std::string url() const
    {
        return "http://127.0.0.1:" + std::to_string(listener_.port());
    }
    [[nodiscard]] int connections() const { return connections_.load(); }
    [[nodiscard]] int requests() const { return requests_.load(); }
    [[nodiscard]] std::vector<std::string> served() const
    {
        std::lock_guard lock(mutex_);
        return served_;
    }
    [[nodiscard]] std::vector<std::string> range_headers() const
    {
        std::lock_guard lock(mutex_);
        return ranges_;
    }

private:
    void accept_loop()
    {
        while (!stop_.load()) {
            auto client = listener_.accept(50ms);
            if (!client) {
                continue;
            }
            const int number = ++connections_;
            if (number <= options_.drop_first_connections) {
                continue; // o soquete fecha ao sair do escopo
            }
            handlers_.emplace_back([this, socket = std::make_shared<net::TcpSocket>(
                                              std::move(*client))] { serve(*socket); });
        }
    }

    void serve(net::TcpSocket& socket)
    {
        socket.set_io_timeout(200ms);
        std::string pending;
        while (!stop_.load()) {
            std::size_t end = pending.find("\r\n\r\n");
            while (end == std::string::npos && !stop_.load()) {
                char buffer[2048];
                const net::RecvResult got = socket.recv_some(buffer, sizeof(buffer));
                if (got.status == net::IoStatus::Timeout) {
                    continue;
                }
                if (got.status != net::IoStatus::Ok) {
                    return;
                }
                pending.append(buffer, got.bytes);
                end = pending.find("\r\n\r\n");
            }
            if (end == std::string::npos) {
                return;
            }
            const std::string head = pending.substr(0, end);
            pending.erase(0, end + 4);
            if (!respond(socket, head)) {
                return;
            }
        }
    }

    bool respond(net::TcpSocket& socket, const std::string& head)
    {
        const int request_number = ++requests_;
        const std::size_t path_start = head.find(' ') + 1;
        const std::string path = head.substr(path_start, head.find(' ', path_start) - path_start);
        std::string range;
        const std::size_t range_pos = head.find("Range: bytes=");
        if (range_pos != std::string::npos) {
            range = head.substr(range_pos + 13, head.find("\r\n", range_pos) - range_pos - 13);
            std::lock_guard lock(mutex_);
            ranges_.push_back(range);
        }
        {
            std::lock_guard lock(mutex_);
            served_.push_back(path);
        }
        if (options_.delay.count() > 0) {
            std::this_thread::sleep_for(options_.delay);
        }
        if (request_number <= options_.error_count) {
            return send_text(socket, options_.error_status_first, "erro", false);
        }
        Bytes data;
        {
            std::lock_guard lock(mutex_);
            const auto found = objects_.find(path);
            if (found == objects_.end()) {
                return send_text(socket, 404, "nao achei", false);
            }
            data = found->second;
        }
        std::size_t first = 0;
        std::size_t last = data.size() - 1;
        int status = 200;
        if (!range.empty() && !options_.ignore_range) {
            const std::size_t dash = range.find('-');
            first = std::stoull(range.substr(0, dash));
            if (dash + 1 < range.size()) {
                last = std::stoull(range.substr(dash + 1));
            }
            if (first >= data.size() || last >= data.size() || last < first) {
                return send_text(socket, 416, "faixa", false);
            }
            status = 206;
        }
        const std::size_t length = last - first + 1;
        std::string out =
            "HTTP/1.1 " + std::to_string(status) + (status == 206 ? " Partial Content" : " OK") +
            "\r\nContent-Length: " + std::to_string(length) + "\r\nAccept-Ranges: bytes\r\n";
        if (status == 206) {
            out += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
                   "/" + std::to_string(data.size()) + "\r\n";
        }
        out += "\r\n";
        if (socket.send_all(out.data(), out.size()) != net::IoStatus::Ok) {
            return false;
        }
        std::size_t to_send = length;
        const bool cut = options_.truncate_first_at > 0 && truncated_.insert(path).second;
        if (cut) {
            to_send = std::min(to_send, options_.truncate_first_at);
        }
        if (socket.send_all(data.data() + first, to_send) != net::IoStatus::Ok) {
            return false;
        }
        return !cut && !options_.close_without_header;
    }

    bool send_text(net::TcpSocket& socket, int status, const char* text, bool keep)
    {
        const std::string body = text;
        const std::string out = "HTTP/1.1 " + std::to_string(status) +
                                " X\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\n\r\n" + body;
        return socket.send_all(out.data(), out.size()) == net::IoStatus::Ok && keep;
    }

    FakeServerOptions options_;
    net::TcpListener listener_;
    std::thread accept_thread_;
    std::vector<std::thread> handlers_;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> requests_{0};
    mutable std::mutex mutex_;
    std::map<std::string, Bytes> objects_;
    std::vector<std::string> ranges_;
    std::vector<std::string> served_;
    std::set<std::string> truncated_;
};

std::unique_ptr<HttpRangeSource> make_source(const FakeServer& server, HttpSourceConfig config = {})
{
    config.base_url = server.url();
    config.io_timeout = 2000ms;
    config.connect_timeout = 2000ms;
    auto source = HttpRangeSource::create(std::move(config));
    SW_CHECK(source.has_value());
    return std::move(source).value();
}

PieceResponse fetch(HttpRangeSource& source, const PieceId& piece)
{
    RequestTicket ticket = source.request(piece, std::chrono::steady_clock::now() + 1s);
    SW_CHECK(ticket.request_id != kInvalidRequestId);
    SW_CHECK(ticket.future.wait_for(30s) == std::future_status::ready);
    return ticket.future.get();
}

// --------------------------------------------------------------- recursos do processo

std::size_t process_threads()
{
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    SW_CHECK(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            count += entry.th32OwnerProcessID == GetCurrentProcessId() ? 1 : 0;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
#else
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (const dirent* entry = readdir(dir)) {
            count += entry->d_name[0] != '.' ? 1 : 0;
        }
        closedir(dir);
    }
    return count;
#endif
}

std::size_t process_handles()
{
#ifdef _WIN32
    DWORD count = 0;
    SW_CHECK(GetProcessHandleCount(GetCurrentProcess(), &count) != 0);
    return count;
#else
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (const dirent* entry = readdir(dir)) {
            count += entry->d_name[0] != '.' ? 1 : 0;
        }
        closedir(dir);
    }
    return count;
#endif
}

// ------------------------------------------------------------------------- testes

void test_persistent_connection_and_data()
{
    std::puts("[Test] HttpRangeSource: conexao persistente e dados corretos");
    FakeServer server;
    std::vector<Bytes> originals;
    for (std::uint32_t i = 0; i < 8; ++i) {
        originals.push_back(make_bytes(50'000 + i * 1000, static_cast<std::uint8_t>(i)));
        server.add(path_of(make_piece(i)), originals.back());
    }
    HttpSourceConfig config;
    config.connections = 1;
    auto source = make_source(server, config);
    for (std::uint32_t i = 0; i < 8; ++i) {
        const PieceResponse response = fetch(*source, make_piece(i));
        SW_CHECK(response.ok());
        SW_CHECK(response.data == originals[i]);
        SW_CHECK(response.piece == make_piece(i));
    }
    const HttpSourceDiagnostics d = source->diagnostics();
    SW_CHECK(d.connections_opened == 1);
    SW_CHECK(d.requests_on_reused_connection == 7);
    SW_CHECK(server.connections() == 1 && server.requests() == 8);
    const SourceStats s = source->stats();
    SW_CHECK(s.requests_total == 8 && s.requests_completed == 8 && s.requests_failed == 0);
    SW_CHECK(s.bytes_received == 50'000 * 8 + 1000 * 28);
    SW_CHECK(source->name() == "cdn");
}

void test_resume_with_range_after_truncation()
{
    std::puts("[Test] HttpRangeSource: retoma com Range quando a conexao cai no meio do corpo");
    FakeServerOptions options;
    options.truncate_first_at = 300;
    FakeServer server(options);
    const Bytes data = make_bytes(1000, 7);
    server.add(path_of(make_piece(1)), data);
    auto source = make_source(server);
    const PieceResponse response = fetch(*source, make_piece(1));
    SW_CHECK(response.ok());
    SW_CHECK(response.data == data);
    SW_CHECK(source->diagnostics().resumed_requests == 1);
    SW_CHECK(source->diagnostics().retries == 1);
    const auto ranges = server.range_headers();
    SW_CHECK(ranges.size() == 1 && ranges[0] == "300-");
    SW_CHECK(source->stats().bytes_received == 1000); // só os 300 + 700, nada repetido
}

void test_server_ignores_range_restarts_from_zero()
{
    std::puts("[Test] HttpRangeSource: servidor que ignora o Range (200) recomeca do zero");
    FakeServerOptions options;
    options.truncate_first_at = 100;
    options.ignore_range = true;
    FakeServer server(options);
    const Bytes data = make_bytes(500, 3);
    server.add(path_of(make_piece(2)), data);
    auto source = make_source(server);
    const PieceResponse response = fetch(*source, make_piece(2));
    SW_CHECK(response.ok() && response.data == data);
}

void test_http_errors()
{
    std::puts("[Test] HttpRangeSource: 404, 5xx com nova tentativa e 5xx persistente");
    {
        FakeServer server;
        auto source = make_source(server);
        const PieceResponse response = fetch(*source, make_piece(9));
        SW_CHECK(response.error == SourceErrorCode::NotFound);
        SW_CHECK(source->stats().requests_failed == 1);
    }
    {
        FakeServerOptions options;
        options.error_status_first = 503;
        options.error_count = 1;
        FakeServer server(options);
        server.add(path_of(make_piece(3)), make_bytes(2000, 1));
        auto source = make_source(server);
        const PieceResponse response = fetch(*source, make_piece(3));
        SW_CHECK(response.ok());
        SW_CHECK(source->diagnostics().retries == 1 && source->diagnostics().http_errors == 1);
    }
    {
        FakeServerOptions options;
        options.error_status_first = 500;
        options.error_count = 1000;
        FakeServer server(options);
        server.add(path_of(make_piece(3)), make_bytes(2000, 1));
        HttpSourceConfig config;
        config.max_attempts = 3;
        auto source = make_source(server, config);
        const PieceResponse response = fetch(*source, make_piece(3));
        SW_CHECK(response.error == SourceErrorCode::NetworkError);
        SW_CHECK(response.error_message.find("3 tentativas") != std::string::npos);
        SW_CHECK(server.requests() == 3);
    }
}

void test_dropped_connections_are_retried()
{
    std::puts("[Test] HttpRangeSource: conexoes derrubadas sem resposta sao refeitas");
    FakeServerOptions options;
    options.drop_first_connections = 2;
    FakeServer server(options);
    server.add(path_of(make_piece(4)), make_bytes(3000, 9));
    HttpSourceConfig config;
    config.connections = 1;
    config.max_attempts = 4;
    auto source = make_source(server, config);
    SW_CHECK(fetch(*source, make_piece(4)).ok());
    SW_CHECK(source->diagnostics().retries == 2);
}

void test_stale_keepalive_connection_is_replaced_silently()
{
    std::puts("[Test] HttpRangeSource: conexao parada fechada pelo servidor nao gasta tentativa");
    FakeServerOptions options;
    options.close_without_header = true; // fecha depois de cada resposta, sem "Connection: close"
    FakeServer server(options);
    for (std::uint32_t i = 0; i < 5; ++i) {
        server.add(path_of(make_piece(i)), make_bytes(4000, static_cast<std::uint8_t>(i)));
    }
    HttpSourceConfig config;
    config.connections = 1;
    config.max_attempts = 1; // se contasse tentativa, o segundo pedido falharia
    auto source = make_source(server, config);
    for (std::uint32_t i = 0; i < 5; ++i) {
        SW_CHECK(fetch(*source, make_piece(i)).ok());
    }
    SW_CHECK(source->diagnostics().retries == 0);
    SW_CHECK(source->diagnostics().connections_opened == 5);
}

void test_package_window_with_range()
{
    std::puts("[Test] HttpRangeSource: peca como janela (Range) de um arquivo de pacote");
    FakeServer server;
    const Bytes package = make_bytes(10'000, 5);
    server.add("/pkg/core.bin", package);
    HttpSourceConfig config;
    config.locator = [](const PieceId& piece, std::string_view) {
        const std::uint64_t start = std::uint64_t{piece.index} * 1000;
        return PieceLocation{"/pkg/core.bin", std::make_pair(start, start + 999)};
    };
    auto source = make_source(server, config);
    const PieceResponse response = fetch(*source, make_piece(3));
    SW_CHECK(response.ok());
    SW_CHECK(response.data == Bytes(package.begin() + 3000, package.begin() + 4000));
    const auto ranges = server.range_headers();
    SW_CHECK(ranges.size() == 1 && ranges[0] == "3000-3999");

    // servidor que ignora o Range e manda o pacote inteiro: recusado
    FakeServerOptions ignoring;
    ignoring.ignore_range = true;
    FakeServer bad(ignoring);
    bad.add("/pkg/core.bin", package);
    auto source2 = make_source(bad, config);
    SW_CHECK(fetch(*source2, make_piece(3)).error == SourceErrorCode::NetworkError);
}

void test_limits_and_verifier()
{
    std::puts("[Test] HttpRangeSource: limite de tamanho, verificador e configuracao invalida");
    FakeServer server;
    server.add(path_of(make_piece(1)), make_bytes(5000, 1));
    {
        HttpSourceConfig config;
        config.max_piece_bytes = 4096;
        auto source = make_source(server, config);
        SW_CHECK(fetch(*source, make_piece(1)).error == SourceErrorCode::CorruptedData);
    }
    {
        HttpSourceConfig config;
        config.verifier = [](const PieceId&, std::span<const std::uint8_t>) { return false; };
        auto source = make_source(server, config);
        SW_CHECK(fetch(*source, make_piece(1)).error == SourceErrorCode::CorruptedData);
    }
    for (const char* url : {"https://example.com", "ftp://x", "http://", "http://host:0", "host"}) {
        HttpSourceConfig config;
        config.base_url = url;
        SW_CHECK(!HttpRangeSource::create(config).has_value());
    }
    HttpSourceConfig bad;
    bad.base_url = "http://127.0.0.1:1";
    bad.connections = 0;
    SW_CHECK(!HttpRangeSource::create(bad).has_value());
}

void test_connection_refused_fails_with_error()
{
    std::puts("[Test] HttpRangeSource: servidor fora do ar vira erro, nao travamento");
    std::string error;
    auto listener = net::TcpListener::listen_loopback(0, error);
    SW_CHECK(listener.has_value());
    const std::uint16_t port = listener->port();
    listener->close(); // ninguem escuta nessa porta
    HttpSourceConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.max_attempts = 2;
    config.connect_timeout = 500ms;
    auto source = std::move(HttpRangeSource::create(config)).value();
    const PieceResponse response = fetch(*source, make_piece(1));
    SW_CHECK(!response.ok() && response.error == SourceErrorCode::NetworkError);
}

void test_cancel_and_shutdown()
{
    std::puts("[Test] HttpRangeSource: cancelamento (na fila e em andamento) e encerramento");
    FakeServerOptions options;
    options.delay = 400ms;
    FakeServer server(options);
    for (std::uint32_t i = 0; i < 3; ++i) {
        server.add(path_of(make_piece(i)), make_bytes(1000, static_cast<std::uint8_t>(i)));
    }
    HttpSourceConfig config;
    config.connections = 1;
    auto source = make_source(server, config);

    RequestTicket running = source->request(make_piece(0), std::chrono::steady_clock::now() + 1s);
    std::this_thread::sleep_for(100ms); // o primeiro já foi para o servidor
    RequestTicket queued = source->request(make_piece(1), std::chrono::steady_clock::now() + 2s);
    RequestTicket never = source->request(make_piece(2), std::chrono::steady_clock::now() + 3s);

    SW_CHECK(source->cancel(queued.request_id)); // na fila
    SW_CHECK(queued.future.get().error == SourceErrorCode::Canceled);
    SW_CHECK(source->cancel(running.request_id)); // em andamento
    SW_CHECK(running.future.get().error == SourceErrorCode::Canceled);
    SW_CHECK(!source->cancel(running.request_id)); // já entregue
    SW_CHECK(!source->cancel(987654));             // desconhecido

    source.reset(); // encerra com `never` ainda pendente ou em andamento: não pode travar
    SW_CHECK(never.future.get().error == SourceErrorCode::Canceled);
}

void test_deadline_order()
{
    std::puts("[Test] HttpRangeSource: o pedido de prazo mais cedo sai primeiro");
    FakeServerOptions options;
    options.delay = 150ms;
    FakeServer server(options);
    for (std::uint32_t i = 0; i < 4; ++i) {
        server.add(path_of(make_piece(i)), make_bytes(500, static_cast<std::uint8_t>(i)));
    }
    HttpSourceConfig config;
    config.connections = 1;
    auto source = make_source(server, config);
    const auto now = std::chrono::steady_clock::now();
    RequestTicket first = source->request(make_piece(0), now + 10s); // ocupa o worker
    std::this_thread::sleep_for(50ms);
    RequestTicket late = source->request(make_piece(1), now + 9s);
    RequestTicket urgent = source->request(make_piece(2), now + 1s);
    SW_CHECK(first.future.get().ok());
    SW_CHECK(urgent.future.get().ok() && late.future.get().ok());
    const std::vector<std::string> order = server.served();
    SW_CHECK(order.size() == 3);
    SW_CHECK(order[0] == path_of(make_piece(0)));
    SW_CHECK(order[1] == path_of(make_piece(2))); // prazo mais cedo, apesar de ter chegado depois
    SW_CHECK(order[2] == path_of(make_piece(1)));
}

void test_long_session_bounded_resources()
{
    std::puts("[Test] HttpRangeSource: sessao longa com threads e handles limitados");
    FakeServer server;
    constexpr std::uint32_t kPieces = 16;
    for (std::uint32_t i = 0; i < kPieces; ++i) {
        server.add(path_of(make_piece(i)), make_bytes(4096, static_cast<std::uint8_t>(i)));
    }
    HttpSourceConfig config;
    config.connections = 4;
    auto source = make_source(server, config);
    // aquece: abre as conexões e deixa o servidor falso criar suas threads
    for (std::uint32_t i = 0; i < 64; ++i) {
        SW_CHECK(fetch(*source, make_piece(i % kPieces)).ok());
    }
    std::vector<std::future<PieceResponse>> warm;
    for (std::uint32_t i = 0; i < 16; ++i) {
        warm.push_back(
            source->request(make_piece(i % kPieces), std::chrono::steady_clock::now()).future);
    }
    for (auto& f : warm) {
        SW_CHECK(f.get().ok());
    }
    const std::size_t threads_before = process_threads();
    const std::size_t handles_before = process_handles();

    constexpr int kRounds = 250;
    constexpr int kBatch = 8;
    std::size_t max_threads = threads_before;
    std::size_t max_handles = handles_before;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<RequestTicket> batch;
        for (int k = 0; k < kBatch; ++k) {
            batch.push_back(
                source->request(make_piece(static_cast<std::uint32_t>((round + k) % kPieces)),
                                std::chrono::steady_clock::now() + 1s));
        }
        for (auto& ticket : batch) {
            SW_CHECK(ticket.future.get().ok());
        }
        if (round % 25 == 0) {
            max_threads = std::max(max_threads, process_threads());
            max_handles = std::max(max_handles, process_handles());
        }
    }
    max_threads = std::max(max_threads, process_threads());
    max_handles = std::max(max_handles, process_handles());

    const HttpSourceDiagnostics d = source->diagnostics();
    std::printf(
        "  %d pedidos: threads %zu -> max %zu, handles %zu -> max %zu, conexoes abertas %llu\n",
        kRounds * kBatch, threads_before, max_threads, handles_before, max_handles,
        static_cast<unsigned long long>(d.connections_opened));
    SW_CHECK(source->stats().requests_completed >= static_cast<std::uint64_t>(kRounds * kBatch));
    SW_CHECK(max_threads <= threads_before + 1); // nenhuma thread nova por pedido
    SW_CHECK(max_handles <= handles_before + 8); // nenhum handle acumulado por pedido
    SW_CHECK(d.connections_opened <= 4 + 2); // as 4 conexões persistentes (+ folga de reconexão)
}

} // namespace

void run_http_range_source_tests()
{
    test_persistent_connection_and_data();
    test_resume_with_range_after_truncation();
    test_server_ignores_range_restarts_from_zero();
    test_http_errors();
    test_dropped_connections_are_retried();
    test_stale_keepalive_connection_is_replaced_silently();
    test_package_window_with_range();
    test_limits_and_verifier();
    test_connection_refused_fails_with_error();
    test_cancel_and_shutdown();
    test_deadline_order();
    test_long_session_bounded_resources();
}

} // namespace swarmstream
