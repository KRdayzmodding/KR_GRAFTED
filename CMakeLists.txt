cmake_minimum_required(VERSION 3.30)
project(graft VERSION 0.1.0 LANGUAGES CXX C)

# ══════════════════════════════════════════════════════════════════════════════
#  graft — прививка C++ к движку Enfusion.
#
#  Три части, и каждая собирается отдельно:
#
#    cmake --build build                     graft: библиотека, graft.exe, dwmapi.dll
#    cmake --build build --target tests      + фикстуры и юнит-тесты
#    cmake --build build --target examples   + примеры, каждый своим проектом
#
#  По умолчанию собирается ТОЛЬКО graft: тесты и примеры в ALL не входят и на обычной
#  сборке не трогаются вовсе.
#
#  Сборка плагина даёт DLL и каталог <ИМЯ>.scripts рядом с ней — объявления для PBO,
#  разложенные по модулям Enforce. PBO и папки @МОД проект не собирает: их делает
#  мододел своим обычным инструментом.
#
#  Таргеты библиотеки: graft::client (линкуется в плагин), graft::host, graft::gen,
#  graft::tool (graft.exe), graft::proxy (dwmapi.dll).
# ══════════════════════════════════════════════════════════════════════════════

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd / IDE
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Артефакты и промежуточное разведены: в build/ — кэш, объектники и .lib, в out/ —
# только то, что забирают руками, разложенное по частям проекта:
#
#   out/<конфиг>/graft/              dwmapi.dll и graft.exe
#   out/<конфиг>/tests/              фикстуры, их объявления, graft_tests.exe
#   out/<конфиг>/examples/<пример>/  плагин примера и его объявления
#
# Конфигурация — в имени каталога, поэтому debug и release лежат рядом и не дерутся.
#
# ВСЁ ЭТО — только когда graft собирают сам по себе. В чужом проекте у нас нет никаких
# прав на его дерево: ни складывать graft.exe в его исходники, ни двигать его .lib.
# Там graft.exe уезжает в каталог сборки потребителя (см. SRC/CMakeLists.txt).
if(PROJECT_IS_TOP_LEVEL)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" GRAFT_CONFIG)
    if(NOT GRAFT_CONFIG)
        set(GRAFT_CONFIG "release")
    endif()
    # Не CACHE: иначе смена CMAKE_BUILD_TYPE в том же каталоге сборки не переезжает и
    # release-артефакты продолжают падать в out/debug.
    set(GRAFT_OUT "${CMAKE_SOURCE_DIR}/out/${GRAFT_CONFIG}")

    # Импортные библиотеки, .exp и статические .lib — мусор сборки, в out/ им нечего делать.
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

# C++26 для нашего кода: clang-cl не знает /std:c++26, прокидываем в clang-драйвер.
add_library(graft_std INTERFACE)
target_compile_options(graft_std INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>>:/clang:-std=c++2c>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-std=c++2c>
)

# Статический runtime: и хост, и плагины едут к чужим людям, MSVC-редиста у них нет.
#
# Только для своей сборки: в чужом проекте выбор рантайма — его дело, а наши статические
# библиотеки должны совпасть с ЕГО выбором, иначе линковка падает на _ITERATOR_DEBUG_LEVEL.
# Ничего не задав, мы наследуем его CMAKE_MSVC_RUNTIME_LIBRARY — и совпадаем всегда.
if(MSVC AND PROJECT_IS_TOP_LEVEL)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(GRAFT_COVERAGE "инструментировать сборку для llvm-cov" OFF)
if(GRAFT_COVERAGE)
    add_compile_options(/clang:-fprofile-instr-generate /clang:-fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

# Хост нужен только тому, кто собирает graft сам по себе; чужому проекту — нет.
set(GRAFT_BUILD_HOST ${PROJECT_IS_TOP_LEVEL})
add_subdirectory(SRC)

# Как объявляется плагин — нужно и чужому проекту, поэтому до выхода ниже.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/graft_plugin.cmake")

if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

option(GRAFT_BUILD_TESTS    "подключить тесты (таргет tests)"      ON)
option(GRAFT_BUILD_EXAMPLES "подключить примеры (таргет examples)" ON)

# Зеркало движкового API генерится ОТДЕЛЬНОЙ командой и живёт где угодно:
#   graft apigen P:/scripts <каталог>
# Здесь только путь к готовому — искать скрипты игры сборка не должна.
set(GRAFT_API_DIR "" CACHE PATH "готовое зеркало движкового API (graft apigen)")

# EXCLUDE_FROM_ALL у обоих: обычная сборка — это graft и только graft. Тесты и примеры
# собираются, когда их спросили, таргетами tests и examples.
if(GRAFT_BUILD_TESTS)
    enable_testing()          # в корне: иначе ctest не увидит сьюту из tests/
    add_subdirectory(tests EXCLUDE_FROM_ALL)
endif()
if(GRAFT_BUILD_EXAMPLES)
    add_subdirectory(examples EXCLUDE_FROM_ALL)
endif()

# ── Хост в игре ──────────────────────────────────────────────────────────────
# Единственное, что graft ставит и снимает, — это себя: dwmapi.dll рядом с exe. Моды
# (PBO, папки @МОД) он не собирает и не трогает — это дело мододела.
add_custom_target(mount
    COMMAND "$<TARGET_FILE:graft_tool>" install "${GRAFT_GAME_DIR}"
    COMMENT "хост -> ${GRAFT_GAME_DIR}"
    VERBATIM)
add_dependencies(mount graft_proxy graft_tool)

add_custom_target(unmount
    COMMAND "$<TARGET_FILE:graft_tool>" uninstall "${GRAFT_GAME_DIR}"
    COMMENT "снять хост из ${GRAFT_GAME_DIR}"
    VERBATIM)
add_dependencies(unmount graft_tool)

set_target_properties(mount unmount PROPERTIES FOLDER graft)
