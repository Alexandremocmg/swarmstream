# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# sw-pack: ferramenta de build do conteúdo (docs/03). Hoje: chunker FastCDC (F1-01) e SHA-256.

cmake_minimum_required(VERSION 3.20)
project(sw-pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sw-pack STATIC
    src/chunker.cpp
    src/sha256.cpp
)
target_include_directories(sw-pack PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(MSVC)
    target_compile_options(sw-pack PRIVATE /W4 /permissive-)
else()
    target_compile_options(sw-pack PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Testes. Os vetores (testdata/vectors-v0.json) são a referência do chunker.
if(BUILD_TESTING OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()

    add_executable(sw-pack-tests tests/test_chunker.cpp)
    target_link_libraries(sw-pack-tests PRIVATE sw-pack)
    # SW_CHECK, a verificação dos testes que não some com NDEBUG (F1-46).
    target_include_directories(sw-pack-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests)
    if(MSVC)
        target_compile_options(sw-pack-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sw-pack-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set(SW_PACK_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/vectors-v0.json)
    add_test(NAME sw-pack-chunker COMMAND sw-pack-tests ${SW_PACK_VECTORS})
    set_tests_properties(sw-pack-chunker PROPERTIES TIMEOUT 300)

    # Estabilidade em 1 GiB (mudar/inserir/remover 1 byte altera <= 3 blocos). Pesado: fica
    # fora do CTest padrão. Ligue com -DSW_PACK_HEAVY_TESTS=ON e rode `ctest -L heavy`.
    option(SW_PACK_HEAVY_TESTS "Testes pesados do sw-pack (1 GiB)" OFF)
    if(SW_PACK_HEAVY_TESTS)
        add_test(NAME sw-pack-chunker-1gib COMMAND sw-pack-tests ${SW_PACK_VECTORS} --heavy)
        set_tests_properties(sw-pack-chunker-1gib PROPERTIES TIMEOUT 900 LABELS heavy)
    endif()
endif()
