// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "swarmstream/http_range_source.hpp"

#include "tcp_socket.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swarmstream {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

// ------------------------------------------------------------------------------------ texto

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    text = trim(text);
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (text.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// ------------------------------------------------------------------------------------- URL

struct ParsedUrl
{
    std::string host;
    std::uint16_t port = 80;
    std::string base_path; // sem barra no fim; vazio = raiz
};

Result<ParsedUrl, std::string> parse_http_url(const std::string& url)
{
    constexpr std::string_view kScheme = "http://";
    if (lower(url).rfind("https://", 0) == 0) {
        return Unexpected(std::string("https:// exige TLS, que o sw-core ainda nao tem; use "
                                      "http:// (decisao pendente do dono)"));
    }
    if (lower(url).rfind(kScheme, 0) != 0) {
        return Unexpected(std::string("a URL deve comecar com http://"));
    }
    std::string_view rest = std::string_view(url).substr(kScheme.size());
    const std::size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    std::string_view path =
        slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);

    ParsedUrl parsed;
    std::string_view port_text;
    if (!authority.empty() && authority.front() == '[') { // IPv6 literal
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return Unexpected(std::string("URL com '[' sem ']'"));
        }
        parsed.host = std::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return Unexpected(std::string("URL invalida depois do ']'"));
            }
            port_text = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        parsed.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
        }
    }
    if (parsed.host.empty()) {
        return Unexpected(std::string("URL sem host"));
    }
    if (!port_text.empty()) {
        const auto port = parse_u64(port_text);
        if (!port || *port == 0 || *port > 65535) {
            return Unexpected(std::string("porta invalida na URL"));
        }
        parsed.port = static_cast<std::uint16_t>(*port);
    }
    if (path.find_first_of(" \r\n\t") != std::string_view::npos) {
        return Unexpected(std::string("URL com espacos ou quebras de linha"));
    }
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    parsed.base_path = std::string(path);
    return parsed;
}

// ------------------------------------------------------------------------------- resposta HTTP

struct ContentRange
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::optional<std::uint64_t> total;
};

struct ResponseHead
{
    int status = 0;
    std::optional<std::uint64_t> content_length;
    bool has_transfer_encoding = false;
    bool connection_close = false;
    std::optional<ContentRange> content_range;
};

// `head` = linha de status + cabeçalhos, sem a linha vazia do fim.
std::optional<ResponseHead> parse_head(std::string_view head)
{
    ResponseHead out;
    std::size_t line_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, line_end);
    if (status_line.rfind("HTTP/1.", 0) != 0 || status_line.size() < 12 || status_line[8] != ' ') {
        return std::nullopt;
    }
    const auto status = parse_u64(status_line.substr(9, 3));
    if (!status || *status < 100 || *status > 599) {
        return std::nullopt;
    }
    out.status = static_cast<int>(*status);

    while (line_end != std::string_view::npos) {
        const std::size_t start = line_end + 2;
        line_end = head.find("\r\n", start);
        const std::string_view line = head.substr(
            start, line_end == std::string_view::npos ? std::string_view::npos : line_end - start);
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "content-length") {
            const auto length = parse_u64(value);
            if (!length) {
                return std::nullopt;
            }
            if (out.content_length && *out.content_length != *length) {
                return std::nullopt; // Content-Length repetido e diferente
            }
            out.content_length = *length;
        } else if (name == "transfer-encoding") {
            out.has_transfer_encoding = true;
        } else if (name == "connection") {
            out.connection_close = lower(value).find("close") != std::string::npos;
        } else if (name == "content-range") {
            // "bytes <first>-<last>/<total|*>"
            std::string_view v = value;
            if (v.rfind("bytes ", 0) != 0) {
                return std::nullopt;
            }
            v.remove_prefix(6);
            const std::size_t dash = v.find('-');
            const std::size_t slash = v.find('/');
            if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash) {
                continue; // por exemplo "bytes */123" (resposta 416): sem faixa
            }
            const auto first = parse_u64(v.substr(0, dash));
            const auto last = parse_u64(v.substr(dash + 1, slash - dash - 1));
            if (!first || !last || *last < *first) {
                return std::nullopt;
            }
            ContentRange range{*first, *last, std::nullopt};
            const std::string_view total = trim(v.substr(slash + 1));
            if (total != "*") {
                const auto total_value = parse_u64(total);
                if (!total_value) {
                    return std::nullopt;
                }
                range.total = *total_value;
            }
            out.content_range = range;
        }
    }
    return out;
}

