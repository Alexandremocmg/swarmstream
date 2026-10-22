// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "tcp_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace swarmstream::net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kBad = INVALID_SOCKET;

int last_error() noexcept { return WSAGetLastError(); }
bool is_timeout_error(int error) noexcept
{
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}

// Winsock precisa de WSAStartup antes de qualquer chamada. A inicialização é feita uma vez e
// não é desfeita (o processo termina logo depois), o que evita corridas com threads vivas.
bool ensure_network_started() noexcept
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
void close_native(NativeSocket s) noexcept { closesocket(s); }
bool set_non_blocking(NativeSocket s, bool enable) noexcept
{
    u_long mode = enable ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
using NativeSocket = int;
constexpr NativeSocket kBad = -1;

int last_error() noexcept { return errno; }
bool is_timeout_error(int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
}
bool ensure_network_started() noexcept { return true; }
void close_native(NativeSocket s) noexcept { ::close(s); }
bool set_non_blocking(NativeSocket s, bool enable) noexcept
{
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(s, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
#endif

NativeSocket to_native(std::intptr_t fd) noexcept { return static_cast<NativeSocket>(fd); }

std::string describe(int error)
{
#ifdef _WIN32
    return "erro de rede " + std::to_string(error);
#else
    return std::string(std::strerror(error));
#endif
}

// Espera o soquete ficar legível (read=true) ou gravável, até `timeout`. 1 = pronto, 0 = prazo.
int wait_ready(NativeSocket s, bool for_read, std::chrono::milliseconds timeout) noexcept
{
#ifdef _WIN32
    WSAPOLLFD entry{};
    entry.fd = s;
    entry.events = for_read ? POLLRDNORM : POLLWRNORM;
    return WSAPoll(&entry, 1, static_cast<INT>(timeout.count()));
#else
    pollfd entry{};
    entry.fd = s;
    entry.events = for_read ? POLLIN : POLLOUT;
    // Um sinal interrompe o poll (EINTR): refaz com o prazo que sobrou.
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now());
        const int rc = poll(&entry, 1, static_cast<int>(std::max<long long>(left.count(), 0)));
        if (rc >= 0 || errno != EINTR) {
            return rc;
        }
    }
#endif
}

bool apply_timeout(NativeSocket s, int option, std::chrono::milliseconds timeout) noexcept
{
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeout.count());
    return setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    return setsockopt(s, SOL_SOCKET, option, &tv, sizeof(tv)) == 0;
#endif
}

void tune(NativeSocket s) noexcept
{
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

} // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(std::exchange(other.fd_, kInvalid)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, kInvalid);
    }
    return *this;
}

std::optional<TcpSocket> TcpSocket::connect(const std::string& host, std::uint16_t port,
                                            std::chrono::milliseconds timeout, std::string& error)
{
    if (!ensure_network_started()) {
        error = "Winsock nao iniciou";
        return std::nullopt;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
    if (rc != 0 || found == nullptr) {
        error = "nao resolvi o host '" + host + "'";
        return std::nullopt;
    }

    std::string last_failure = "sem enderecos";
    for (const addrinfo* ai = found; ai != nullptr; ai = ai->ai_next) {
        const NativeSocket s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kBad) {
            last_failure = describe(last_error());
            continue;
        }
        bool connected = false;
        if (set_non_blocking(s, true)) {
            const int cr = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
            if (cr == 0) {
                connected = true;
            } else {
                const int e = last_error();
#ifdef _WIN32
                const bool in_progress = e == WSAEWOULDBLOCK;
#else
                const bool in_progress = e == EINPROGRESS;
#endif
                const int ready = in_progress ? wait_ready(s, false, timeout) : 0;
                if (in_progress && ready > 0) {
                    int so_error = 0;
#ifdef _WIN32
                    int len = sizeof(so_error);
#else
                    socklen_t len = sizeof(so_error);
#endif
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                                   &len) != 0) {
                        last_failure = "getsockopt(SO_ERROR) falhou: " + describe(last_error());
                    } else {
                        connected = so_error == 0;
                        last_failure = connected ? last_failure : describe(so_error);
                    }
                } else if (in_progress && ready < 0) {
                    last_failure = "poll falhou ao conectar: " + describe(last_error());
                } else {
                    last_failure = in_progress ? "tempo esgotado ao conectar" : describe(e);
                }
            }
        }
        if (connected && set_non_blocking(s, false)) {
            tune(s);
            freeaddrinfo(found);
            return TcpSocket(static_cast<std::intptr_t>(s));
        }
        close_native(s);
    }
    freeaddrinfo(found);
    error = last_failure;
    return std::nullopt;
}

