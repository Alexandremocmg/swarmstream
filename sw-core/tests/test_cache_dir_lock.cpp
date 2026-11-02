// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-69: lock exclusivo da pasta de cache entre processos (F1-60 §6.1, ADR-0011).
//
// Uso: sw-core-lock-tests <caminho do posse-tool>. O executável auxiliar roda como PROCESSO
// FILHO (segura o lock, é morto à força, é iniciado com exec depois de o pai pegar o lock).
//
// Cobre: segundo lock no mesmo processo; processo filho segura e o pai recebe "bloqueado" SEM
// alterar a pasta (instantâneo de nomes, tamanhos e hash); filho morto à força libera o lock;
// arquivo de lock pré-existente sem dono não impede; o arquivo sobrevive ao unlock e à falha;
// processo iniciado com exec não herda o lock (com controle positivo: um lock HERDÁVEL de
// propósito é herdado, então o teste sabe detectar a herança); pasta que não é pasta, lock que
// é pasta e somente leitura => erro de E/S (não "bloqueado"); criação da pasta; 16 threads
// disputando (o job TSan roda este teste).
#include "cache_dir_lock.hpp"
#include "cache_inspect.hpp"
#include "check.hpp"
#include "swarmstream/cache_owner.hpp"
#include "test_process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sw = swarmstream;
namespace det = swarmstream::detail;
namespace fs = std::filesystem;
using std::chrono::milliseconds;

namespace {

fs::path g_tool;

fs::path fresh_dir(const char* name)
{
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("swarmstream_f1_69l_") + name + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    SW_CHECK(!ec);
    return dir;
}

void cleanup(const fs::path& dir)
{
    // Devolve a escrita antes de apagar (alguns testes tiram a permissão).
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code pec;
        fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, pec);
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    ec.clear();
    fs::remove_all(dir, ec);
    SW_CHECK(!ec);
}

void write_file(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    f.flush();
    SW_CHECK(static_cast<bool>(f));
}

bool running_as_root()
{
#if defined(_WIN32)
    return false;
#else
    return ::geteuid() == 0;
#endif
}

// Instantâneo da pasta: caminho relativo -> (tamanho, FNV-1a 64 do conteúdo). Pastas entram
// com tamanho 0 e hash 0 (a existência de `installed/` conta).
using Snapshot = std::map<std::string, std::pair<std::uintmax_t, std::uint64_t>>;

Snapshot snapshot(const fs::path& dir, bool skip_lock_file)
{
    Snapshot snap;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        const std::string rel = fs::relative(e.path(), dir).generic_string();
        if (skip_lock_file && rel == det::kCacheLockFileName) continue;
        if (e.is_directory()) {
            snap[rel] = {0, 0};
            continue;
        }
        std::ifstream f(e.path(), std::ios::binary);
        SW_CHECK(static_cast<bool>(f));
        std::uint64_t h = 1469598103934665603ULL;
        std::uintmax_t n = 0;
        char c = 0;
        while (f.get(c)) {
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
            ++n;
        }
        snap[rel] = {n, h};
    }
    SW_CHECK(!ec);
    return snap;
}

void populate_cache_like(const fs::path& dir)
{
    write_file(dir / "index.bin", "indice de mentira");
    write_file(dir / "journal.bin", "journal de mentira");
    write_file(dir / "store-000.bin", std::string(4096, 'x'));
    fs::create_directories(dir / "installed");
    write_file(dir / "installed" / "abc.manifest", "copia de mentira");
}

// Espera o pai conseguir o lock depois da morte do filho. O SO solta o lock quando termina de
// destruir o processo, que pode ser um instante depois de o wait() ver a saída.
bool acquire_eventually(const fs::path& dir, det::CacheDirLock& lock)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const det::LockStatus st = det::CacheDirLock::acquire(dir, lock);
        if (st.ok()) return true;
        SW_CHECK(st.code == det::LockCode::Locked);
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(milliseconds(20));
    }
}

// ---------------------------------------------------------------------------------------------

