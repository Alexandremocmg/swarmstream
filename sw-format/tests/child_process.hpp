// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Processo-filho para a varredura de falta de memória do `sw-format` (F1-73): roda um executável
// com a saída (stdout e stderr) num arquivo temporário, espera com limite de tempo e devolve como
// ele terminou. O filho é necessário porque um std::terminate mata o processo: com um filho por
// ponto K, o pai conta TODOS os pontos em vez de parar no primeiro. Só para testes.
//
// Escrito para este cartão (não é cópia do `test_process.hpp` do PR #37, que serve aos testes de
// posse de pasta e não redireciona a saída).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char**
    environ; // NOLINT(readability-redundant-declaration): POSIX não o declara em cabeçalho
#endif

namespace swarmstream::test {

struct ChildResult
{
    bool started = false;   // false: o processo nem começou (executável ausente, etc.)
    bool timed_out = false; // o pai matou o filho depois do limite
    bool signaled = false;  // POSIX: terminou por sinal (`code` é o número do sinal)
    int code = -1;          // código de saída
    double seconds = 0.0;
    std::string output; // stdout + stderr
};

// Caminho deste executável (para ele se relançar como filho).
inline std::filesystem::path self_exe(const char* argv0)
{
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n > 0 && n < buffer.size()) {
        buffer.resize(n);
        return std::filesystem::path(buffer);
    }
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) return self;
#endif
    std::error_code ec2;
    const std::filesystem::path abs = std::filesystem::absolute(argv0, ec2);
    return ec2 ? std::filesystem::path(argv0) : abs;
}

namespace detail {

inline std::filesystem::path child_log_path()
{
    static std::uint64_t counter = 0;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long long>(::getpid());
#endif
    return dir /
           ("sw-format-oom-" + std::to_string(pid) + "-" + std::to_string(++counter) + ".log");
}

inline std::string read_and_remove(const std::filesystem::path& path)
{
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return text;
}

} // namespace detail

// `args` são fichas simples (sem espaços nem aspas): é o que a varredura usa.
inline ChildResult run_child(const std::filesystem::path& exe, const std::vector<std::string>& args,
                             std::chrono::milliseconds timeout)
{
    ChildResult result;
    const std::filesystem::path log_path = detail::child_log_path();
    const auto t0 = std::chrono::steady_clock::now();
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    const HANDLE log =
        ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return result;
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const std::string& a : args)
        cmd += L" \"" + std::wstring(a.begin(), a.end()) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(log);
    if (!created) {
        (void)detail::read_and_remove(log_path);
        return result;
    }
    result.started = true;
    ::CloseHandle(pi.hThread);
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
    if (waited != WAIT_OBJECT_0) {
        result.timed_out = true;
        ::TerminateProcess(pi.hProcess, 99);
        ::WaitForSingleObject(pi.hProcess, 30000);
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(pi.hProcess, &code) != 0) result.code = static_cast<int>(code);
    ::CloseHandle(pi.hProcess);
#else
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return result;
    const std::string log_name = log_path.string();
    const std::string exe_name = exe.string();
    bool ok = ::posix_spawn_file_actions_addopen(&actions, 1, log_name.c_str(),
                                                 O_WRONLY | O_CREAT | O_TRUNC, 0600) == 0;
    ok = ok && ::posix_spawn_file_actions_adddup2(&actions, 1, 2) == 0;
    std::vector<std::string> storage;
    storage.push_back(exe_name);
    for (const std::string& a : args)
        storage.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const bool spawned =
        ok && ::posix_spawn(&pid, exe_name.c_str(), &actions, nullptr, argv.data(), environ) == 0;
    ::posix_spawn_file_actions_destroy(&actions);
    if (!spawned) {
        (void)detail::read_and_remove(log_path);
        return result;
    }
    result.started = true;
    const auto deadline = t0 + timeout;
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { // não deveria acontecer: trata como filho que não terminou direito
            result.timed_out = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!result.timed_out) {
        if (WIFSIGNALED(status)) {
            result.signaled = true;
            result.code = WTERMSIG(status);
        } else if (WIFEXITED(status)) {
            result.code = WEXITSTATUS(status);
        }
    }
#endif
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    result.output = detail::read_and_remove(log_path);
    return result;
}

} // namespace swarmstream::test