bool TcpSocket::set_io_timeout(std::chrono::milliseconds timeout) noexcept
{
    if (!valid() || timeout.count() <= 0) {
        return false;
    }
    const bool receive_ok = apply_timeout(to_native(fd_), SO_RCVTIMEO, timeout);
    const bool send_ok = apply_timeout(to_native(fd_), SO_SNDTIMEO, timeout);
    return receive_ok && send_ok;
}

bool TcpSocket::readable_now() const noexcept
{
    if (!valid()) {
        return true;
    }
    // Qualquer resultado diferente de "nada pronto" (dados, EOF ou erro) torna a conexão suspeita.
    return wait_ready(to_native(fd_), true, std::chrono::milliseconds(0)) != 0;
}

IoStatus TcpSocket::send_all(const void* data, std::size_t size) noexcept
{
    const char* bytes = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const auto chunk = static_cast<int>(std::min<std::size_t>(size - sent, 1U << 20));
        const auto n = ::send(to_native(fd_), bytes + sent, chunk, flags);
        if (n < 0) {
            const int error = last_error();
#ifndef _WIN32
            if (error == EINTR) {
                continue; // interrompido por sinal: repete
            }
#endif
            return is_timeout_error(error) ? IoStatus::Timeout : IoStatus::Error;
        }
        sent += static_cast<std::size_t>(n);
    }
    return IoStatus::Ok;
}

RecvResult TcpSocket::recv_some(void* buffer, std::size_t capacity) noexcept
{
    const auto chunk = static_cast<int>(std::min<std::size_t>(capacity, 1U << 20));
    auto n = ::recv(to_native(fd_), static_cast<char*>(buffer), chunk, 0);
#ifndef _WIN32
    while (n < 0 && errno == EINTR) {
        n = ::recv(to_native(fd_), static_cast<char*>(buffer), chunk, 0);
    }
#endif
    if (n > 0) {
        return {IoStatus::Ok, static_cast<std::size_t>(n)};
    }
    if (n == 0) {
        return {IoStatus::Closed, 0};
    }
    return {is_timeout_error(last_error()) ? IoStatus::Timeout : IoStatus::Error, 0};
}

void TcpSocket::shutdown_both() noexcept
{
    if (valid()) {
#ifdef _WIN32
        ::shutdown(to_native(fd_), SD_BOTH);
#else
        ::shutdown(to_native(fd_), SHUT_RDWR);
#endif
    }
}

void TcpSocket::close() noexcept
{
    if (valid()) {
        close_native(to_native(fd_));
        fd_ = kInvalid;
    }
}

// ------------------------------------------------------------------------------------------

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), port_(std::exchange(other.port_, 0))
{
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        port_ = std::exchange(other.port_, 0);
    }
    return *this;
}

std::optional<TcpListener> TcpListener::listen_loopback(std::uint16_t port, std::string& error)
{
    if (!ensure_network_started()) {
        error = "Winsock nao iniciou";
        return std::nullopt;
    }
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) {
        error = describe(last_error());
        return std::nullopt;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(s, 64) != 0) {
        error = describe(last_error());
        close_native(s);
        return std::nullopt;
    }
#ifdef _WIN32
    int len = sizeof(address);
#else
    socklen_t len = sizeof(address);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
        error = describe(last_error());
        close_native(s);
        return std::nullopt;
    }
    TcpListener listener;
    listener.fd_ = static_cast<std::intptr_t>(s);
    listener.port_ = ntohs(address.sin_port);
    return listener;
}

std::optional<TcpSocket> TcpListener::accept(std::chrono::milliseconds timeout) noexcept
{
    if (fd_ < 0) {
        return std::nullopt;
    }
    const NativeSocket listener = to_native(fd_);
    if (wait_ready(listener, true, timeout) <= 0) {
        return std::nullopt;
    }
    const NativeSocket client = ::accept(listener, nullptr, nullptr);
    if (client == kBad) {
        return std::nullopt;
    }
    tune(client);
    return TcpSocket(static_cast<std::intptr_t>(client));
}

void TcpListener::close() noexcept
{
    if (fd_ >= 0) {
        close_native(to_native(fd_));
        fd_ = -1;
    }
}

} // namespace swarmstream::net
