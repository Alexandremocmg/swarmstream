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
    src/cache_journal.cpp
    src/cache_owner.cpp
    src/deadline_scheduler.cpp
    src/memory_cache_store.cpp
    src/memory_content_source.cpp
    src/http_range_source.cpp
    src/slot_allocator.cpp
    src/store_files.cpp
    src/tcp_socket.cpp
    src/xxh3.cpp
)

# Fonte HTTP (F1-27): threads e, no Windows, Winsock.
find_package(Threads REQUIRED)
target_link_libraries(sw-core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(sw-core PUBLIC ws2_32)
endif()

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
        tests/test_http_range_source.cpp
    )

    target_link_libraries(sw-core-tests PRIVATE sw-core)

    if(NOT MSVC)
        target_link_libraries(sw-core-tests PRIVATE pthread)
    endif()

    add_test(NAME sw-core-tests COMMAND sw-core-tests)

    # F1-10: alocador de slots (1 milhão de slots) e arquivos de store pré-alocados.
    add_executable(sw-core-store-tests tests/test_store.cpp)
    target_link_libraries(sw-core-store-tests PRIVATE sw-core)
    # src/: store_files_testing.hpp (ganchos de teste, fora da API instalada).
    target_include_directories(sw-core-store-tests PRIVATE src)
    add_test(NAME sw-core-store COMMAND sw-core-store-tests)
    set_tests_properties(sw-core-store PROPERTIES TIMEOUT 300)

    # F1-11: XXH3-64 própria contra a biblioteca de referência.
    add_executable(sw-core-xxh3-tests tests/test_xxh3.cpp)
    target_link_libraries(sw-core-xxh3-tests PRIVATE sw-core)
    add_test(NAME sw-core-xxh3 COMMAND sw-core-xxh3-tests)
    # F1-29: estimador de banda e RTT da fonte (só cabeçalho; amostras sintéticas, sem rede).
    add_executable(sw-core-estimator-tests tests/test_source_estimator.cpp)
    target_link_libraries(sw-core-estimator-tests PRIVATE sw-core)
    add_test(NAME sw-core-estimator COMMAND sw-core-estimator-tests)
    add_executable(sw-core-journal-tests tests/test_cache_journal.cpp)
    target_link_libraries(sw-core-journal-tests PRIVATE sw-core)
    target_include_directories(sw-core-journal-tests PRIVATE src)
    add_test(NAME sw-core-journal COMMAND sw-core-journal-tests)
    set_tests_properties(sw-core-journal PROPERTIES TIMEOUT 300)
    add_executable(sw-core-recovery-tests tests/test_cache_recovery.cpp)
    target_link_libraries(sw-core-recovery-tests PRIVATE sw-core)
    target_include_directories(sw-core-recovery-tests PRIVATE src)
    add_test(NAME sw-core-recovery COMMAND sw-core-recovery-tests)
    set_tests_properties(sw-core-recovery PROPERTIES TIMEOUT 300)

    # F1-12: thread dona do cache e fila MPSC (16 leitoras; o job TSan do CI roda este teste).
    add_executable(sw-core-owner-tests tests/test_cache_owner.cpp)
    target_link_libraries(sw-core-owner-tests PRIVATE sw-core)
    # src/: cache_owner_testing.hpp (ganchos de teste, fora da API instalada).
    target_include_directories(sw-core-owner-tests PRIVATE src)
    if(SANITIZE_THREAD)
        target_compile_definitions(sw-core-owner-tests PRIVATE SW_EXPECT_TSAN)
    endif()
    add_test(NAME sw-core-owner COMMAND sw-core-owner-tests)
    set_tests_properties(sw-core-owner PROPERTIES TIMEOUT 300)
    # F1-13: leases com contagem de referência (estresse: peça com lease nunca é removida; o
    # job TSan roda este teste, porque soltar o lease é um atômico fora da dona).
    add_executable(sw-core-lease-tests tests/test_cache_leases.cpp)
    target_link_libraries(sw-core-lease-tests PRIVATE sw-core)
    target_include_directories(sw-core-lease-tests PRIVATE src)
    if(SANITIZE_THREAD)
        target_compile_definitions(sw-core-lease-tests PRIVATE SW_EXPECT_TSAN)
    endif()
    add_test(NAME sw-core-lease COMMAND sw-core-lease-tests)
    set_tests_properties(sw-core-lease PROPERTIES TIMEOUT 300)
    # F1-28: agendador por prazo v1 (fonte falsa controlada pelo teste; estresse com 16 threads
    # pedindo e cancelando; o job TSan roda este teste).
    add_executable(sw-core-scheduler-tests tests/test_deadline_scheduler.cpp)
    target_link_libraries(sw-core-scheduler-tests PRIVATE sw-core)
    add_test(NAME sw-core-scheduler COMMAND sw-core-scheduler-tests)
    set_tests_properties(sw-core-scheduler PROPERTIES TIMEOUT 300)
    # Controle do job TSan: uma corrida de propósito tem de ser reportada (prova que o TSan
    # está ligado; sem isso, "nenhuma corrida" não provaria nada).
    if(SANITIZE_THREAD)
        add_executable(sw-core-tsan-selftest tests/tsan_selftest.cpp)
        target_link_libraries(sw-core-tsan-selftest PRIVATE Threads::Threads)
        add_test(NAME tsan-selftest-detects-race COMMAND sw-core-tsan-selftest)
        set_tests_properties(tsan-selftest-detects-race PROPERTIES
            PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race" TIMEOUT 60)
    endif()
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

    # Fonte HTTP contra o servidor da bancada (sw-netlab) nos 3 perfis (F1-27). Precisa de Python.
    add_executable(sw-core-http-fetch tests/http_fetch_tool.cpp)
    target_link_libraries(sw-core-http-fetch PRIVATE sw-core)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        foreach(profile fibra_300 cabo_50 adsl_10)
            add_test(NAME http-netlab-${profile}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_netlab_test.py
                    $<TARGET_FILE:sw-core-http-fetch>
                    ${CMAKE_CURRENT_SOURCE_DIR}/../sw-netlab/server.py ${profile})
            set_tests_properties(http-netlab-${profile} PROPERTIES TIMEOUT 240 LABELS netlab)
        endforeach()
    else()
        message(STATUS "Python3 nao encontrado: testes http-netlab-* nao serao registrados")
    endif()

    # F1-11: teste de queda (critério de aceite: queda sem peça corrompida). 120 iterações de 3
    # rodadas; o F1-31 roda 1.000 quedas com a mesma ferramenta. Rótulo `crash`: o job TSan o
    # pula (lento sob TSan e sem threads novas a examinar).
    add_executable(sw-core-crash-tool tests/crash_tool.cpp)
    target_link_libraries(sw-core-crash-tool PRIVATE sw-core)
    target_include_directories(sw-core-crash-tool PRIVATE src)
    if(Python3_Interpreter_FOUND)
        add_test(NAME sw-core-crash
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_test.py
                $<TARGET_FILE:sw-core-crash-tool> 120)
        set_tests_properties(sw-core-crash PROPERTIES TIMEOUT 900 LABELS crash)
    else()
        message(STATUS "Python3 nao encontrado: teste de queda sw-core-crash nao sera registrado")
    endif()

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
