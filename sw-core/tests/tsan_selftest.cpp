// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Controle do job TSan (F1-12): uma corrida de propósito. Só é compilado com SANITIZE_THREAD, e
// o teste passa apenas se o TSan imprimir "ThreadSanitizer: data race". Sem isso, um job TSan
// verde poderia ser um TSan que não está ligado (teste que passa sem testar nada).

#include <cstdio>
#include <thread>

namespace {
int g_counter = 0; // NÃO atômico, de propósito

void bump()
{
    for (int i = 0; i < 100000; ++i) {
        ++g_counter;
    }
}
} // namespace

int main()
{
    std::thread a(bump);
    std::thread b(bump);
    a.join();
    b.join();
    std::printf("contador: %d\n", g_counter);
    return 0;
}
