// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// F1-67 parte B, F1-60 §1.5 teste 4: esgotamento do contador GLOBAL de sw_request_id. Executável
// próprio, UM PROCESSO POR CASO (o ctest passa o caso no argumento), porque o gancho mexe no
// contador do processo. O gancho (`testing::set_last_issued_request_id`) é interno e não exportado.
//
//   raw         o contador emite UINT64_MAX-2 e UINT64_MAX-1 e depois só 0, para sempre; o aviso de
//               "primeira vez" vem uma vez só.
//   submit      no registro: 2 pedidos aceitos (UINT64_MAX-2, UINT64_MAX-1); o seguinte falha com
//               IdsExhausted (o sw_*: SW_E_INTERNAL), *out_id == 0, sem callback, e nenhuma chamada
//               seguinte emite 0 nem repete; os 2 pendentes concluem normalmente.
//   concurrent  16 threads disputam os últimos 999 ids: todos emitidos uma vez, nenhum 0, nenhum
//               repetido, nenhum depois do fim.

#include "api_allocator.hpp"
#include "check.hpp"
#include "request_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using swarmstream::detail::ContextAllocator;
using swarmstream::detail::FileRequests;
using swarmstream::detail::issue_request_id;
using swarmstream::detail::RequestKind;
using swarmstream::detail::RequestRegistry;
using swarmstream::detail::RequestRegistryConfig;
using swarmstream::detail::RequestStatus;
using swarmstream::detail::SubmitArgs;
using swarmstream::detail::SubmitResult;
namespace hook = swarmstream::detail::testing;

constexpr std::uint64_t kMax = UINT64_MAX;

int case_raw()
{
    hook::set_last_issued_request_id(kMax - 3);
    bool first = true;
    SW_CHECK(issue_request_id(&first) == kMax - 2 && !first);
    SW_CHECK(issue_request_id(&first) == kMax - 1 && !first);
    SW_CHECK(issue_request_id(&first) == 0 && first); // esgotou: a primeira vez é avisada
    for (int i = 0; i < 1000; ++i) {
        SW_CHECK(issue_request_id(&first) == 0 && !first); // só uma vez
    }
    SW_CHECK(issue_request_id() == 0);
    std::puts("ok: raw: UINT64_MAX-2, UINT64_MAX-1 e depois 0 para sempre; aviso uma vez");
    return 0;
}

std::atomic<int> g_callbacks{0};
std::mutex g_m;
std::vector<sw_request_id> g_cb_ids;

void SW_CALL on_done(void*, sw_request_id id, sw_result result, std::uint64_t)
{
    SW_CHECK(result == SW_OK);
    std::lock_guard<std::mutex> g(g_m);
    g_cb_ids.push_back(id);
    g_callbacks.fetch_add(1);
}

int case_submit()
{
    g_cb_ids.reserve(16);
    const ContextAllocator ca;
    int owner = 1;
    RequestRegistryConfig cfg;
    cfg.capacity = 8;
    cfg.completion_threads = 1;
    cfg.owner = &owner;
    auto c = RequestRegistry::create(ca, cfg);
    SW_CHECK(c.status == RequestStatus::Ok);
    RequestRegistry* reg = c.registry;
    FileRequests file;
    SubmitArgs a;
    a.file = &file;
    a.kind = RequestKind::Synthetic;
    a.cb = &on_done;

    hook::set_last_issued_request_id(kMax - 3);
    sw_request_id id1 = 0;
    sw_request_id id2 = 0;
    SW_CHECK(reg->submit(a, &id1).status == RequestStatus::Ok && id1 == kMax - 2);
    SW_CHECK(reg->submit(a, &id2).status == RequestStatus::Ok && id2 == kMax - 1);
    sw_request_id id3 = 123;
    SubmitResult s = reg->submit(a, &id3);
    SW_CHECK(s.status == RequestStatus::IdsExhausted && id3 == 0 && s.first_exhaustion);
    for (int i = 0; i < 10; ++i) {
        sw_request_id again = 5;
        s = reg->submit(a, &again);
        SW_CHECK(s.status == RequestStatus::IdsExhausted && again == 0 && !s.first_exhaustion);
    }
    // A falha não ocupou o pool: os dois pendentes seguem e concluem normalmente.
    SW_CHECK(reg->pending() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SW_CHECK(g_callbacks.load() == 0); // nenhum callback para os recusados
    SW_CHECK(reg->complete(id1, SW_OK, 1) == RequestStatus::Ok);
    SW_CHECK(reg->complete(id2, SW_OK, 2) == RequestStatus::Ok);
    SW_CHECK(reg->close_file(file) == RequestStatus::Ok);
    SW_CHECK(g_callbacks.load() == 2);
    {
        std::lock_guard<std::mutex> g(g_m);
        std::sort(g_cb_ids.begin(), g_cb_ids.end());
        SW_CHECK(g_cb_ids.size() == 2 && g_cb_ids[0] == kMax - 2 && g_cb_ids[1] == kMax - 1);
    }
    SW_CHECK(RequestRegistry::destroy(reg) == RequestStatus::Ok);
    std::puts("ok: submit: 2 aceitos, o seguinte IdsExhausted com out_id 0 e sem callback");
    return 0;
}

int case_concurrent()
{
    constexpr std::uint64_t kLeft = 999;
    hook::set_last_issued_request_id(kMax - 1 - kLeft);
    constexpr int kThreads = 16;
    std::vector<std::vector<sw_request_id>> got(kThreads);
    for (auto& v : got) v.reserve(kLeft);
    std::atomic<int> firsts{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (;;) {
                bool first = false;
                const sw_request_id id = issue_request_id(&first);
                if (id == 0) {
                    if (first) firsts.fetch_add(1);
                    // Depois do fim, nunca mais um id.
                    for (int i = 0; i < 100; ++i) SW_CHECK(issue_request_id() == 0);
                    return;
                }
                got[t].push_back(id);
            }
        });
    }
    for (auto& th : threads) th.join();
    std::vector<sw_request_id> all;
    for (const auto& v : got) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    SW_CHECK(all.size() == kLeft);
    SW_CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    SW_CHECK(all.front() == kMax - kLeft && all.back() == kMax - 1);
    SW_CHECK(firsts.load() == 1);
    std::printf("ok: concurrent: 16 threads, %zu ids finais, todos uma vez, nenhum 0\n", all.size());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "uso: %s raw|submit|concurrent\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "raw") == 0) return case_raw();
    if (std::strcmp(argv[1], "submit") == 0) return case_submit();
    if (std::strcmp(argv[1], "concurrent") == 0) return case_concurrent();
    std::fprintf(stderr, "caso desconhecido: %s\n", argv[1]);
    return 2;
}
