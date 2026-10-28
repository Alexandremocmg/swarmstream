# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# sw-format: o que o CLIENTE precisa para LER, DESCOMPRIMIR e VERIFICAR o conteúdo (docs/03):
# leitor do manifesto v0 (ManifestView::open e as validações do §8), decompress_block, SHA-256 e
# Ed25519 (só verificar) e crypto_ready(). Não tem chunker, construtor, assinatura nem compress_block:
# isso é do sw-pack (ferramenta de build), que depende deste alvo (F1-61).

cmake_minimum_required(VERSION 3.20)
project(sw-format LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libsodium (ADR-0012): Ed25519 e SHA-256. Com o toolchain do vcpkg (jobs de build/teste do CI e
# o desenvolvimento normal) vem como `unofficial-sodium`; nos jobs sem vcpkg (sanitizers, TSan, lint)
# vem do pacote do sistema (`libsodium-dev`) pelo pkg-config. Sem nenhum dos dois, falha aqui.
# O alvo `sw-sodium` (interface) é GLOBAL para o sw-pack, que também usa o libsodium (assinar).
if(NOT TARGET sw-sodium)
    find_package(unofficial-sodium CONFIG QUIET)
    if(TARGET unofficial-sodium::sodium)
        set_target_properties(unofficial-sodium::sodium PROPERTIES IMPORTED_GLOBAL TRUE)
        set(SW_SODIUM_TARGET unofficial-sodium::sodium)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SW_SODIUM IMPORTED_TARGET GLOBAL libsodium)
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
    add_library(sw-sodium INTERFACE)
    target_link_libraries(sw-sodium INTERFACE ${SW_SODIUM_TARGET})
endif()

# Zstd e LZ4 (F1-03; licenças e NOTICE: docs/licencas-zstd-lz4.md), versões fixas e com hash, compiladas estáticas junto do projeto: o mesmo
# caminho no Linux, no Windows e nos jobs de sanitizer (que não usam o vcpkg). O sw-format usa os
# decodificadores; o sw-pack liga as mesmas bibliotecas para comprimir.
if(NOT TARGET libzstd_static)
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
endif()

add_library(sw-format STATIC
    src/block_codec.cpp
    src/crypto.cpp
    src/ed25519_verify.cpp
    src/manifest_view.cpp
    src/sha256.cpp
)
target_link_libraries(sw-format PRIVATE sw-sodium libzstd_static lz4_static)
target_include_directories(sw-format PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(MSVC)
    target_compile_options(sw-format PRIVATE /W4 /permissive-)
else()
    target_compile_options(sw-format PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Binário mínimo de cliente (F1-61): liga SÓ o sw-format e usa ManifestView::open e
# decompress_block. É o alvo cujos símbolos o teste `sw-format-symbols` (registrado no sw-pack, que
# enxerga os dois lados) inspeciona.
if(BUILD_TESTING OR CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()

    add_executable(sw-format-client-min tests/client_min.cpp tests/client_min_fixture.hpp)
    target_link_libraries(sw-format-client-min PRIVATE sw-format)
    if(MSVC)
        target_compile_options(sw-format-client-min PRIVATE /W4 /permissive-)
        # Mapa do ligador: lista o que entrou no executável (o MSVC não deixa tabela de símbolos).
        target_link_options(sw-format-client-min PRIVATE
            "/MAP:$<TARGET_FILE_DIR:sw-format-client-min>/sw-format-client-min.map")
    else()
        target_compile_options(sw-format-client-min PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME sw-format-client-min COMMAND sw-format-client-min)
endif()
