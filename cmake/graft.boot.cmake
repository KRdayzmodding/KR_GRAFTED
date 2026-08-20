# graft.boot.cmake — втянуть graft в свой проект одной строкой.
#
# Кладётся рядом с проектом или скачивается на этапе конфигурации:
#
#   file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake
#        "${CMAKE_BINARY_DIR}/graft.boot.cmake")
#   include("${CMAKE_BINARY_DIR}/graft.boot.cmake")
#   graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.1.0)
#
# После этого доступны graft::client, graft::tool и graft_plugin().
#
# graft_import(<имя> <url|каталог> [TAG <тег|ветка>] [CACHE <каталог>])
#
#   url        git-репозиторий ИЛИ путь к уже лежащему на диске исходнику
#   TAG        тег или ветка (по умолчанию main)
#   CACHE      где держать исходники (по умолчанию $ENV{GRAFT_CACHE} или ~/.graft)
#
# Кэш общий на машину, а не на проект: второй мод той же версией graft не качает и не
# собирает её заново. Локальный исходник можно подсунуть и снаружи, не трогая CMakeLists:
#
#   cmake -B build -DGRAFT_SOURCE_DIR=C:/src/graft
#
# Это же используют примеры внутри самого репозитория — им скачивать себя незачем.

include_guard(GLOBAL)
include(FetchContent)

set(GRAFT_SOURCE_DIR "" CACHE PATH "локальный исходник graft вместо скачивания")

function(graft_import name url)
    cmake_parse_arguments(G "" "TAG;CACHE" "" ${ARGN})
    if(NOT G_TAG)
        set(G_TAG "main")
    endif()
    if(NOT G_CACHE)
        if(DEFINED ENV{GRAFT_CACHE})
            set(G_CACHE "$ENV{GRAFT_CACHE}")
        elseif(DEFINED ENV{USERPROFILE})
            set(G_CACHE "$ENV{USERPROFILE}/.graft")
        else()
            set(G_CACHE "$ENV{HOME}/.graft")
        endif()
    endif()

    # Локальный исходник: свой каталог вместо скачивания. Так работают и примеры внутри
    # репозитория, и разработка самого graft — правка видна сразу, без релиза.
    set(local "${GRAFT_SOURCE_DIR}")
    if(NOT local AND IS_DIRECTORY "${url}")
        set(local "${url}")
    endif()
    if(local)
        if(NOT EXISTS "${local}/cmake/graft_plugin.cmake")
            message(FATAL_ERROR "graft_import: ${local} не похож на исходник graft")
        endif()
        message(STATUS "graft_import: ${name} <- ${local} (локальный исходник)")
        add_subdirectory("${local}" "${CMAKE_BINARY_DIR}/_${name}" EXCLUDE_FROM_ALL)
        return()
    endif()

    # Кэш общий на машину: каталог на (имя, тег). Одна и та же версия скачивается и
    # собирается один раз, дальше её берут все проекты.
    file(TO_CMAKE_PATH "${G_CACHE}" G_CACHE)
    set(FETCHCONTENT_BASE_DIR "${G_CACHE}/${name}-${G_TAG}" CACHE PATH "" FORCE)
    message(STATUS "graft_import: ${name} <- ${url}@${G_TAG} (кэш ${G_CACHE})")
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${url}"
        GIT_TAG        "${G_TAG}"
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(${name})
endfunction()
