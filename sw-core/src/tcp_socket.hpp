// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Soquete TCP bloqueante, com timeouts, para a fonte HTTP (F1-27). Interno ao sw-core: não faz
// parte da API pública. Windows (Winsock) e POSIX atrás da mesma interface. Sem dependências.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace swarmstream::net {

enum class IoStatus {
    Ok,
    Timeout, // o prazo do soquete estourou
    Closed,  // o par fechou a conexão (EOF)
    Error,   // erro de rede (reset, broken pipe...)
};

struct RecvResult
{
    IoStatus status = IoStatus::Error;
    std::size_t bytes = 0; // só vale com status == Ok
};

class TcpSocket
{
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // Conecta a host:port. Em caso de erro devolve nullopt e descreve em `error`.
    [[nodiscard]] static std::optional<TcpSocket> connect(const std::string& host,
                                                          std::uint16_t port,
                                                          std::chrono::milliseconds timeout,
                                                          std::string& error);

    [[nodiscard]] bool valid() const noexcept { return fd_ != kInvalid; }

    // Prazo de cada send/recv (> 0). Devolve false se o sistema não aplicou o prazo: nesse caso o
    // recv poderia bloquear sem limite, então quem chama deve descartar a conexão.
    [[nodiscard]] bool set_io_timeout(std::chrono::milliseconds timeout) noexcept;

    // true se o soquete já está legível AGORA (poll com prazo 0): chegaram bytes que ninguém
    // pediu, o par fechou ou deu erro. Numa conexão ociosa isso significa "não reutilize".
    [[nodiscard]] bool readable_now() const noexcept;

    [[nodiscard]] IoStatus send_all(const void* data, std::size_t size) noexcept;
    [[nodiscard]] RecvResult recv_some(void* buffer, std::size_t capacity) noexcept;

    // Acorda quem estiver bloqueado em recv/send neste soquete (pode ser chamado de outra thread).
    void shutdown_both() noexcept;
    void close() noexcept;

private:
    friend class TcpListener;
    static constexpr std::intptr_t kInvalid = -1;
    explicit TcpSocket(std::intptr_t fd) noexcept : fd_(fd) {}

    std::intptr_t fd_ = kInvalid;
};

// Servidor de loopback, usado só pelos testes (servidor HTTP falso em processo).
class TcpListener
{
public:
    TcpListener() noexcept = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Escuta em 127.0.0.1; port = 0 escolhe uma porta livre.
    [[nodiscard]] static std::optional<TcpListener> listen_loopback(std::uint16_t port,
                                                                    std::string& error);
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Espera uma conexão até `timeout`; nullopt se estourar ou der erro.
    [[nodiscard]] std::optional<TcpSocket> accept(std::chrono::milliseconds timeout) noexcept;
    void close() noexcept;

private:
    std::intptr_t fd_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace swarmstream::net
