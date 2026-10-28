# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# Autoteste do classificador de símbolos (F1-61): linhas de exemplo de `nm` e do mapa do MSVC com a
# classe esperada. Garante que DEFINIÇÃO, REFERÊNCIA INDEFINIDA (U) e IMPORTAÇÃO não se confundem.
#   cmake -P symbol_classes_selftest.cmake
# As linhas do mapa seguem o formato documentado do link.exe (/MAP); o log do CI mostra as linhas
# reais, e este teste só impede que a lógica de classificação regrida.

include("${CMAKE_CURRENT_LIST_DIR}/symbol_classes.cmake")

set(failures 0)
function(expect label got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(STATUS "FALHOU ${label}: obtido '${got}', esperado '${want}'")
        math(EXPR n "${failures}+1")
        set(failures ${n} PARENT_SCOPE)
    else()
        message(STATUS "ok ${label}: ${got}")
    endif()
endfunction()

# ---- nm
set(nm_lines
    "0000000140001000 T crypto_sign_verify_detached"
    "0000000140001010 t crypto_sign_detached"
    "0000000140001020 W crypto_sign_open"
    "                 U crypto_sign_seed_keypair"
    "0000000140001030 N .debug_frame\$crypto_sign_x"
    "0000000140001040 t .text\$crypto_sign_y"
    "0000000140001050 p .pdata\$crypto_sign_z")
sw_classify(NM1 nm_lines "crypto_sign_verify_detached" nm)
expect("nm T definido" "${NM1_DEF}/${NM1_UND}/${NM1_IMP}" "1/0/0")
sw_classify(NM2 nm_lines "crypto_sign_detached" nm)
expect("nm t definido" "${NM2_DEF}/${NM2_UND}/${NM2_IMP}" "1/0/0")
sw_classify(NM3 nm_lines "crypto_sign_seed_keypair" nm)
expect("nm U indefinido" "${NM3_DEF}/${NM3_UND}/${NM3_IMP}" "0/1/0")
sw_classify(NM4 nm_lines "crypto_sign_[xyz]" nm)
expect("nm secoes e depuracao nao contam como definicao" "${NM4_DEF}/${NM4_OUT}" "0/3")

# ---- mapa do MSVC
set(map_lines
    " 0001:00001234       ?open@ManifestView@pack@swarmstream@@SA?AUManifestStatus@23@XZ 0000000140002234 f   manifest_view.cpp.obj"
    " 0001:00002000       ZSTD_decompress            0000000140003000 f   libzstd_static:zstd_decompress.c.obj"
    " 0002:00000100       __imp_crypto_sign_verify_detached 0000000140014100     libsodium:libsodium.dll"
    " 0002:00000108       crypto_sign_verify_detached 0000000140014108     libsodium:libsodium.dll")
sw_classify(MP1 map_lines "open@ManifestView@pack@" map)
expect("mapa funcao do .obj = definido" "${MP1_DEF}/${MP1_UND}/${MP1_IMP}" "1/0/0")
sw_classify(MP2 map_lines "ZSTD_decompress" map)
expect("mapa biblioteca estatica = definido" "${MP2_DEF}/${MP2_UND}/${MP2_IMP}" "1/0/0")
sw_classify(MP3 map_lines "crypto_sign_verify_detached" map)
expect("mapa importacao de DLL" "${MP3_DEF}/${MP3_UND}/${MP3_IMP}" "0/0/2")

if(failures GREATER 0)
    message(FATAL_ERROR "autoteste do classificador FALHOU (${failures})")
endif()
message(STATUS "autoteste do classificador: OK")
