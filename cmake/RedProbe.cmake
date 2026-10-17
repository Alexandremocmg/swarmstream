# Script do CTest usado apenas para provar o critério de aceite do F0-03
# ("PR com teste falhando fica vermelho"). Não faz parte do build normal:
#   ctest --test-dir build -S cmake/RedProbe.cmake
# deve terminar com código de saída diferente de zero.
add_test(NAME red_probe COMMAND ${CMAKE_COMMAND} -E false)