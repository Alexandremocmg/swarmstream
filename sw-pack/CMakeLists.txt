# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# sw-pack: ferramenta de build do conteúdo (docs/03). Hoje: chunker FastCDC (F1-01), assinatura
# Ed25519 (libsodium), compressão por bloco Zstd/LZ4 (F1-03) e a escrita do manifesto v0 assinado
# (F1-04). Leitura, descompressão e verificação são do sw-format (F1-61), de que este alvo depende.

cmake_minimum_required(VERSION 3.20)
project(sw-pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# O que o cliente precisa (ler, descomprimir, verificar) é do sw-format (F1-61); este alvo é a
# ferramenta de build e depende dele. O sw-format também acha o libsodium (alvo `sw-sodium`) e
# traz o Zstd/LZ4 (F1-03; licenças e NOTICE: docs/licencas-zstd-lz4.md) compilados estáticos.
if(NOT TARGET sw-format)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sw-format sw-format)
endif()

add_library(sw-pack STATIC
    src/chunker.cpp
    src/codec.cpp
    src/ed25519.cpp
    src/manifest_build.cpp
)
target_link_libraries(sw-pack PUBLIC sw-format PRIVATE sw-sodium libzstd_static lz4_static)
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

# Gerador das fixtures do VFS (F1-17): manifestos assinados para os testes do sw-core. Só compila
# aqui (para não apodrecer); o cabeçalho gerado é versionado em sw-core/tests/vfs_fixture.hpp.
add_executable(gen-vfs-fixture tools/gen_vfs_fixture.cpp)
target_link_libraries(gen-vfs-fixture PRIVATE sw-pack)
target_include_directories(gen-vfs-fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sw-core/tests)
if(MSVC)
    target_compile_options(gen-vfs-fixture PRIVATE /W4 /permissive-)
else()
    target_compile_options(gen-vfs-fixture PRIVATE -Wall -Wextra -Wpedantic)
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

    # F1-61: o que ENTRA no binário de um cliente que liga só o sw-format (o grafo de alvos prova
    # dependência; os símbolos provam conteúdo). Veja sw-format/tests/check_symbols.cmake.
    if(TARGET sw-format-client-min)
        set(SW_SYMBOL_ARGS "")
        if(MSVC)
            get_filename_component(SW_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
            find_program(SW_DUMPBIN dumpbin HINTS "${SW_LINKER_DIR}")
            list(APPEND SW_SYMBOL_ARGS -DMSVC_MODE=ON "-DDUMPBIN=${SW_DUMPBIN}"
                 "-DMAP_FILE=$<TARGET_FILE_DIR:sw-format-client-min>/sw-format-client-min.map")
        else()
            if(NOT CMAKE_NM)
                find_program(CMAKE_NM nm)
            endif()
            list(APPEND SW_SYMBOL_ARGS "-DNM=${CMAKE_NM}")
        endif()
        add_test(NAME sw-format-symbols
            COMMAND ${CMAKE_COMMAND}
                "-DEXE=$<TARGET_FILE:sw-format-client-min>"
                "-DFORMAT_LIB=$<TARGET_FILE:sw-format>"
                "-DPACK_LIB=$<TARGET_FILE:sw-pack>"
                "-DLINKER=${CMAKE_LINKER}"
                "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DCXX_ID=${CMAKE_CXX_COMPILER_ID}"
                "-DCXX_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
                "-DBUILD_TYPE=$<CONFIG>"
                "-DIPO=${CMAKE_INTERPROCEDURAL_OPTIMIZATION}"
                "-DCXX_FLAGS=${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}> $<JOIN:$<TARGET_PROPERTY:sw-format-client-min,COMPILE_OPTIONS>, >"
                "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS} $<$<CONFIG:Debug>:${CMAKE_EXE_LINKER_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_EXE_LINKER_FLAGS_RELEASE}> $<JOIN:$<TARGET_PROPERTY:sw-format-client-min,LINK_OPTIONS>, >"
                ${SW_SYMBOL_ARGS}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/../sw-format/tests/check_symbols.cmake)
        # Autoteste do classificador (definição x referência indefinida U x importação); não usa nm.
        add_test(NAME sw-format-symbols-selftest
            COMMAND ${CMAKE_COMMAND}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/../sw-format/tests/symbol_classes_selftest.cmake)
        # Sem a ferramenta (nm/dumpbin/mapa) o script escreve "PULADO:" e o CTest o marca Skipped.
        set_tests_properties(sw-format-symbols PROPERTIES TIMEOUT 120
            SKIP_REGULAR_EXPRESSION "PULADO:")
    endif()

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
