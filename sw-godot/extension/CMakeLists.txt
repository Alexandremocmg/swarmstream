# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# GDExtension do Swarmstream (F1-20, portão do ADR-0016).
# Build separado do CMake da raiz: precisa do godot-cpp, que fica fora do git.
#
#   cmake -S sw-godot/extension -B build-godot -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#         -DGODOT_CPP_DIR=<pasta do godot-cpp, tag godot-4.5-stable>
#   cmake --build build-godot
#
# Saída: sw-godot/project/addons/swarmstream/ (DLL e .gdextension; fora do git)

cmake_minimum_required(VERSION 3.20)
project(swarmstream_godot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GODOT_CPP_DIR "" CACHE PATH "Pasta do godot-cpp (tag godot-4.5-stable, a mesma do spike F0)")
if(NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Defina -DGODOT_CPP_DIR=<godot-cpp>. Ele não está no git (ver sw-godot/README.md).")
endif()

set(GODOTCPP_DISABLE_EXCEPTIONS ON)
add_subdirectory("${GODOT_CPP_DIR}" godot-cpp EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../sw-core" sw-core EXCLUDE_FROM_ALL)

# Adaptador do mock do sw-core. Biblioteca separada porque o godot-cpp propaga
# -fno-exceptions e os mocks do sw-core usam exceções.
add_library(sw-godot-gate STATIC src/gate_core.cpp)
target_include_directories(sw-godot-gate PUBLIC src)
target_link_libraries(sw-godot-gate PUBLIC sw-core)

add_library(swarmstream_godot SHARED
    src/register_types.cpp
    src/swarm_loader.cpp
)
target_link_libraries(swarmstream_godot PRIVATE godot::cpp sw-godot-gate)
if(NOT MSVC)
    target_compile_options(swarmstream_godot PRIVATE -Wall -Wextra)
    target_compile_options(sw-godot-gate PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(SWARM_ADDON_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../project/addons/swarmstream/bin")
set_target_properties(swarmstream_godot PROPERTIES
    OUTPUT_NAME "swarmstream_godot"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${SWARM_ADDON_BIN}"
    LIBRARY_OUTPUT_DIRECTORY "${SWARM_ADDON_BIN}"
)
# O .gdextension só entra no projeto quando a extensão é compilada: sem isso,
# quem abre o projeto sem o DLL veria "Can't open dynamic library" toda vez
# (inclusive nas rodadas do baseline).
add_custom_command(TARGET swarmstream_godot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/swarmstream.gdextension"
            "${SWARM_ADDON_BIN}/../swarmstream.gdextension")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(swarmstream_godot PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG} "${SWARM_ADDON_BIN}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG} "${SWARM_ADDON_BIN}")
endforeach()
