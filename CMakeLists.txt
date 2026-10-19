# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres

cmake_minimum_required(VERSION 3.20)
project(swarmstream LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# MinGW: linka libgcc/libstdc++ estaticamente. Sem isso o .exe carrega a
# libstdc++-6.dll que vier primeiro no PATH (ex.: MSYS2 vs WinLibs) e morre
# com 0xc0000139 (entry point not found).
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

# Sanitizers (F0-03): -DSANITIZE=ON liga ASan+UBSan em tudo que vem depois.
# Só faz sentido com GCC/Clang no Unix — no MSVC isso não existe.
option(SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SANITIZE não suporta MSVC; use o job de sanitizers do Linux")
    endif()
    add_compile_options(
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g
        -O1
    )
    add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

# Submodules
add_subdirectory(sw-core)
add_subdirectory(sw-pack)

# Parte da GDExtension do Godot que roda sem o Godot (F1-20)
if(BUILD_TESTING)
    add_subdirectory(sw-godot/extension/tests)
endif()
