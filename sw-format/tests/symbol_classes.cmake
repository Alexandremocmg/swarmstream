# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
#
# Classifica as linhas de `nm` (GNU) ou do mapa do ligador do MSVC (/MAP) que casam com um padrão
# (F1-61). "Está no binário" não é uma coisa só:
#   DEF = DEFINIÇÃO: o código/dado está NO executável (nm: T t W w V v D d B b R r S s G g I i;
#         mapa: linha de função/objeto `.obj` ligado estaticamente);
#   UND = REFERÊNCIA INDEFINIDA (nm: U): o executável pede o símbolo a uma biblioteca
#         DINÂMICA (.so/.dll) no momento de rodar; o código NÃO está no executável;
#   IMP = IMPORTAÇÃO do Windows (mapa: `__imp_...` ou `Lib:arquivo.dll`): idem, vem de uma DLL;
#   OUT = outras linhas (nm: N de depuração, p/pdata e nomes de seção que começam com ".";
#         mapa: o que não for reconhecido).
# Uso: sw_classify(PREFIXO LISTA_DE_LINHAS PADRAO MODO)   (MODO = nm | map)
# Devolve PREFIXO_DEF, PREFIXO_UND, PREFIXO_IMP, PREFIXO_OUT e PREFIXO_TEXTO.

function(sw_classify prefix lines_var pattern mode)
    set(lines ${${lines_var}})
    list(FILTER lines INCLUDE REGEX "${pattern}")
    set(def 0)
    set(und 0)
    set(imp 0)
    set(out 0)
    foreach(line IN LISTS lines)
        if(mode STREQUAL "nm")
            if(line MATCHES "^[0-9a-fA-F ]*([A-Za-z?-]) ([^ ]+)$")
                set(type "${CMAKE_MATCH_1}")
                set(name "${CMAKE_MATCH_2}")
                if(name MATCHES "^[.]")
                    math(EXPR out "${out}+1")
                elseif(type MATCHES "^[TtWwVvDdBbRrSsGgIi]$")
                    math(EXPR def "${def}+1")
                elseif(type MATCHES "^[Uu]$")
                    math(EXPR und "${und}+1")
                else()
                    math(EXPR out "${out}+1")
                endif()
            else()
                math(EXPR out "${out}+1")
            endif()
        else()
            if(line MATCHES "__imp_" OR line MATCHES ":[^ ]*[.][dD][lL][lL] *$")
                math(EXPR imp "${imp}+1")
            elseif(line MATCHES "^ *[0-9a-fA-F]+:[0-9a-fA-F]+ +[^ ]+ +[0-9a-fA-F]+ +(f +|i +)?[^ ]*[.]obj *$")
                math(EXPR def "${def}+1")
            else()
                math(EXPR out "${out}+1")
            endif()
        endif()
    endforeach()
    set(${prefix}_DEF ${def} PARENT_SCOPE)
    set(${prefix}_UND ${und} PARENT_SCOPE)
    set(${prefix}_IMP ${imp} PARENT_SCOPE)
    set(${prefix}_OUT ${out} PARENT_SCOPE)
    set(${prefix}_TEXTO "definido=${def} indefinido(U)=${und} importacao=${imp} outros=${out}" PARENT_SCOPE)
endfunction()
