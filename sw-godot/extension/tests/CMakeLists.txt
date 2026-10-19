# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# Parte da GDExtension que não depende do Godot: o adaptador GateCore e os
# leitores de formato. Entra no build da raiz para rodar no CI (Linux, Windows
# e ASan+UBSan); o resto da extensão precisa do godot-cpp (../CMakeLists.txt).

add_executable(sw-godot-gate-tests
    test_gate_core.cpp
    ../src/gate_core.cpp
)
target_include_directories(sw-godot-gate-tests PRIVATE ../src)
target_link_libraries(sw-godot-gate-tests PRIVATE sw-core)
if(NOT MSVC)
    target_link_libraries(sw-godot-gate-tests PRIVATE pthread)
endif()
add_test(NAME sw-godot-gate-core COMMAND sw-godot-gate-tests)