std::string hex_of(const PieceHash& hash) { return hash.to_hex(); }

PieceLocation default_location(const PieceId& piece, std::string_view base_path)
{
    return PieceLocation{std::string(base_path) + "/p/" + hex_of(piece.hash), std::nullopt};
}

} // namespace

// ================================================================================== Impl

class HttpRangeSource::Impl
{
public:
    Impl(HttpSourceConfig config, ParsedUrl url) : config_(std::move(config)), url_(std::move(url))
    {
        host_header_ = url_.host.find(':') != std::string::npos ? "[" + url_.host + "]" : url_.host;
        if (url_.port != 80) {
            host_header_ += ":" + std::to_string(url_.port);
        }
        if (!config_.locator) {
            config_.locator = default_location;
        }
    }

    ~Impl() { shutdown(); }

    void start()
    {
        workers_.reserve(config_.connections);
        for (unsigned i = 0; i < config_.connections; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    RequestTicket submit(const PieceId& piece, SteadyTimePoint deadline)
    {
        auto job = std::make_shared<Job>();
        job->piece = piece;
        job->deadline = deadline;
        RequestTicket ticket;
        ticket.future = job->promise.get_future();

        std::unique_lock lock(mutex_);
        job->id = ++last_id_;
        ticket.request_id = job->id;
        ++requests_total_;
        if (stopping_) {
            lock.unlock();
            finish_error(*job, SourceErrorCode::Canceled, "fonte encerrada");
            requests_canceled_.fetch_add(1);
            return ticket;
        }
        jobs_.emplace(job->id, job);
        pending_.emplace(std::make_pair(deadline, job->id), job);
        lock.unlock();
        wake_.notify_one();
        return ticket;
    }

    bool cancel(RequestId id)
    {
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(mutex_);
            const auto found = jobs_.find(id);
            if (found == jobs_.end()) {
                return false; // já entregue (ou nunca existiu)
            }
            job = found->second;
            if (job->state == Job::State::Pending) {
                pending_.erase(std::make_pair(job->deadline, job->id));
                jobs_.erase(found);
                job->state = Job::State::Done;
            } else {
                job->canceled.store(true);
                if (job->socket != nullptr) {
                    job->socket->shutdown_both(); // acorda o recv/send do worker
                }
                return true;
            }
        }
        finish_error(*job, SourceErrorCode::Canceled, "cancelado");
        requests_canceled_.fetch_add(1);
        return true;
    }

    SourceStats stats() const
    {
        SourceStats s;
        s.requests_total = requests_total_.load();
        s.requests_completed = requests_completed_.load();
        s.requests_canceled = requests_canceled_.load();
        s.requests_failed = requests_failed_.load();
        s.bytes_received = bytes_received_.load();
        return s;
    }

    HttpSourceDiagnostics diagnostics() const
    {
        HttpSourceDiagnostics d;
        d.connections_opened = connections_opened_.load();
        d.requests_on_reused_connection = reused_requests_.load();
        d.retries = retries_.load();
        d.resumed_requests = resumed_requests_.load();
        d.http_errors = http_errors_.load();
        return d;
    }

    void shutdown()
    {
        std::vector<std::shared_ptr<Job>> to_cancel;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            for (auto& [key, job] : pending_) {
                job->state = Job::State::Done;
                to_cancel.push_back(job);
                jobs_.erase(job->id);
            }
            pending_.clear();
            for (auto& [id, job] : jobs_) { // em andamento
                job->canceled.store(true);
                if (job->socket != nullptr) {
                    job->socket->shutdown_both();
                }
            }
        }
        wake_.notify_all();
        for (auto& job : to_cancel) {
            finish_error(*job, SourceErrorCode::Canceled, "fonte encerrada");
            requests_canceled_.fetch_add(1);
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    struct Job
    {
        enum class State { Pending, InFlight, Done };
        RequestId id = kInvalidRequestId;
        PieceId piece{};
        SteadyTimePoint deadline{};
        std::promise<PieceResponse> promise;
        State state = State::Pending;
        std::atomic<bool> canceled{false};
        net::TcpSocket* socket = nullptr; // protegido por Impl::mutex_
    };

    enum class AttemptKind { Done, RetryFresh, Retry, Fatal };
    struct Attempt
    {
        AttemptKind kind = AttemptKind::Fatal;
        SourceErrorCode code = SourceErrorCode::NetworkError;
        std::string message;
    };

    // Estado de uma peça entre tentativas (para retomar com Range).
    struct Download
    {
        PieceLocation location;
        std::vector<std::uint8_t> body;
        std::uint64_t expected = 0; // tamanho final da peça; 0 = ainda não sabido
    };

    struct Connection
    {
        net::TcpSocket socket;
    };

    // --------------------------------------------------------------------------- entrega

    void finish_error(Job& job, SourceErrorCode code, std::string message)
    {
        PieceResponse response;
        response.piece = job.piece;
        response.error = code;
        response.error_message = std::move(message);
        job.promise.set_value(std::move(response));
    }

    // ------------------------------------------------------------------------ worker

    void worker_loop()
    {
        Connection connection;
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_) {
                    break;
                }
                const auto first = pending_.begin();
                job = first->second;
                pending_.erase(first);
                job->state = Job::State::InFlight;
            }
            process(*job, connection);
        }
        drop_connection(connection, nullptr);
    }

    void adopt_connection(Connection& connection, net::TcpSocket socket, Job& job)
    {
        std::lock_guard lock(mutex_);
        connection.socket = std::move(socket);
        job.socket = &connection.socket;
    }

    void drop_connection(Connection& connection, Job* job)
    {
        std::lock_guard lock(mutex_);
        if (job != nullptr) {
            job->socket = nullptr;
        }
        connection.socket.close();
    }

    void process(Job& job, Connection& connection)
    {
        const auto started = std::chrono::steady_clock::now();
        Download download;
        download.location = config_.locator(job.piece, url_.base_path);
        if (download.location.range) {
            const auto [first, last] = *download.location.range;
            if (last < first) {
                complete(job, connection, download, SourceErrorCode::NetworkError,
                         "faixa invalida");
                return;
            }
            download.expected = last - first + 1;
        }

        {
            std::lock_guard lock(mutex_);
            job.socket = connection.socket.valid() ? &connection.socket : nullptr;
        }

        unsigned attempt_number = 0;
        SourceErrorCode last_code = SourceErrorCode::NetworkError;
        std::string last_message;
        while (true) {
            if (job.canceled.load()) {
                complete(job, connection, download, SourceErrorCode::Canceled, "cancelado");
                return;
            }
            if (std::chrono::steady_clock::now() - started > config_.request_timeout) {
                complete(job, connection, download, SourceErrorCode::Timeout,
                         "prazo da peca esgotado: " + last_message);
                return;
            }

            const Attempt attempt = try_once(job, connection, download);
            if (attempt.kind == AttemptKind::Done) {
                break;
            }
            if (job.canceled.load()) {
                complete(job, connection, download, SourceErrorCode::Canceled, "cancelado");
                return;
            }
            if (attempt.kind == AttemptKind::Fatal) {
                complete(job, connection, download, attempt.code, attempt.message);
                return;
            }
            last_code = attempt.code;
            last_message = attempt.message;
            if (attempt.kind == AttemptKind::RetryFresh) {
                continue; // conexão parada que o servidor fechou: refaz sem gastar tentativa
            }
            ++attempt_number;
            retries_.fetch_add(1);
            if (attempt_number >= config_.max_attempts) {
                complete(job, connection, download, last_code,
                         last_message + " (apos " + std::to_string(attempt_number) +
                             " tentativas)");
                return;
            }
            backoff(job, attempt_number);
        }

        if (config_.verifier && !config_.verifier(job.piece, download.body)) {
            complete(job, connection, download, SourceErrorCode::CorruptedData,
                     "verificacao do hash da peca falhou");
            return;
        }
        complete(job, connection, download, SourceErrorCode::Success, {});
    }

    void backoff(const Job& job, unsigned attempt_number)
    {
        auto remaining =
            std::chrono::milliseconds(std::min<unsigned>(20U << (attempt_number - 1), 500U));
        constexpr std::chrono::milliseconds kSlice{10};
        while (remaining.count() > 0 && !job.canceled.load()) {
            const auto step = std::min(remaining, kSlice);
            std::this_thread::sleep_for(step);
            remaining -= step;
        }
    }

    // Entrega o resultado, atualiza contadores e tira o job das tabelas.
    void complete(Job& job, Connection& connection, Download& download, SourceErrorCode code,
                  std::string message)
    {
        {
            std::lock_guard lock(mutex_);
            job.socket = nullptr;
            job.state = Job::State::Done;
            jobs_.erase(job.id);
        }
        PieceResponse response;
        response.piece = job.piece;
        if (code == SourceErrorCode::Success && !job.canceled.load()) {
            response.data = std::move(download.body);
            requests_completed_.fetch_add(1);
        } else if (code ==
                   SourceErrorCode::Success) { // cancelado depois de chegar: vale o cancelamento
            response.error = SourceErrorCode::Canceled;
            response.error_message = "cancelado";
            requests_canceled_.fetch_add(1);
        } else if (code == SourceErrorCode::Canceled) {
            response.error = code;
            response.error_message = std::move(message);
            requests_canceled_.fetch_add(1);
        } else {
            response.error = code;
            response.error_message = std::move(message);
            requests_failed_.fetch_add(1);
        }
        (void)connection;
        job.promise.set_value(std::move(response));
    }

    // ---------------------------------------------------------------------- uma tentativa

    Attempt outcome(AttemptKind kind, SourceErrorCode code, std::string message)
    {
        return Attempt{kind, code, std::move(message)};
    }

    Attempt try_once(Job& job, Connection& connection, Download& download)
    {
        const bool reused = connection.socket.valid();
        if (!reused) {
            std::string error;
            auto socket =
                net::TcpSocket::connect(url_.host, url_.port, config_.connect_timeout, error);
            if (!socket) {
                return outcome(AttemptKind::Retry, SourceErrorCode::NetworkError,
                               "conexao: " + error);
            }
            socket->set_io_timeout(config_.io_timeout);
            adopt_connection(connection, std::move(*socket), job);
            connections_opened_.fetch_add(1);
        }
        if (job.canceled.load()) {
            drop_connection(connection, &job);
            return outcome(AttemptKind::Retry, SourceErrorCode::Canceled, "cancelado");
        }

        // ---- pedido
        const std::size_t offset = download.body.size();
        std::string request =
            "GET " + download.location.path + " HTTP/1.1\r\nHost: " + host_header_ +
            "\r\nUser-Agent: swarmstream-sw-core/0\r\nAccept: */*\r\nConnection: keep-alive\r\n";
        std::uint64_t requested_first = 0;
        bool sent_range = false;
        if (download.location.range) {
            requested_first = download.location.range->first + offset;
            request += "Range: bytes=" + std::to_string(requested_first) + "-" +
                       std::to_string(download.location.range->second) + "\r\n";
            sent_range = true;
        } else if (offset > 0) {
            requested_first = offset;
            request += "Range: bytes=" + std::to_string(offset) + "-\r\n";
            sent_range = true;
        }
        request += "\r\n";
        if (reused) {
            reused_requests_.fetch_add(1);
        }
        if (offset > 0) {
            resumed_requests_.fetch_add(1);
        }

        const net::IoStatus sent = connection.socket.send_all(request.data(), request.size());
        if (sent != net::IoStatus::Ok) {
            drop_connection(connection, &job);
            return outcome(reused ? AttemptKind::RetryFresh : AttemptKind::Retry,
                           sent == net::IoStatus::Timeout ? SourceErrorCode::Timeout
                                                          : SourceErrorCode::NetworkError,
                           "envio do pedido falhou");
        }

        // ---- cabeçalho
        std::vector<std::uint8_t> head;
        head.reserve(1024);
        std::size_t head_end = std::string::npos;
        std::uint8_t chunk[4096];
        while (head_end == std::string::npos) {
            const net::RecvResult got = connection.socket.recv_some(chunk, sizeof(chunk));
            if (got.status != net::IoStatus::Ok) {
                drop_connection(connection, &job);
                const bool silent_close = head.empty() && got.status != net::IoStatus::Timeout;
                // Conexão reaproveitada que o servidor já tinha fechado: tenta de novo sem gastar
                // tentativa.
                return outcome(
                    reused && silent_close ? AttemptKind::RetryFresh : AttemptKind::Retry,
                    got.status == net::IoStatus::Timeout ? SourceErrorCode::Timeout
                                                         : SourceErrorCode::NetworkError,
                    got.status == net::IoStatus::Timeout ? "tempo esgotado esperando a resposta"
                                                         : "conexao fechada antes da resposta");
            }
            head.insert(head.end(), chunk, chunk + got.bytes);
            const std::string_view view(reinterpret_cast<const char*>(head.data()), head.size());
            const std::size_t found = view.find("\r\n\r\n");
            if (found != std::string_view::npos) {
                head_end = found;
            } else if (head.size() > kMaxHeaderBytes) {
                drop_connection(connection, &job);
                return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                               "cabecalho HTTP maior que o limite");
            }
        }
        const std::string_view head_view(reinterpret_cast<const char*>(head.data()), head_end);
        const auto parsed = parse_head(head_view);
        if (!parsed) {
            drop_connection(connection, &job);
            return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                           "resposta HTTP invalida");
        }
        const ResponseHead& response = *parsed;
        const std::size_t body_start = head_end + 4;

        // ---- status
        if (response.status != 200 && response.status != 206) {
            http_errors_.fetch_add(1);
            drop_connection(connection, &job); // não lê o corpo de erro; abre outra conexão depois
            const std::string text = "HTTP " + std::to_string(response.status);
            if (response.status == 404) {
                return outcome(AttemptKind::Fatal, SourceErrorCode::NotFound,
                               text + ": peca nao encontrada");
            }
            if (response.status >= 500 || response.status == 408 || response.status == 429) {
                return outcome(AttemptKind::Retry, SourceErrorCode::NetworkError, text);
            }
            return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError, text);
        }
        if (response.has_transfer_encoding || !response.content_length) {
            drop_connection(connection, &job);
            return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                           "resposta sem Content-Length (Transfer-Encoding nao suportado)");
        }
        const std::uint64_t length = *response.content_length;

        std::uint64_t piece_size = 0; // tamanho final da peça, conforme esta resposta
        if (response.status == 200) {
            if (download.location.range) {
                drop_connection(connection, &job);
                return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                               "o servidor ignorou o Range e mandou o arquivo inteiro");
            }
            download.body.clear(); // se pedimos a retomada e veio tudo, recomeça do zero
            piece_size = length;
        } else { // 206
            if (!sent_range || !response.content_range) {
                drop_connection(connection, &job);
                return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                               "206 sem Range pedido ou sem Content-Range");
            }
            const ContentRange& cr = *response.content_range;
            if (cr.first != requested_first || cr.last - cr.first + 1 != length) {
                drop_connection(connection, &job);
                return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                               "Content-Range diferente do pedido");
            }
            if (download.location.range) {
                if (cr.last != download.location.range->second) {
                    drop_connection(connection, &job);
                    return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                                   "Content-Range termina fora da faixa pedida");
                }
                piece_size = download.expected;
            } else {
                if (!cr.total || cr.last + 1 != *cr.total ||
                    (download.expected != 0 && *cr.total != download.expected)) {
                    drop_connection(connection, &job);
                    return outcome(AttemptKind::Fatal, SourceErrorCode::NetworkError,
                                   "Content-Range com tamanho total inconsistente");
                }
                piece_size = *cr.total;
            }
        }
        if (piece_size > config_.max_piece_bytes || piece_size == 0 ||
            download.body.size() + length != piece_size) {
            drop_connection(connection, &job);
            return outcome(AttemptKind::Fatal, SourceErrorCode::CorruptedData,
                           piece_size > config_.max_piece_bytes ? "peca maior que o limite"
                                                                : "tamanho da peca invalido");
        }
        download.expected = piece_size;

        // ---- corpo
        const std::size_t base = download.body.size();
        download.body.resize(base + static_cast<std::size_t>(length));
        std::size_t filled = 0;
        const std::size_t leftover = std::min<std::size_t>(head.size() - body_start, length);
        if (leftover > 0) {
            std::copy_n(head.begin() + static_cast<std::ptrdiff_t>(body_start), leftover,
                        download.body.begin() + static_cast<std::ptrdiff_t>(base));
            filled = leftover;
            bytes_received_.fetch_add(leftover);
        }
        while (filled < length) {
            const net::RecvResult got =
                connection.socket.recv_some(download.body.data() + base + filled, length - filled);
            if (got.status != net::IoStatus::Ok) {
                download.body.resize(base + filled); // guarda o que chegou para retomar
                drop_connection(connection, &job);
                return outcome(AttemptKind::Retry,
                               got.status == net::IoStatus::Timeout ? SourceErrorCode::Timeout
                                                                    : SourceErrorCode::NetworkError,
                               got.status == net::IoStatus::Timeout
                                   ? "tempo esgotado no corpo"
                                   : "conexao caiu no meio do corpo");
            }
            filled += got.bytes;
            bytes_received_.fetch_add(got.bytes);
        }
        // Bytes além do Content-Length na mesma resposta quebrariam o próximo pedido: fecha a
        // conexão.
        const bool stray_bytes = head.size() - body_start > length;
        if (response.connection_close || stray_bytes) {
            drop_connection(connection, &job);
        }
        return Attempt{AttemptKind::Done, SourceErrorCode::Success, {}};
    }

    // ------------------------------------------------------------------------- dados

    HttpSourceConfig config_;
    ParsedUrl url_;
    std::string host_header_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    RequestId last_id_ = kInvalidRequestId;
    std::map<std::pair<SteadyTimePoint, RequestId>, std::shared_ptr<Job>> pending_;
    std::unordered_map<RequestId, std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> workers_;

    std::atomic<std::uint64_t> requests_total_{0};
    std::atomic<std::uint64_t> requests_completed_{0};
    std::atomic<std::uint64_t> requests_canceled_{0};
    std::atomic<std::uint64_t> requests_failed_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> connections_opened_{0};
    std::atomic<std::uint64_t> reused_requests_{0};
    std::atomic<std::uint64_t> retries_{0};
    std::atomic<std::uint64_t> resumed_requests_{0};
    std::atomic<std::uint64_t> http_errors_{0};
};

