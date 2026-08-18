cmake_minimum_required(VERSION 3.30)
project(example_hashmap CXX C)

# Пример шаблонного скриптового класса: `CppHashMap<K,V>` в скрипте — обычный
# `std::unordered_map` в C++.
#
# От minimal отличается ровно одним: класс не привязывается по имени, а
# РАЗВОРАЧИВАЕТСЯ. Ты пишешь один шаблон обычными типами, библиотека инстанциирует его
# по всем поддерживаемым и на каждом вызове выбирает нужную инстанциацию по имени
# скриптового класса объекта ("CppHashMap<string,int>"). Своей рукой в моде для этого
# класса не написано ни строчки — объявления печатает генератор.
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

graft_plugin(example_hashmap
    NAME    EXAMPLE_HASHMAP
    VERSION 1
    SOURCES src/cpp_hashmap.cpp src/plugin.cpp
    MODULES 1_Core)
