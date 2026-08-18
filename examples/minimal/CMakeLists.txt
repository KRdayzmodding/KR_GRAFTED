cmake_minimum_required(VERSION 3.30)
project(example_graft CXX C)

# Шаблон своего плагина: несколько нативов на все поддерживаемые типы + PBO.
# Копируется целиком: правишь src/natives.cpp и mod/, остальное трогать не нужно.
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

# SCRIPTS_DIR: объявления печатаются ПРЯМО В МОД, а не рядом с DLL. Так пример
# читается целиком — открыл mod/ и видишь весь скриптовый код, включая тот, что
# написан не рукой. В своём проекте так делать не обязательно: по умолчанию они
# ложатся рядом с DLL, а в мод их забирает deploy.ps1 (см. examples/hello).
graft_plugin(example_graft
    NAME    EXAMPLE_GRAFT
    VERSION 1
    SOURCES src/natives.cpp src/plugin.cpp
    MODULES 1_Core
    SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod/EXAMPLE_GRAFT/scripts")
