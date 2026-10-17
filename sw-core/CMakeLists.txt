# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres

cmake_minimum_required(VERSION 3.20)
project(sw-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library (headers and mocks)
add_library(sw-core STATIC
    src/api_stub.cpp
    src/memory_cache_store.cpp
    src/memory_content_source.cpp
)

target_include_directories(sw-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

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
endif()
