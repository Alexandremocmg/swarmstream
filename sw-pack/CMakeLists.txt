# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# sw-pack: ferramenta de build do conteúdo (docs/03). Hoje: chunker FastCDC (F1-01), SHA-256 e compressão por bloco Zstd/LZ4 (F1-03).

cmake_minimum_required(VERSION 3.20)
project(sw-pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Zstd e LZ4 (F1-03), versões fixas e com hash, compiladas estáticas junto do projeto: o mesmo
# caminho no Linux, no Windows e nos jobs de sanitizer (que não usam o vcpkg).
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz
    URL_HASH SHA256=30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(zstd lz4)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

add_library(sw-pack STATIC
    src/chunker.cpp
    src/codec.cpp
    src/sha256.cpp
)
target_link_libraries(sw-pack PRIVATE libzstd_static lz4_static)
target_include_directories(sw-pack PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(MSVC)
    target_compile_options(sw-pack PRIVATE /W4 /permissive-)
else()
    target_compile_options(sw-pack PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CLI do relatório de compressão por tipo de arquivo (F1-03).
add_executable(sw-pack-report tools/sw_pack_report.cpp)
target_link_libraries(sw-pack-report PRIVATE sw-pack)
if(MSVC)
    target_compile_options(sw-pack-report PRIVATE /W4 /permissive-)
else()
    target_compile_options(sw-pack-report PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Testes. Os vetores (testdata/vectors-v0.json) são a referência do chunker.
if(BUILD_TESTING OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()

    add_executable(sw-pack-tests tests/test_chunker.cpp)
    add_executable(sw-pack-codec-tests tests/test_codec.cpp)
    target_link_libraries(sw-pack-codec-tests PRIVATE sw-pack libzstd_static)
    target_include_directories(sw-pack-codec-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests)
    add_test(NAME sw-pack-codec COMMAND sw-pack-codec-tests)
    target_link_libraries(sw-pack-tests PRIVATE sw-pack)
    # SW_CHECK, a verificação dos testes que não some com NDEBUG (F1-46).
    target_include_directories(sw-pack-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests)
    if(MSVC)
        target_compile_options(sw-pack-tests PRIVATE /W4 /permissive-)
        target_compile_options(sw-pack-codec-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sw-pack-tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(sw-pack-codec-tests PRIVATE -Wall -Wextra -Wpedantic)
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