void test_same_process_second_lock_collides()
{
    std::cout << "[Test] segundo lock no mesmo processo colide\n";
    const fs::path dir = fresh_dir("mesmo");
    det::CacheDirLock a;
    const det::LockStatus sa = det::CacheDirLock::acquire(dir, a);
    SW_CHECK(sa.ok() && a.held());
    SW_CHECK(a.path() == dir / det::kCacheLockFileName);

    det::CacheDirLock b;
    const det::LockStatus sb = det::CacheDirLock::acquire(dir, b);
    SW_CHECK(sb.code == det::LockCode::Locked && !b.held());
    SW_CHECK(!sb.detail.empty() && sb.detail.find("swarmstream.lock") != std::string::npos);
    SW_CHECK(a.held()); // o segundo não mexeu no primeiro

    SW_CHECK(a.release().ok() && !a.held());
    SW_CHECK(det::CacheDirLock::acquire(dir, b).ok() && b.held());
    SW_CHECK(b.release().ok());
    SW_CHECK(b.release().ok()); // soltar sem lock é Ok
    cleanup(dir);
}

void test_two_folders_independent()
{
    std::cout << "[Test] pastas diferentes não colidem\n";
    const fs::path d1 = fresh_dir("p1");
    const fs::path d2 = fresh_dir("p2");
    det::CacheDirLock a, b;
    SW_CHECK(det::CacheDirLock::acquire(d1, a).ok());
    SW_CHECK(det::CacheDirLock::acquire(d2, b).ok());
    SW_CHECK(a.release().ok() && b.release().ok());
    cleanup(d1);
    cleanup(d2);
}

void test_creates_folder()
{
    std::cout << "[Test] cria a pasta (e as intermediárias) se faltar\n";
    const fs::path base = fresh_dir("cria");
    const fs::path dir = base / "a" / "b" / "cache";
    SW_CHECK(!fs::exists(dir));
    det::CacheDirLock lock;
    SW_CHECK(det::CacheDirLock::acquire(dir, lock).ok());
    SW_CHECK(fs::is_directory(dir) && fs::is_regular_file(dir / det::kCacheLockFileName));
    SW_CHECK(fs::file_size(dir / det::kCacheLockFileName) == 0); // vazio, sem PID
    SW_CHECK(lock.release().ok());
    cleanup(base);
}