// =============================================================================== público

HttpRangeSource::HttpRangeSource(std::string name, std::unique_ptr<Impl> impl)
    : name_(std::move(name)), impl_(std::move(impl))
{
}

HttpRangeSource::~HttpRangeSource() = default;

Result<std::unique_ptr<HttpRangeSource>, std::string>
HttpRangeSource::create(HttpSourceConfig config)
{
    if (config.connections < 1 || config.connections > 64) {
        return Unexpected(std::string("connections deve estar entre 1 e 64"));
    }
    if (config.max_attempts < 1 || config.max_attempts > 10) {
        return Unexpected(std::string("max_attempts deve estar entre 1 e 10"));
    }
    if (config.max_piece_bytes == 0) {
        return Unexpected(std::string("max_piece_bytes deve ser maior que 0"));
    }
    if (config.connect_timeout.count() <= 0 || config.io_timeout.count() <= 0 ||
        config.request_timeout.count() <= 0) {
        return Unexpected(std::string("os timeouts devem ser maiores que 0"));
    }
    auto url = parse_http_url(config.base_url);
    if (!url.has_value()) {
        return Unexpected(std::string(url.error()));
    }
    std::string name = config.name;
    auto impl = std::make_unique<Impl>(std::move(config), std::move(url.value()));
    impl->start();
    return std::unique_ptr<HttpRangeSource>(new HttpRangeSource(std::move(name), std::move(impl)));
}

RequestTicket HttpRangeSource::request(const PieceId& piece, SteadyTimePoint deadline)
{
    return impl_->submit(piece, deadline);
}

bool HttpRangeSource::cancel(RequestId request_id) { return impl_->cancel(request_id); }

SourceStats HttpRangeSource::stats() const { return impl_->stats(); }

HttpSourceDiagnostics HttpRangeSource::diagnostics() const { return impl_->diagnostics(); }

} // namespace swarmstream
