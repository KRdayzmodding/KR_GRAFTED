# graft_plugin(<таргет> NAME <имя> [VERSION <n>] SOURCES <файлы...> [MOD_SCRIPTS <каталог>])
#
# Объявляет плагин: DLL с нативами, которую хост подхватит из <мод>/graft/ или из
# <каталог игры>/graft/. Заменяет собой шестьдесят строк копипасты, которые раньше
# приходилось таскать в каждый проект: MinHook, список исходников библиотеки, свой
# генератор объявлений, переименование в dwmapi.
#
#   graft_plugin(mymod NAME MYMOD VERSION 1
#                SOURCES src/natives.cpp src/plugin.cpp
#                MOD_SCRIPTS P:/MYMOD/scripts)
#
# NAME попадает и в паспорт плагина, и в метку GRAFT_PLUGIN_TAG — от неё зависит имя
# служебного натива NativeDispose_<NAME>, чтобы два плагина, держащие состояние на
# одном скриптовом классе, не спорили за один метод.
function(graft_plugin target)
    cmake_parse_arguments(P "" "NAME;VERSION;MOD_SCRIPTS" "SOURCES" ${ARGN})
    if(NOT P_NAME)
        message(FATAL_ERROR "graft_plugin(${target}): нужен NAME")
    endif()
    if(NOT P_SOURCES)
        message(FATAL_ERROR "graft_plugin(${target}): нужен SOURCES")
    endif()

    add_library(${target} SHARED ${P_SOURCES})
    target_link_libraries(${target} PRIVATE graft_client)
    target_compile_definitions(${target} PRIVATE GRAFT_PLUGIN_TAG="${P_NAME}")

    # Хост ищет ровно такие имена; префикса lib быть не должно.
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${P_NAME}.grafted" PREFIX "")

    # Объявления для PBO печатает graft.exe, читая саму собранную DLL: плагин сам себе
    # реестр, поэтому персональный генератор проекту не нужен.
    if(P_MOD_SCRIPTS AND EXISTS "${P_MOD_SCRIPTS}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<TARGET_FILE:graft_tool>" protogen "$<TARGET_FILE:${target}>"
                    "${P_MOD_SCRIPTS}"
            COMMENT "graft protogen ${P_NAME} -> ${P_MOD_SCRIPTS}"
            VERBATIM)
        add_dependencies(${target} graft_tool)
    endif()
endfunction()
