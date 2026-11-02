// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Processos filhos para os testes da posse da pasta de cache (F1-69): iniciar um executável
// auxiliar, esperar por ele e MATÁ-LO À FORÇA (TerminateProcess / SIGKILL), que é a queda que o
// SO precisa tratar sozinho (soltar o lock). Só para testes.
#pragma once

#include "check.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace swarmstream::test {

// Como o filho terminou.
struct ChildExit
{
    bool exited = false;   // false: ainda vivo (ou o tempo esgotou)
    bool by_kill = false;  // morto à força (sinal SIGKILL no POSIX; no Windows não distingue)
    int code = -1;         // código de saída (ou o número do sinal no POSIX)
};

// Código de saída com que o filho se mata no Windows (TerminateProcess), para o pai reconhecer.
inline constexpr int kHardKillExitCode = 77;

// Mata o PRÓPRIO processo à força, sem destrutores nem atexit (usado pelo filho para simular
// uma queda num ponto exato).
[[noreturn]] inline void hard_kill_self()
{
    std::fflush(nullptr);
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(kHardKillExitCode));
    std::_Exit(kHardKillExitCode); // não deve chegar aqui
#else
    ::kill(::getpid(), SIGKILL);
    std::_Exit(137); // não deve chegar aqui
#endif
}

class Child
{
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& o) noexcept { *this = std::move(o); }
    Child& operator=(Child&& o) noexcept
    {
        if (this != &o) {
            kill_and_reap();
#if defined(_WIN32)
            h_ = o.h_;
            o.h_ = nullptr;
#else
            pid_ = o.pid_;
            o.pid_ = -1;
#endif
            reaped_ = o.reaped_;
            exit_ = o.exit_;
        }
        return *this;
    }
    ~Child() { kill_and_reap(); }

    // `inherit_handles`: só o controle positivo da herança do lock usa true (Windows); no POSIX
    // o filho herda o que não for O_CLOEXEC de qualquer jeito.
    static std::optional<Child> spawn(const std::filesystem::path& exe,
                                      const std::vector<std::string>& args,
                                      bool inherit_handles = false)
    {
        Child c;
#if defined(_WIN32)
        std::wstring cmd = quote(exe.wstring());
        for (const std::string& a : args) cmd += L" " + quote(std::filesystem::path(a).wstring());
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, inherit_handles ? TRUE : FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return std::nullopt;
        ::CloseHandle(pi.hThread);
        c.h_ = pi.hProcess;
#else
        (void)inherit_handles;
        std::vector<std::string> storage;
        storage.push_back(exe.string());
        for (const std::string& a : args) storage.push_back(a);
        std::vector<char*> argv;
        for (std::string& s : storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        pid_t pid = -1;
        if (::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
            return std::nullopt;
        c.pid_ = pid;
#endif
        return c;
    }

    // Espera o filho terminar por `timeout`. exited == false se o tempo esgotou.
    ChildExit wait(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (poll()) return exit_;
            if (std::chrono::steady_clock::now() >= deadline) return ChildExit{};
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Morte à força (TerminateProcess / SIGKILL) e espera o SO confirmar.
    ChildExit kill_hard()
    {
        if (!reaped_) {
#if defined(_WIN32)
            SW_CHECK(::TerminateProcess(h_, static_cast<UINT>(kHardKillExitCode)) != 0);
#else
            SW_CHECK(::kill(pid_, SIGKILL) == 0);
#endif
        }
        const ChildExit e = wait(std::chrono::seconds(30));
        SW_CHECK(e.exited);
        return e;
    }

private:
    bool poll()
    {
        if (reaped_) return true;
#if defined(_WIN32)
        const DWORD r = ::WaitForSingleObject(h_, 0);
        if (r == WAIT_TIMEOUT) return false;
        SW_CHECK(r == WAIT_OBJECT_0);
        DWORD code = 0;
        SW_CHECK(::GetExitCodeProcess(h_, &code) != 0);
        exit_.exited = true;
        exit_.code = static_cast<int>(code);
        exit_.by_kill = exit_.code == kHardKillExitCode;
        ::CloseHandle(h_);
        h_ = nullptr;
#else
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == 0) return false;
        SW_CHECK(r == pid_);
        exit_.exited = true;
        if (WIFSIGNALED(status)) {
            exit_.code = WTERMSIG(status);
            exit_.by_kill = exit_.code == SIGKILL;
        } else {
            exit_.code = WEXITSTATUS(status);
        }
        pid_ = -1;
#endif
        reaped_ = true;
        return true;
    }

    void kill_and_reap()
    {
        if (reaped_) return;
#if defined(_WIN32)
        if (h_ == nullptr) return;
        ::TerminateProcess(h_, static_cast<UINT>(kHardKillExitCode));
#else
        if (pid_ < 0) return;
        ::kill(pid_, SIGKILL);
#endif
        wait(std::chrono::seconds(30));
    }

#if defined(_WIN32)
    static std::wstring quote(const std::wstring& s)
    {
        return L"\"" + s + L"\"";
    }
    HANDLE h_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
    bool reaped_ = false;
    ChildExit exit_{};
};

// Espera `path` existir (o filho a cria quando chega ao ponto combinado). false: tempo esgotado.
inline bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace swarmstream::test
