# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# Inspeção de símbolos do cliente mínimo (F1-61). Rodado pelo CTest (`sw-format-symbols`):
#   cmake -DEXE=... -DFORMAT_LIB=... -DPACK_LIB=... -DNM=... -DMAP_FILE=... -P check_symbols.cmake
# Prova o CONTEÚDO do binário (o grafo de alvos prova só a dependência entre alvos):
#   1. escreve a configuração de build (compilador, ligador, flags, tipo, LTO, gc-sections);
#   2. exige no executável os símbolos de ler/verificar/descomprimir;
#   3. exige a AUSÊNCIA dos símbolos do sw-pack (chunker, construtor, assinatura, compress_block);
#   4. só REPORTA, sem falhar, os símbolos de compressão/assinatura das bibliotecas de terceiros,
#      porque o que o ligador descarta depende dele e das flags (não presumimos nem uma coisa nem outra);
#   5. confere nos arquivos .a/.lib que cada símbolo do leitor é definido SÓ no sw-format e que o
#      sw-format não define nada de escrita (uma só implementação).
# Sem a ferramenta (nm no GCC/Clang; dumpbin e o mapa do ligador no MSVC) o teste se declara
# PULADO e diz o motivo (o CTest o marca como "Skipped" pela linha "PULADO:"); nunca passa em
# silêncio.

function(skip reason)
    message(STATUS "PULADO: ${reason}")
    message(FATAL_ERROR "a inspeção de símbolos NÃO foi feita nesta configuração")
endfunction()

# ---------------------------------------------------------------- configuração de build
function(first_line out text)
    string(REGEX REPLACE "\r?\n.*" "" line "${text}")
    set(${out} "${line}" PARENT_SCOPE)
endfunction()

set(linker_banner "(não obtido)")
if(MSVC_MODE)
    execute_process(COMMAND "${LINKER}" OUTPUT_VARIABLE lb ERROR_VARIABLE lb2 RESULT_VARIABLE lrc)
    first_line(linker_banner "${lb}${lb2}")
elseif(CXX)
    execute_process(COMMAND "${CXX}" -Wl,--version OUTPUT_VARIABLE lb ERROR_VARIABLE lb2
                    RESULT_VARIABLE lrc)
    first_line(linker_banner "${lb}${lb2}")
endif()

set(all_flags "${CXX_FLAGS} ${LINK_FLAGS}")
function(flag_state out needle)
    string(FIND "${all_flags}" "${needle}" pos)
    if(pos GREATER -1)
        set(${out} "SIM" PARENT_SCOPE)
    else()
        set(${out} "não passado" PARENT_SCOPE)
    endif()
endfunction()
flag_state(st_gc "gc-sections")
flag_state(st_fsec "ffunction-sections")
flag_state(st_flto "flto")
flag_state(st_ref "OPT:REF")
flag_state(st_gl "/GL")
flag_state(st_ltcg "LTCG")

message(STATUS "== CONFIGURAÇÃO DE BUILD (sw-format-symbols) ==")
message(STATUS "compilador : ${CXX_ID} ${CXX_VERSION} (${CXX})")
message(STATUS "ligador    : ${linker_banner}")
message(STATUS "tipo       : ${BUILD_TYPE}   LTO(CMAKE_INTERPROCEDURAL_OPTIMIZATION)=${IPO}")
message(STATUS "flags cxx  : ${CXX_FLAGS}")
message(STATUS "flags link : ${LINK_FLAGS}")
set(lto_state "não")
if(IPO OR st_flto STREQUAL "SIM" OR st_gl STREQUAL "SIM" OR st_ltcg STREQUAL "SIM")
    set(lto_state "SIM")
endif()
message(STATUS "LTO ligado: ${lto_state}   (IPO=${IPO}; -flto/GL/LTCG nas flags: ${st_flto}/${st_gl}/${st_ltcg})")
message(STATUS "-Wl,--gc-sections: ${st_gc}   -ffunction-sections: ${st_fsec}")
message(STATUS "/OPT:REF explícito: ${st_ref}   /GL: ${st_gl}   /LTCG: ${st_ltcg}"
               "   (no MSVC, /OPT:REF é padrão do link.exe fora de /DEBUG; aqui só conta o explícito)")

# ---------------------------------------------------------------- texto de símbolos do executável
if(MSVC_MODE)
    if(NOT EXISTS "${MAP_FILE}")
        skip("mapa do ligador ${MAP_FILE} não existe")
    endif()
    file(READ "${MAP_FILE}" exe_syms)
    set(exe_source "mapa do ligador (/MAP): ${MAP_FILE}")