void test_preexisting_lock_file_without_owner()
{
    std::cout << "[Test] arquivo de lock pré-existente sem dono não impede\n";
    const fs::path dir = fresh_dir("orfao");
    write_file(dir / det::kCacheLockFileName, "");
    det::CacheDirLock a;
    SW_CHECK(det::CacheDirLock::acquire(dir, a).ok());
    SW_CHECK(a.release().ok());
    // Mesmo com lixo dentro (um "PID" de uma versão antiga, por exemplo): existência não é posse.
    write_file(dir / det::kCacheLockFileName, "12345\n");
    det::CacheDirLock b;
    SW_CHECK(det::CacheDirLock::acquire(dir, b).ok());
    SW_CHECK(b.release().ok());
    // O conteúdo não foi tocado (não é truncado nem reescrito).
    {
        std::ifstream f(dir / det::kCacheLockFileName, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        SW_CHECK(content == "12345\n");
    } // fecha antes de apagar a pasta (o Windows não apaga arquivo aberto)
    cleanup(dir);
}

void test_lock_file_survives_unlock_and_failure()
{
    std::cout << "[Test] o arquivo de lock sobrevive ao unlock, à falha e ao destrutor\n";
    const fs::path dir = fresh_dir("sobrevive");
    const fs::path lock_file = dir / det::kCacheLockFileName;
    {
        det::CacheDirLock a;
        SW_CHECK(det::CacheDirLock::acquire(dir, a).ok());
        det::CacheDirLock b;
        SW_CHECK(det::CacheDirLock::acquire(dir, b).code == det::LockCode::Locked);
        SW_CHECK(fs::exists(lock_file)); // falha de B não o apagou
        SW_CHECK(a.release().ok());
        SW_CHECK(fs::exists(lock_file)); // unlock não o apagou
        det::CacheDirLock c;
        SW_CHECK(det::CacheDirLock::acquire(dir, c).ok());
    } // destrutor de c solta o lock
    SW_CHECK(fs::exists(lock_file));
    det::CacheDirLock d;
    SW_CHECK(det::CacheDirLock::acquire(dir, d).ok()); // o destrutor soltou de verdade
    SW_CHECK(d.release().ok());
    cleanup(dir);
}

void test_misuse_and_move()
{
    std::cout << "[Test] uso incorreto (acquire em objeto que segura) e mover o lock\n";
    const fs::path dir = fresh_dir("move");
    const fs::path other = fresh_dir("move2");
    det::CacheDirLock a;
    SW_CHECK(det::CacheDirLock::acquire(dir, a).ok());
    // Pedir outro lock com o mesmo objeto: erro, e o lock original continua seguro.
    const det::LockStatus again = det::CacheDirLock::acquire(other, a);
    SW_CHECK(again.code == det::LockCode::Io && a.held() && a.path() == dir / det::kCacheLockFileName);
    SW_CHECK(!fs::exists(other / det::kCacheLockFileName)); // nem tocou na outra pasta

    det::CacheDirLock moved(std::move(a));
    SW_CHECK(!a.held() && moved.held());
    det::CacheDirLock probe;
    SW_CHECK(det::CacheDirLock::acquire(dir, probe).code == det::LockCode::Locked); // ainda preso

    det::CacheDirLock assigned;
    assigned = std::move(moved);
    SW_CHECK(!moved.held() && assigned.held());
    SW_CHECK(det::CacheDirLock::acquire(dir, probe).code == det::LockCode::Locked);
    SW_CHECK(assigned.release().ok());
    SW_CHECK(det::CacheDirLock::acquire(dir, probe).ok());
    SW_CHECK(probe.release().ok());
    cleanup(dir);
    cleanup(other);
}

void test_io_errors_are_not_locked()
{
    std::cout << "[Test] erro de E/S não vira 'bloqueado'\n";
    // 1) O caminho da pasta é um arquivo comum.
    {
        const fs::path base = fresh_dir("io1");
        write_file(base / "arquivo", "x");
        det::CacheDirLock l;
        const det::LockStatus st = det::CacheDirLock::acquire(base / "arquivo", l);
        SW_CHECK(st.code == det::LockCode::Io && !l.held() && !st.detail.empty());
        cleanup(base);
    }
    // 2) swarmstream.lock é uma pasta.
    {
        const fs::path base = fresh_dir("io2");
        fs::create_directories(base / det::kCacheLockFileName);
        det::CacheDirLock l;
        const det::LockStatus st = det::CacheDirLock::acquire(base, l);
        SW_CHECK(st.code == det::LockCode::Io && !l.held());
        SW_CHECK(fs::is_directory(base / det::kCacheLockFileName)); // não o apagou
        cleanup(base);
    }
    // 3) Arquivo de lock somente leitura (Windows: atributo; POSIX: 0444, que o root ignora).
    {
        const fs::path base = fresh_dir("io3");
        write_file(base / det::kCacheLockFileName, "");
        std::error_code ec;
        fs::permissions(base / det::kCacheLockFileName,
                        fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove, ec);
        SW_CHECK(!ec);
        if (running_as_root()) {
            std::cout << "  NAO COBERTO (lock somente leitura): rodando como root, a permissao "
                         "nao vale; o caso fica sem prova neste ambiente\n";
        } else {
            det::CacheDirLock l;
            const det::LockStatus st = det::CacheDirLock::acquire(base, l);
            SW_CHECK(st.code == det::LockCode::Io && !l.held());
        }
        cleanup(base);
    }
#if !defined(_WIN32)
    // 4) Pasta somente leitura e sem o arquivo de lock (POSIX; no Windows o atributo de pasta
    // somente leitura não impede criar arquivos dentro dela).
    {
        const fs::path base = fresh_dir("io4");
        std::error_code ec;
        fs::permissions(base, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, ec);
        SW_CHECK(!ec);
        if (running_as_root()) {
            std::cout << "  NAO COBERTO (pasta somente leitura): rodando como root, a permissao "
                         "nao vale; o caso fica sem prova neste ambiente\n";
        } else {
            det::CacheDirLock l;
            const det::LockStatus st = det::CacheDirLock::acquire(base, l);
            SW_CHECK(st.code == det::LockCode::Io && !l.held());
            SW_CHECK(!fs::exists(base / det::kCacheLockFileName));
        }
        cleanup(base);
    }
#endif
}

void test_child_holds_lock_parent_gets_locked_without_changing_folder()
{
    std::cout << "[Test] processo filho segura; o pai recebe 'bloqueado' sem alterar a pasta\n";
    const fs::path dir = fresh_dir("filho");
    populate_cache_like(dir);
    const fs::path scratch = fresh_dir("filho_aux"); // o marcador fica FORA da pasta de cache
    const fs::path ready = scratch / "pronto";

    auto child = sw::test::Child::spawn(g_tool, {"hold", dir.string(), ready.string()});
    SW_CHECK(child.has_value());
    SW_CHECK(sw::test::wait_for_file(ready, std::chrono::seconds(30)));
    {
        std::ifstream f(ready);
        std::string word;
        f >> word;
        SW_CHECK(word == "ACQUIRED");
    }

    // Instantâneo com o filho já dono (o arquivo de lock existe).
    const Snapshot before = snapshot(dir, /*skip_lock_file=*/true);
    SW_CHECK(before.size() == 5 + 0); // index, journal, store, installed/, installed/abc.manifest
    det::CacheDirLock mine;
    const det::LockStatus st = det::CacheDirLock::acquire(dir, mine);
    SW_CHECK(st.code == det::LockCode::Locked && !mine.held());
    const Snapshot after = snapshot(dir, true);
    SW_CHECK(before == after); // nomes, tamanhos e hash idênticos, exceto o arquivo de lock

    // Outro processo (a ferramenta `try`) também recebe 'bloqueado'.
    auto tryer = sw::test::Child::spawn(g_tool, {"try", dir.string()});
    SW_CHECK(tryer.has_value());
    const sw::test::ChildExit te = tryer->wait(std::chrono::seconds(30));
    SW_CHECK(te.exited && te.code == 10);

    // Controle positivo do critério de pasta intacta: o instantâneo SABE ver uma mudança.
    write_file(dir / "intruso.bin", "x");
    SW_CHECK(snapshot(dir, true) != before);
    fs::remove(dir / "intruso.bin");

    // Filho morto à força => o SO solta o lock => o pai consegue.
    const sw::test::ChildExit ke = child->kill_hard();
    SW_CHECK(ke.exited && ke.by_kill);
    SW_CHECK(acquire_eventually(dir, mine));
    SW_CHECK(mine.release().ok());
    SW_CHECK(fs::exists(dir / det::kCacheLockFileName)); // o arquivo do filho morto ficou
    cleanup(dir);
    cleanup(scratch);
}

void test_child_killed_releases_when_parent_never_held()
{
    std::cout << "[Test] filho morto à força libera o lock para outro processo\n";
    const fs::path dir = fresh_dir("morte");
    const fs::path scratch = fresh_dir("morte_aux");
    const fs::path ready = scratch / "pronto";
    auto child = sw::test::Child::spawn(g_tool, {"hold", dir.string(), ready.string()});
    SW_CHECK(child.has_value());
    SW_CHECK(sw::test::wait_for_file(ready, std::chrono::seconds(30)));
    SW_CHECK(child->kill_hard().by_kill);

    // Um segundo filho (outro processo) pega o lock depois da morte do primeiro.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int code = -1;
    for (;;) {
        auto tryer = sw::test::Child::spawn(g_tool, {"try", dir.string()});
        SW_CHECK(tryer.has_value());
        const sw::test::ChildExit e = tryer->wait(std::chrono::seconds(30));
        SW_CHECK(e.exited);
        code = e.code;
        if (code == 0 || std::chrono::steady_clock::now() >= deadline) break;
        SW_CHECK(code == 10);
        std::this_thread::sleep_for(milliseconds(20));
    }
    SW_CHECK(code == 0);
    cleanup(dir);
    cleanup(scratch);
}

// Lock HERDÁVEL de propósito (controle positivo da detecção de herança): sem O_CLOEXEC no POSIX,
// com handle herdável no Windows.
struct RawInheritableLock
{
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool take(const fs::path& file)
    {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        h = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov{};
        return ::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0;
#else
        fd = ::open(file.c_str(), O_RDWR | O_CREAT, 0644); // sem O_CLOEXEC
        if (fd < 0) return false;
        return ::flock(fd, LOCK_EX | LOCK_NB) == 0;
#endif
    }
    void drop_parent_copy()
    {
#if defined(_WIN32)
        SW_CHECK(::CloseHandle(h) != 0);
        h = INVALID_HANDLE_VALUE;
#else
        SW_CHECK(::close(fd) == 0); // fecha só o descritor do pai; o filho tem o dele
        fd = -1;
#endif
    }
};

void test_exec_child_does_not_inherit_lock()
{
    std::cout << "[Test] processo iniciado com exec não herda o lock (com controle positivo)\n";
    const fs::path dir = fresh_dir("exec");
    const fs::path scratch = fresh_dir("exec_aux");

    // (a) O lock de verdade: o pai segura, inicia um filho que dorme, SOLTA o lock; outro
    // processo consegue o lock enquanto o filho dorminhoco continua vivo.
    {
        det::CacheDirLock mine;
        SW_CHECK(det::CacheDirLock::acquire(dir, mine).ok());
        const fs::path ready = scratch / "pronto_a";
        // inherit_handles=true no Windows: se o handle fosse herdável o filho o levaria.
        auto sleeper = sw::test::Child::spawn(g_tool, {"sleep", ready.string()}, true);
        SW_CHECK(sleeper.has_value());
        SW_CHECK(sw::test::wait_for_file(ready, std::chrono::seconds(30)));
        SW_CHECK(mine.release().ok());
#if defined(_WIN32)
        {
            // No Windows o lock é do processo que o pegou e some com o CloseHandle dele, herança ou
            // não. O que a herança deixaria para trás é o ARQUIVO aberto no filho (a abertura não
            // compartilha DELETE): se o filho não herdou o handle, o arquivo pode ser apagado.
            std::error_code rm;
            fs::remove(dir / det::kCacheLockFileName, rm);
            SW_CHECK(!rm);
        }
#endif
        auto tryer = sw::test::Child::spawn(g_tool, {"try", dir.string()});
        SW_CHECK(tryer.has_value());
        const sw::test::ChildExit e = tryer->wait(std::chrono::seconds(30));
        SW_CHECK(e.exited && e.code == 0); // o dorminhoco NÃO segurou o lock
        const sw::test::ChildExit still = sleeper->wait(milliseconds(0));
        SW_CHECK(!still.exited); // e continuava vivo durante a prova
        SW_CHECK(sleeper->kill_hard().by_kill);
    }

    // (b) Controle positivo: o mesmo roteiro com um lock herdável => o filho o herda e o lock
    // continua preso depois de o pai largar a sua cópia.
    {
        const fs::path raw_dir = fresh_dir("exec_raw");
        RawInheritableLock raw;
        SW_CHECK(raw.take(raw_dir / det::kCacheLockFileName));
        const fs::path ready = scratch / "pronto_b";
        auto sleeper = sw::test::Child::spawn(g_tool, {"sleep", ready.string()}, true);
        SW_CHECK(sleeper.has_value());
        SW_CHECK(sw::test::wait_for_file(ready, std::chrono::seconds(30)));
        raw.drop_parent_copy();
#if defined(_WIN32)
        {
            // Herdado: o arquivo continua aberto no filho, então NÃO pode ser apagado. É a mesma
            // sonda de (a); aqui ela tem de acusar a herança.
            std::error_code rm;
            fs::remove(raw_dir / det::kCacheLockFileName, rm);
            SW_CHECK(rm);
        }
#else
        auto tryer = sw::test::Child::spawn(g_tool, {"try", raw_dir.string()});
        SW_CHECK(tryer.has_value());
        const sw::test::ChildExit e = tryer->wait(std::chrono::seconds(30));
        SW_CHECK(e.exited && e.code == 10); // herdado: o lock continua preso no filho
#endif
        SW_CHECK(sleeper->kill_hard().by_kill);
        cleanup(raw_dir);
    }
    cleanup(dir);
    cleanup(scratch);
}

void test_sixteen_threads_one_winner()
{
    std::cout << "[Test] 16 threads disputando: exatamente uma ganha\n";
    const fs::path dir = fresh_dir("threads");
    constexpr int kThreads = 16;
    std::latch start(kThreads);
    std::latch done_trying(kThreads);
    std::atomic<int> winners{0};
    std::atomic<int> locked{0};
    std::atomic<int> other{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            det::CacheDirLock lock;
            start.arrive_and_wait();
            const det::LockStatus st = det::CacheDirLock::acquire(dir, lock);
            if (st.ok()) {
                ++winners;
            } else if (st.code == det::LockCode::Locked) {
                ++locked;
            } else {
                ++other;
            }
            done_trying.arrive_and_wait(); // o vencedor só solta depois de todos tentarem
            if (lock.held()) {
                if (!lock.release().ok()) ++other;
            }
        });
    }
    for (auto& t : threads) t.join();
    SW_CHECK(winners.load() == 1 && locked.load() == kThreads - 1 && other.load() == 0);
    cleanup(dir);
}


