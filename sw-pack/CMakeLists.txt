# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# sw-pack: ferramenta de build do conteúdo (docs/03). Hoje: chunker FastCDC (F1-01), SHA-256 e
# Ed25519 (libsodium), compressão por bloco Zstd/LZ4 (F1-03) e o manifesto v0 assinado (F1-04).

cmake_minimum_required(VERSION 3.20)
project(sw-pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libsodium (ADR-0012): Ed25519 do manifesto. Com o toolchain do vcpkg (jobs de build/teste do CI e
# o desenvolvimento normal) vem como `unofficial-sodium`; nos jobs sem vcpkg (sanitizers, TSan, lint)
# vem do pacote do sistema (`libsodium-dev`) pelo pkg-config. Sem nenhum dos dois, falha aqui.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    set(SW_SODIUM_TARGET unofficial-sodium::sodium)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SW_SODIUM IMPORTED_TARGET libsodium)
    endif()
    if(TARGET PkgConfig::SW_SODIUM)
        set(SW_SODIUM_TARGET PkgConfig::SW_SODIUM)
    else()
        message(FATAL_ERROR
            "libsodium nao encontrado. Use o toolchain do vcpkg "
            "(-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake, o vcpkg.json declara "
            "libsodium) ou instale libsodium-dev e pkg-config.")
    endif()
endif()

# Zstd e LZ4 (F1-03; licenças e NOTICE: docs/licencas-zstd-lz4.md), versões fixas e com hash, compiladas estáticas junto do projeto: o mesmo
# caminho no Linux, no Windows e nos jobs de sanitizer (que não usam o vcpkg).
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE) # sem decodificadores dos formatos antigos (v0.1-0.7)
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
    src/crypto.cpp
    src/ed25519.cpp
    src/manifest.cpp
)
target_link_libraries(sw-pack PRIVATE ${SW_SODIUM_TARGET} libzstd_static lz4_static)
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

    add_executable(sw-pack-manifest-tests tests/test_manifest.cpp)
    target_link_libraries(sw-pack-manifest-tests PRIVATE sw-pack)
    target_include_directories(sw-pack-manifest-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests)
    if(MSVC)
        target_compile_options(sw-pack-manifest-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sw-pack-manifest-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set(SW_PACK_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/vectors-v0.json)
    add_test(NAME sw-pack-chunker COMMAND sw-pack-tests ${SW_PACK_VECTORS})
    set_tests_properties(sw-pack-chunker PROPERTIES TIMEOUT 300)
    add_test(NAME sw-pack-manifest COMMAND sw-pack-manifest-tests ${SW_PACK_VECTORS}
             ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ed25519-vectors.txt)
    set_tests_properties(sw-pack-manifest PROPERTIES TIMEOUT 600)

    # Medição do manifesto (F1-04): tamanho do arquivo, tempo de abertura e memória residente.
    add_executable(sw-manifest-bench tools/sw_manifest_bench.cpp)
    target_link_libraries(sw-manifest-bench PRIVATE sw-pack)
    if(WIN32)
        target_link_libraries(sw-manifest-bench PRIVATE psapi)
    endif()
    target_include_directories(sw-manifest-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(sw-manifest-bench PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sw-manifest-bench PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Estabilidade em 1 GiB (mudar/inserir/remover 1 byte altera <= 3 blocos). Pesado: fica
    # fora do CTest padrão. Ligue com -DSW_PACK_HEAVY_TESTS=ON e rode `ctest -L heavy`.
    option(SW_PACK_HEAVY_TESTS "Testes pesados do sw-pack (1 GiB)" OFF)
    if(SW_PACK_HEAVY_TESTS)
        add_test(NAME sw-pack-chunker-1gib COMMAND sw-pack-tests ${SW_PACK_VECTORS} --heavy)
        set_tests_properties(sw-pack-chunker-1gib PROPERTIES TIMEOUT 900 LABELS heavy)
    endif()
endif()
