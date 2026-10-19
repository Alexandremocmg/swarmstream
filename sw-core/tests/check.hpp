// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Verificação dos testes do sw-core que NÃO some com NDEBUG.
//
// `assert` da <cassert> vira nada em Release/RelWithDebInfo: o teste passaria sem
// verificar coisa alguma, e a expressão nem seria avaliada (efeitos colaterais somem).
// SW_CHECK avalia a condição sempre e, se falhar, imprime arquivo, linha e condição no
// stderr e termina o processo com código 1, em qualquer tipo de build e em qualquer
// thread. Variádica para aceitar vírgulas fora de parênteses (ex.: templates).
#pragma once

#include <cstdio>
#include <cstdlib>

namespace swarmstream::test {

[[noreturn]] inline void check_failed(const char* file, int line, const char* condition)
{
    std::fprintf(stderr, "%s:%d: falhou: %s\n", file, line, condition);
    // _Exit: termina na hora com código 1, sem caixa de diálogo do MSVC nem do Windows, em
    // qualquer thread. Antes, descarrega stdout/stderr para o log do teste não perder linhas.
    std::fflush(nullptr);
    std::_Exit(1);
}

} // namespace swarmstream::test

#define SW_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            ::swarmstream::test::check_failed(__FILE__, __LINE__, #__VA_ARGS__);                   \
        }                                                                                          \
    } while (0)