// swarmstream.lock e installed/ convivem com o resto da pasta (F1-60 §6.1/§6.2, "confirmar no
// teste"): o StoreFiles só apaga store-NNN.bin e a inspeção ignora os outros nomes.
void test_other_components_leave_lock_and_installed_alone()
{
    std::cout << "[Test] CacheOwner (teto menor apaga store-NNN.bin) e inspeção não mexem em swarmstream.lock nem installed/" << std::endl;
    const fs::path dir = fresh_dir("convive");
    constexpr sw::StoreLayout kLayout{.slots_per_file = 4, .slot_size = 256 * 1024};
    det::CacheDirLock lock;
    SW_CHECK(det::CacheDirLock::acquire(dir, lock).ok());
    fs::create_directories(dir / "installed");
    write_file(dir / "installed" / "abc.manifest", "copia");
    write_file(dir / "installed" / "abc.manifest.tmp", "tmp");
    {
        auto o = sw::CacheOwner::start(
            sw::CacheOwnerConfig{.dir = dir, .total_slots = 12, .layout = kLayout});
        SW_CHECK(o.has_value());
        for (std::uint32_t i = 0; i < 12; ++i) {
            sw::PieceHash h{};
            h.bytes[0] = static_cast<std::uint8_t>(i + 1);
            const std::vector<std::byte> data(3000, static_cast<std::byte>(i));
            const std::array<std::uint32_t, 3> blocks{1000, 1000, 1000};
            auto r = sw::CacheRequest::put(sw::PieceId(i, h), data, blocks);
            SW_CHECK((*o)->call(r).has_value());
        }
    }
    SW_CHECK(fs::exists(dir / "store-002.bin")); // 12 slots / 4 por arquivo = 3 arquivos
    const Snapshot mine_before = snapshot(dir, false);
    {
        // Reabre com teto de 2 slots: store-001.bin e store-002.bin são apagados.
        auto o = sw::CacheOwner::start(
            sw::CacheOwnerConfig{.dir = dir, .total_slots = 2, .layout = kLayout});
        SW_CHECK(o.has_value());
    }
    SW_CHECK(!fs::exists(dir / "store-002.bin")); // controle positivo: a poda aconteceu
    SW_CHECK(fs::exists(dir / det::kCacheLockFileName));
    SW_CHECK(fs::file_size(dir / det::kCacheLockFileName) == 0);
    SW_CHECK(fs::exists(dir / "installed" / "abc.manifest") &&
             fs::exists(dir / "installed" / "abc.manifest.tmp"));
    const Snapshot after = snapshot(dir, false);
    SW_CHECK(after.at("installed/abc.manifest") == mine_before.at("installed/abc.manifest"));
    SW_CHECK(after.at("installed/abc.manifest.tmp") == mine_before.at("installed/abc.manifest.tmp"));
    // A inspeção (somente leitura) também ignora os dois nomes e não muda a pasta.
    const Snapshot before_inspect = snapshot(dir, true);
    const auto ins = det::inspect_cache(dir, kLayout);
    SW_CHECK(ins.has_value());
    SW_CHECK(snapshot(dir, true) == before_inspect);
    SW_CHECK(lock.release().ok());
    cleanup(dir);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "uso: sw-core-lock-tests <posse-tool>\n");
        return 2;
    }
    g_tool = argv[1];
    SW_CHECK(fs::exists(g_tool));
    test_same_process_second_lock_collides();
    test_two_folders_independent();
    test_creates_folder();
    test_preexisting_lock_file_without_owner();
    test_lock_file_survives_unlock_and_failure();
    test_misuse_and_move();
    test_io_errors_are_not_locked();
    test_child_holds_lock_parent_gets_locked_without_changing_folder();
    test_child_killed_releases_when_parent_never_held();
    test_exec_child_does_not_inherit_lock();
    test_sixteen_threads_one_winner();
    test_other_components_leave_lock_and_installed_alone();
    std::cout << "TODOS OS TESTES DO LOCK PASSARAM\n";
    return 0;
}
