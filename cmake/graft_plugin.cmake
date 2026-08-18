# graft_plugin(<таргет> NAME <имя> [VERSION <n>] SOURCES <файлы...>
#              [MODULES <1_Core 3_Game ...>] [SCRIPTS_DIR <каталог>])
#
# Объявляет плагин: DLL с нативами. Рядом с ней сборка кладёт каталог <ИМЯ>.scripts —
# объявления `proto native` для PBO, разложенные по модулям Enforce:
#
#   build/HELLO_GRAFT.grafted.dll
#   build/HELLO_GRAFT.scripts/3_Game/grafted_natives_HELLO_GRAFT.c
#
# Это ровно как `.pb.cc` у protobuf: артефакт сборки, а не файл в исходниках. Печатает
# их graft.exe, читая собранную DLL, — источник истины один, разъехаться нечему.
# Забрать их в свой мод и собрать PBO — дело мододела: graft мод не собирает.
#
#   graft_plugin(hello NAME HELLO_GRAFT VERSION 1 SOURCES src/hello.cpp)
#
# NAME попадает и в паспорт плагина, и в метку GRAFT_PLUGIN_TAG — от неё зависит имя
# служебного натива NativeDispose_<NAME>, чтобы два плагина, держащие состояние на
# одном скриптовом классе, не спорили за один метод.
#
# MODULES — модули, в которые плагин пишет объявления (из блоков GRAFT_BINDINGS).
# Нужны только затем, чтобы ninja знал файлы поимённо и пересобирал зависящее от них.
# Не указаны — каталог всё равно наполнится, просто без точных зависимостей.
#
# SCRIPTS_DIR — куда класть каталог с объявлениями (по умолчанию рядом с DLL).
function(graft_plugin target)
    cmake_parse_arguments(P "" "NAME;VERSION;SCRIPTS_DIR" "SOURCES;MODULES" ${ARGN})
    if(NOT P_NAME)
        message(FATAL_ERROR "graft_plugin(${target}): нужен NAME")
    endif()
    if(NOT P_SOURCES)
        message(FATAL_ERROR "graft_plugin(${target}): нужен SOURCES")
    endif()

    add_library(${target} SHARED ${P_SOURCES})
    target_link_libraries(${target} PRIVATE graft::client)
    target_compile_definitions(${target} PRIVATE GRAFT_PLUGIN_TAG="${P_NAME}")

    # Хост ищет ровно такие имена; префикса lib быть не должно.
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${P_NAME}.grafted" PREFIX "")

    # Путь считаем на конфигурации, а не генератор-выражением: BYPRODUCTS должны быть
    # известны поимённо, иначе ninja не сможет на них ссылаться.
    if(P_SCRIPTS_DIR)
        set(scripts "${P_SCRIPTS_DIR}")
    elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(scripts "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${P_NAME}.scripts")
    else()
        set(scripts "${CMAKE_CURRENT_BINARY_DIR}/${P_NAME}.scripts")
    endif()

    set(byproducts "")
    foreach(module IN LISTS P_MODULES)
        list(APPEND byproducts "${scripts}/${module}/grafted_natives_${P_NAME}.c")
    endforeach()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "$<TARGET_FILE:graft::tool>" protogen "$<TARGET_FILE:${target}>" "${scripts}"
        BYPRODUCTS ${byproducts}
        COMMENT "graft protogen ${P_NAME} -> ${P_NAME}.scripts"
        VERBATIM)
    add_dependencies(${target} graft_tool)

    # Где лежат объявления — свойство таргета: его читают и сборка мода, и IDE, и
    # чужие скрипты, которым надо забрать сгенерированное.
    set_property(TARGET ${target} PROPERTY GRAFT_SCRIPTS_DIR "${scripts}")
    set_property(TARGET ${target} PROPERTY GRAFT_PLUGIN_NAME "${P_NAME}")
endfunction()
