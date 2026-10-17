# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# F0-02: build vazia de sw-pack (nenhuma fonte ainda — vira biblioteca assim
# que a especificação do manifesto, F0-08, virar código).

cmake_minimum_required(VERSION 3.20)
project(sw-pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sw-pack INTERFACE)
target_include_directories(sw-pack INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
