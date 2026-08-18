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
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(GRAFT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "корень KR_GRAFT")
if(NOT EXISTS "${GRAFT_ROOT}/cmake/graft_plugin.cmake")
    message(FATAL_ERROR "GRAFT_ROOT=${GRAFT_ROOT} не похож на корень KR_GRAFT")
endif()
add_subdirectory("${GRAFT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/graft")

# ── Зеркало движкового API ───────────────────────────────────────────────────
# Генерится из скриптов игры один раз (и заново после патча):
#   graft apigen E:/DayZ/PDrive/scripts src/graft/dayz
#
# Дефайны игры ему НЕ нужны: он печатает объединение всех веток препроцессора, а чего
# в конкретной сборке нет — то промахнётся в рантайме со строкой в журнале.
set(EXAMPLE_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "куда сгенерено зеркало")

set(EXAMPLE_MOD_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/mod/EXAMPLE_PLAYERS/scripts"
    CACHE PATH "каталог scripts мода")

graft_plugin(example_players
    NAME     EXAMPLE_PLAYERS
    VERSION  1
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cpp"
    MOD_SCRIPTS "${EXAMPLE_MOD_SCRIPTS}")
target_include_directories(example_players PRIVATE "${EXAMPLE_API_DIR}")
