cmake_minimum_required(VERSION 3.30)
project(example_players CXX C)

# Пример обратного направления: C++ сам ходит в игру.
#
# От minimal отличается двумя вещами: тиком (точка входа для нашего кода) и ЗЕРКАЛОМ
# движкового API — сгенерированными заголовками, в которых движковые классы выглядят
# обычным C++.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # без зависимости от MSVC-рантайма
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

# ── Зеркало движкового API ───────────────────────────────────────────────────
# Генерится из скриптов игры один раз (и заново после патча) ОТДЕЛЬНОЙ командой —
# сборка скрипты игры не ищет:
#
#   graft apigen P:/scripts src/graft/dayz
#
# Дефайны игры ему НЕ нужны: он печатает объединение всех веток препроцессора, а чего
# в конкретной сборке нет — то промахнётся в рантайме со строкой в журнале.
set(GRAFT_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "готовое зеркало движкового API")
if(NOT EXISTS "${GRAFT_API_DIR}/graft/dayz/3_Game.hpp")
    message(FATAL_ERROR
        "нет зеркала движкового API в ${GRAFT_API_DIR}. Сделай его один раз:\n"
        "  graft apigen <скрипты игры> <куда>/graft/dayz\n"
        "и передай путь: -DGRAFT_API_DIR=<куда>")
endif()

graft_plugin(example_players
    NAME    EXAMPLE_PLAYERS
    VERSION 1
    SOURCES src/main.cpp src/plugin.cpp
    MODULES 1_Core)
target_include_directories(example_players PRIVATE "${GRAFT_API_DIR}")