else()
    if(NOT NM)
        skip("nm não encontrado")
    endif()
    execute_process(COMMAND "${NM}" "${EXE}" OUTPUT_VARIABLE exe_syms ERROR_VARIABLE nm_err
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "nm falhou (${rc}): ${nm_err}")
    endif()
    set(exe_source "nm ${EXE}")
endif()
string(LENGTH "${exe_syms}" exe_len)
if(exe_len LESS 1000)
    message(FATAL_ERROR "lista de símbolos do executável suspeitamente curta (${exe_len} bytes): ${exe_source}")
endif()
message(STATUS "símbolos do executável: ${exe_source} (${exe_len} bytes)")

# "Está no binário" tem 3 sentidos que NÃO se misturam (ver symbol_classes.cmake): DEFINIÇÃO (o código
# está no executável), REFERÊNCIA INDEFINIDA `U` (vem de uma biblioteca dinâmica na hora de rodar) e
# IMPORTAÇÃO do Windows (`__imp_`/DLL). Cada símbolo abaixo sai com a sua classe.
include("${CMAKE_CURRENT_LIST_DIR}/symbol_classes.cmake")
if(MSVC_MODE)
    set(sym_mode "map")
else()
    set(sym_mode "nm")
endif()
string(REPLACE "
" ";" exe_lines "${exe_syms}")
message(STATUS "classes: definido=código NO executável (nm T/t/W...; mapa .obj) | indefinido(U)=pedido a "
               "biblioteca dinâmica | importacao=DLL (__imp_/.dll) | outros=seções/depuração")

# Padrões independentes de decoração: Itanium (`_ZN11swarmstream4pack12ManifestView4open...`),
# MSVC (`?open@ManifestView@pack@swarmstream@@...`) e nome C simples. Os NOSSOS símbolos são
# casados com o namespace (`pack`) para não confundir com nomes parecidos de terceiros
# (ex.: `crypto_sign_ed25519_*` do libsodium).
function(ours out name)
    set(${out} "4pack[0-9]+${name}|[?]${name}@pack@" PARENT_SCOPE)
endfunction()
# Classe: o comprimento do nome entra no padrão Itanium para não casar `ChunkerInfo` (do manifesto)
# com `Chunker`.
function(ours_class out name length)
    set(${out} "4pack${length}${name}|[@]${name}@pack@" PARENT_SCOPE)
endfunction()
ours(P_OPEN "ManifestView")       # só para o texto; o leitor tem padrão próprio abaixo
ours(P_DECOMPRESS "decompress_block")
ours(P_READY "crypto_ready")
ours(P_VERIFY "ed25519_verify")
ours(P_COMPRESS "compress_block")
ours(P_BUILD "build_manifest")
ours(P_ENCODE "encode_manifest")
ours(P_SIGNP "sign_payload")
ours(P_SIGN "ed25519_sign")
ours(P_PUBKEY "ed25519_public_key")
ours(P_CODEC_EXT "codec_for_extension")
ours(P_CODEC_PATH "codec_for_path")
ours(P_PROBE "probe_min_reader")
ours(P_KEYID "key_id_of")
ours_class(P_CHUNKER "Chunker" 7)
ours_class(P_SHA "Sha256" 6)
set(P_OPEN "ManifestView4open|[?]open@ManifestView@pack@")

set(REQUIRED
    "${P_OPEN}"                             # leitor do manifesto
    "${P_DECOMPRESS}"                       # descompressão
    "${P_READY}"                            # inicialização do libsodium
    "${P_VERIFY}"                           # verificação de assinatura (nossa interface)
    "ZSTD_decompress"                       # decodificador Zstd
    "LZ4_decompress_safe"                   # decodificador LZ4
)
set(FORBIDDEN # nossos símbolos de empacotamento: não podem estar no cliente
    "${P_COMPRESS}" "${P_CHUNKER}" "${P_BUILD}" "${P_ENCODE}" "${P_SIGNP}" "${P_SIGN}"
    "${P_PUBKEY}" "${P_CODEC_EXT}" "${P_CODEC_PATH}"
)
set(REPORT_ONLY # de terceiros: depende do ligador; a evidência registra, não presume
    "ZSTD_compress"
    "LZ4_compress"
    "crypto_sign_detached"
    "crypto_sign_seed_keypair"
)
# A verificação no libsodium: no MSVC com libsodium em DLL o mapa pode listar só a importação
# (`__imp_...`), então ali ela é só relatada; nas demais toolchains é exigida.
if(MSVC_MODE)
    list(APPEND REPORT_ONLY "crypto_sign_verify_detached")
else()
    list(APPEND REQUIRED "crypto_sign_verify_detached")
endif()

set(failures 0)
foreach(pattern IN LISTS REQUIRED)
    sw_classify(C "exe_lines" "${pattern}" ${sym_mode})
    if("${exe_syms}" MATCHES "${pattern}")
        message(STATUS "PRESENTE  (exigido)  ${pattern}   [${C_TEXTO}]")
        # No nm (formato conhecido) o NOSSO símbolo e os decodificadores têm de ser DEFINIÇÃO; só o
        # libsodium pode ser definição OU referência dinâmica (libsodium-dev nos jobs sem vcpkg).
        if(NOT MSVC_MODE)
            if(pattern STREQUAL "crypto_sign_verify_detached")
                math(EXPR any "${C_DEF}+${C_UND}+${C_IMP}")
                if(any EQUAL 0)
                    message(STATUS "  ERRO: ${pattern} casa no texto mas não é definição nem referência")
                    math(EXPR failures "${failures}+1")
                endif()
            elseif(C_DEF EQUAL 0)
                if(lto_state STREQUAL "SIM")
                    # Com LTO a função pode ter sido INLINADA: sobra só um nome sem código (nm tipo A/N).
                    message(STATUS "  AVISO: ${pattern} não é definição; com LTO a função pode estar inlinada "
                                   "(o nome que sobrou não é código). Presença NÃO provada por símbolo.")
                else()
                    message(STATUS "  ERRO: ${pattern} não é DEFINIÇÃO (T/t/W...) no executável")
                    math(EXPR failures "${failures}+1")
                endif()
            endif()
        else()
            math(EXPR any "${C_DEF}+${C_IMP}")
            if(any EQUAL 0)
                message(STATUS "  AVISO: classificação do mapa não reconhecida para ${pattern} (o texto casou)")
            endif()
        endif()
    else()
        message(STATUS "AUSENTE   (exigido)  ${pattern}   <-- ERRO")
        math(EXPR failures "${failures}+1")
    endif()
endforeach()
foreach(pattern IN LISTS FORBIDDEN)
    if("${exe_syms}" MATCHES "${pattern}")
        sw_classify(C "exe_lines" "${pattern}" ${sym_mode})
        message(STATUS "PRESENTE  (proibido) ${pattern}   [${C_TEXTO}]   <-- ERRO")
        math(EXPR failures "${failures}+1")
    else()
        message(STATUS "AUSENTE   (proibido) ${pattern}")
    endif()
endforeach()
foreach(pattern IN LISTS REPORT_ONLY)
    if("${exe_syms}" MATCHES "${pattern}")
        sw_classify(C "exe_lines" "${pattern}" ${sym_mode})
        message(STATUS "PRESENTE  (so relato) ${pattern}   [${C_TEXTO}]")
    else()
        message(STATUS "AUSENTE   (so relato) ${pattern}")
    endif()
endforeach()

message(STATUS "(so relato: presença NÃO é falha; o que o ligador descarta de uma biblioteca de "
               "terceiros depende dele e das flags. Ex.: o lz4.c é UMA unidade de tradução, então "
               "LZ4_compress* entra junto de LZ4_decompress*; ver o cartão F1-61.)")

# ---------------------------------------------------------------- conteúdo da DLL distribuída (Windows)
# Ausência de IMPORTAÇÃO no .exe não é ausência na distribuição: o vcpkg (x64-windows) distribui a
# libsodium.dll inteira ao lado do executável. Aqui se lista o que ela EXPORTA (dumpbin /EXPORTS).
# Se a DLL ou a ferramenta não existir, diz que NÃO inspecionou (não falha: é informação).
if(MSVC_MODE)
    get_filename_component(exe_dir "${EXE}" DIRECTORY)
    set(dll_path "${exe_dir}/libsodium.dll")
    if(EXISTS "${dll_path}" AND DUMPBIN)
        execute_process(COMMAND "${DUMPBIN}" /EXPORTS "${dll_path}" OUTPUT_VARIABLE dll_text
                        RESULT_VARIABLE dll_rc)
        if(dll_rc EQUAL 0)
            string(REPLACE "
" ";" dll_lines "${dll_text}")
            set(dll_exports ${dll_lines})
            list(FILTER dll_exports INCLUDE REGEX "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_]")
            list(LENGTH dll_exports dll_n)
            message(STATUS "DLL distribuída: ${dll_path}: ${dll_n} funções exportadas (dumpbin /EXPORTS)")
            foreach(fn crypto_sign_detached crypto_sign_seed_keypair crypto_sign_verify_detached
                       crypto_hash_sha256)
                if("${dll_text}" MATCHES "[ ]${fn}[ 
]")
                    message(STATUS "  DLL EXPORTA ${fn}")
                else()
                    message(STATUS "  DLL NÃO exporta ${fn}")
                endif()
            endforeach()
        else()
            message(STATUS "DLL distribuída: dumpbin /EXPORTS falhou (${dll_rc}); conteúdo da DLL NÃO inspecionado")
        endif()
    else()
        message(STATUS "DLL distribuída: libsodium.dll não encontrada ao lado de ${EXE} (ou sem dumpbin); "
                       "conteúdo da DLL NÃO inspecionado")
    endif()
endif()

# ---------------------------------------------------------------- uma só implementação (arquivos .a/.lib)
function(defined_symbols out lib)
    if(MSVC_MODE)
        if(NOT DUMPBIN)
            skip("dumpbin não encontrado")
        endif()
        execute_process(COMMAND "${DUMPBIN}" /SYMBOLS "${lib}" OUTPUT_VARIABLE text RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "dumpbin falhou (${rc}) em ${lib}")
        endif()
        # Define símbolo: linha com "External" e sem "UNDEF".
        string(REPLACE "\n" ";" lines "${text}")
        set(defs "")
        foreach(line IN LISTS lines)
            if(line MATCHES "External" AND NOT line MATCHES "UNDEF")
                list(APPEND defs "${line}")
            endif()
        endforeach()
        string(REPLACE ";" "\n" text "${defs}")
    else()
        execute_process(COMMAND "${NM}" --defined-only "${lib}" OUTPUT_VARIABLE text
                        ERROR_VARIABLE err RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "nm falhou (${rc}) em ${lib}: ${err}")
        endif()
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

defined_symbols(format_defs "${FORMAT_LIB}")
defined_symbols(pack_defs "${PACK_LIB}")
string(LENGTH "${format_defs}" fl)
string(LENGTH "${pack_defs}" pl)
if(fl LESS 500 OR pl LESS 500)
    message(FATAL_ERROR "lista de símbolos das bibliotecas suspeitamente curta (${fl}, ${pl})")
endif()

set(ONLY_IN_FORMAT # leitura, descompressão e verificação: só no sw-format
    "${P_OPEN}" "${P_DECOMPRESS}" "${P_READY}" "${P_VERIFY}" "${P_PROBE}" "${P_KEYID}" "${P_SHA}")
set(ONLY_IN_PACK # escrita, assinatura, compressão e chunker: só no sw-pack
    "${P_COMPRESS}" "${P_BUILD}" "${P_ENCODE}" "${P_SIGNP}" "${P_SIGN}" "${P_PUBKEY}" "${P_CHUNKER}"
    "${P_CODEC_EXT}" "${P_CODEC_PATH}")
foreach(pattern IN LISTS ONLY_IN_FORMAT)
    if(NOT "${format_defs}" MATCHES "${pattern}")
        message(STATUS "ERRO: ${pattern} não é definido no sw-format")
        math(EXPR failures "${failures}+1")
    elseif("${pack_defs}" MATCHES "${pattern}")
        message(STATUS "ERRO: ${pattern} é definido TAMBÉM no sw-pack (duplicação)")
        math(EXPR failures "${failures}+1")
    else()
        message(STATUS "OK        ${pattern}: definido só no sw-format")
    endif()
endforeach()
foreach(pattern IN LISTS ONLY_IN_PACK)
    if(NOT "${pack_defs}" MATCHES "${pattern}")
        message(STATUS "ERRO: ${pattern} não é definido no sw-pack")
        math(EXPR failures "${failures}+1")
    elseif("${format_defs}" MATCHES "${pattern}")
        message(STATUS "ERRO: ${pattern} é definido no sw-format (cliente carregaria ferramenta de build)")
        math(EXPR failures "${failures}+1")
    else()
        message(STATUS "OK        ${pattern}: definido só no sw-pack")
    endif()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "RESULTADO: FALHOU (${failures} verificações)")
endif()
message(STATUS "RESULTADO: OK")
