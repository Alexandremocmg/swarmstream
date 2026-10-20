# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres

cmake_minimum_required(VERSION 3.20)
project(sw-core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library (headers and mocks)
add_library(sw-core STATIC
    src/api_stub.cpp
    src/memory_cache_store.cpp
    src/memory_content_source.cpp
    src/slot_allocator.cpp
    src/store_files.cpp
)

target_include_directories(sw-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Biblioteca estática: quem inclui swarmstream.h não deve usar dllimport.
target_compile_definitions(sw-core PUBLIC SW_STATIC)

if(MSVC)
    target_compile_options(sw-core PRIVATE /W4 /permissive-)
else()
    target_compile_options(sw-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Test runner
if(BUILD_TESTING OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()

    add_executable(sw-core-tests
        tests/main.cpp
        tests/test_content_source.cpp
        tests/test_cache_store.cpp
        tests/test_pipeline_integration.cpp
    )

    target_link_libraries(sw-core-tests PRIVATE sw-core)

    if(NOT MSVC)
        target_link_libraries(sw-core-tests PRIVATE pthread)
    endif()

    add_test(NAME sw-core-tests COMMAND sw-core-tests)

    # F1-10: alocador de slots (1 milhão de slots) e arquivos de store pré-alocados.
    add_executable(sw-core-store-tests tests/test_store.cpp)
    target_link_libraries(sw-core-store-tests PRIVATE sw-core)
    add_test(NAME sw-core-store COMMAND sw-core-store-tests)
    set_tests_properties(sw-core-store PROPERTIES TIMEOUT 300)
    # Teste pendurado deve falhar, não travar o CI.
    set_tests_properties(sw-core-tests PROPERTIES TIMEOUT 120)

    # SW_CHECK (tests/check.hpp) não pode sumir com NDEBUG. Estes testes provam isso em
    # qualquer tipo de build (Debug e Release): se alguém trocar SW_CHECK por assert, o
    # caso "effect" e as falhas esperadas deixam de falhar/aparecer no build Release.
    add_executable(sw-core-check-selftest tests/check_selftest.cpp)
    target_link_libraries(sw-core-check-selftest PRIVATE sw-core)
    if(NOT MSVC)
        target_link_libraries(sw-core-check-selftest PRIVATE pthread)
    endif()
    add_test(NAME check-selftest-ok COMMAND sw-core-check-selftest ok)
    add_test(NAME check-selftest-evaluates-condition COMMAND sw-core-check-selftest effect)
    # Falha esperada: o processo tem de terminar com erro...
    add_test(NAME check-selftest-fails COMMAND sw-core-check-selftest fail)
    add_test(NAME check-selftest-fails-in-thread COMMAND sw-core-check-selftest thread)
    set_tests_properties(check-selftest-fails check-selftest-fails-in-thread PROPERTIES WILL_FAIL TRUE)
    # ...e dizer arquivo, linha e condição (a regex vale no lugar do código de saída).
    add_test(NAME check-selftest-message COMMAND sw-core-check-selftest fail)
    set_tests_properties(check-selftest-message PROPERTIES
        PASS_REGULAR_EXPRESSION "check_selftest\\.cpp:[0-9]+: falhou: 1 \\+ 1 == 3")
    set_tests_properties(check-selftest-ok check-selftest-evaluates-condition
        check-selftest-fails check-selftest-fails-in-thread check-selftest-message
        PROPERTIES TIMEOUT 60)

    # Contrato swarmstream.h v0 (ADR-0014): layout (sizeof/offsetof) e stub.
    # Teste que quebra aqui = mudança de contrato, que exige ADR.
    add_executable(sw-core-api-v0-tests
        tests/api_v0_smoke.c
        tests/api_v0_cxx.cpp
    )
    set_target_properties(sw-core-api-v0-tests PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    target_link_libraries(sw-core-api-v0-tests PRIVATE sw-core)
    if(MSVC)
        target_compile_options(sw-core-api-v0-tests PRIVATE /W4 /WX)
    else()
        target_compile_options(sw-core-api-v0-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME sw-core-api-v0 COMMAND sw-core-api-v0-tests)
    set_tests_properties(sw-core-api-v0 PROPERTIES TIMEOUT 120)
endif()
