// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Prova que SW_CHECK funciona em qualquer tipo de build (inclusive com NDEBUG):
//   check_selftest ok       -> condição verdadeira: sai com 0
//   check_selftest fail     -> condição falsa: imprime arquivo:linha e a condição e sai com 1
//   check_selftest effect   -> a condição é AVALIADA uma vez (assert some com NDEBUG)
//   check_selftest thread   -> falha dentro de outra thread também derruba o processo
// O CTest roda cada caso; os de falha usam WILL_FAIL / PASS_REGULAR_EXPRESSION.

#include "check.hpp"

#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "ok") == 0) {
        SW_CHECK(1 + 1 == 2);
        SW_CHECK(sizeof(int) >= 2 && sizeof(char) == 1);
        return 0;
    }
    if (std::strcmp(mode, "fail") == 0) {
        SW_CHECK(1 + 1 == 3);
        std::puts("chegou depois da falha: SW_CHECK nao abortou");
        return 0;
    }
    if (std::strcmp(mode, "effect") == 0) {
        int calls = 0;
        SW_CHECK(++calls == 1);
        if (calls != 1) {
            std::puts("a condicao nao foi avaliada exatamente uma vez");
            return 1;
        }
        return 0;
    }
    if (std::strcmp(mode, "thread") == 0) {
        std::thread worker([] { SW_CHECK(false && "falha numa thread"); });
        worker.join();
        std::puts("chegou depois da falha na thread");
        return 0;
    }
    std::fprintf(stderr, "modo desconhecido: '%s'\n", mode);
    return 2;
}
