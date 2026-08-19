cmake_minimum_required(VERSION 3.30)
project(hello_graft CXX C)

# Самый маленький мод на graft: одна функция в C++, один вызов в скрипте.
# Весь проект — этот файл, src/hello.cpp и папка мода. Больше ничего нет.
#
# Сборка печатает в мод готовые объявления для PBO (scripts/3_Game). Собрать PBO и
# положить в игру — твоё дело: graft мод не собирает, см. deploy.ps1.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # Статический рантайм: у игроков MSVC-редиста нет. Форма с $<CONFIG:Debug> обязательна —
    # graft наследует ЭТО значение, и обе половины должны совпасть (иначе линкер ругнётся
    # на _ITERATOR_DEBUG_LEVEL).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── graft ────────────────────────────────────────────────────────────────────
# Бут-файл втягивает библиотеку одной строкой: качает (или берёт локальный исходник,
# если задан -DGRAFT_SOURCE_DIR) и даёт graft::client и graft_plugin().
set(GRAFT_BOOT_FILE "${CMAKE_BINARY_DIR}/graft.boot.cmake" CACHE FILEPATH "graft.boot.cmake")
if(NOT EXISTS "${GRAFT_BOOT_FILE}")
    file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake "${GRAFT_BOOT_FILE}")
endif()
include("${GRAFT_BOOT_FILE}")
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG main)

# SCRIPTS_DIR: объявления печатаются ПРЯМО В МОД, а не рядом с DLL. Иначе они живут
# только в каталоге сборки — а он чистится, и мод остаётся без объявлений, с которыми
# PBO не собирается. Свой проект волен хранить их где угодно (см. deploy.ps1).
graft_plugin(hello
    NAME    HELLO_GRAFT
    VERSION 1
    SOURCES src/hello.cpp
    MODULES 3_Game
    SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod/HELLO_GRAFT/scripts")
