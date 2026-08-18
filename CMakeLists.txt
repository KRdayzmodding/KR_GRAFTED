cmake_minimum_required(VERSION 3.30)
project(KR_GRAFT CXX C)

# ── Стандарты ─────────────────────────────────────────────────────────────────
# Базовый — 23 (его CMake 3.31 умеет мапить на clang-cl и на зависимости gtest/minhook).
# Наши таргеты поднимаем до C++26 явным флагом ниже (seraph_std).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd / IDE

# C++26 для нашего кода: clang-cl не знает /std:c++26, прокидываем в clang-драйвер.
add_library(seraph_std INTERFACE)
target_compile_options(seraph_std INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>>:/clang:-std=c++2c>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-std=c++2c>
)

# ── Portable EXE: статический runtime, без зависимости от MSVC DLL ────────────
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ══════════════════════════════════════════════════════════════════════════════
#  graft: хост и клиент
#
#  Хост (dwmapi.dll) — один на игру: скан движка, хуки, script-контексты, загрузчик.
#  Клиент — то, что компилируется в КАЖДЫЙ плагин: трамплины, разбор аргументов,
#  арена строк и переходники к сервисам хоста.
#
#  Водораздел проведён по одному признаку: нужно ли состояние движка. Всё, чему не
#  нужно, живёт inline в заголовках и компилируется прямо в плагин — поэтому горячий
#  путь границу между модулями НЕ ПЕРЕСЕКАЕТ (движок зовёт трамплин плагина напрямую).
#  Сторожит это Perf_HotPathBudget в сьюте seraph::graft.
# ══════════════════════════════════════════════════════════════════════════════

# Общее для обеих сторон: зонд памяти, арена возвращаемых строк, логика загрузчика.
add_library(graft_common STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/text.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/guard.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/plugins.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/loader_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/frame_state.cpp"
)
target_include_directories(graft_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/INCLUDE")
target_link_libraries(graft_common PUBLIC seraph_std)

# Только хост: поиск точек движка, доступ к script-контекстам, журнал.
add_library(graft_host STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/scan.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/script_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/log_host.cpp"
)
target_link_libraries(graft_host PUBLIC graft_common)

# Только плагин: переходники к хосту и экспорт graft_plugin_entry.
add_library(graft_client STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/client.cpp"
)
target_link_libraries(graft_client PUBLIC graft_common)

# Генерация объявлений: нужна инструменту и тестам, в игре не нужна никому.
add_library(graft_gen STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/proto.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/enfparse.cpp"
)
target_link_libraries(graft_gen PUBLIC graft_common)

# ── Инструмент ───────────────────────────────────────────────────────────────
# Реестр самого хоста (нативы Graft*) слинкован сюда же: так `protogen --host` обходится
# без загрузки dwmapi.dll. Нужен и стороннему проекту — им генерятся его объявления.
add_executable(graft_tool
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/tool_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/host_natives.cpp"
)
target_link_libraries(graft_tool PRIVATE graft_gen graft_host)
set_target_properties(graft_tool PROPERTIES OUTPUT_NAME "graft")

# ── Как объявляется плагин ───────────────────────────────────────────────────
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/graft_plugin.cmake")

# ══════════════════════════════════════════════════════════════════════════════
#  Дальше — только когда этот файл собирают сам по себе.
#
#  Сторонний проект подключает нас через add_subdirectory ради graft_client и
#  graft_plugin(): ему не нужны ни хост, ни MinHook, ни gtest, ни наши плагины — и уж
#  точно не нужно, чтобы сборка его примера переписывала объявления в ЧУЖОМ моде.
# ══════════════════════════════════════════════════════════════════════════════
if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

# ── Хост ─────────────────────────────────────────────────────────────────────
include(FetchContent)
if(WIN32)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        v1.3.3)
    FetchContent_MakeAvailable(minhook)
    # v1.3.3 без корневого CMakeLists — компилируем его C-исходники прямо в DLL.
    add_library(SeraphGraft SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/dll/proxy.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/dll/intrinsics.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/engine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/loader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/frame.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC/graft/host_natives.cpp"
        "${minhook_SOURCE_DIR}/src/buffer.c"
        "${minhook_SOURCE_DIR}/src/hook.c"
        "${minhook_SOURCE_DIR}/src/trampoline.c"
        "${minhook_SOURCE_DIR}/src/hde/hde64.c"
    )
    target_link_libraries(SeraphGraft PRIVATE graft_host)
    target_include_directories(SeraphGraft PRIVATE "${minhook_SOURCE_DIR}/include")
    # Игра ищет dwmapi.dll рядом с exe -> так и называем, без префикса lib.
    set_target_properties(SeraphGraft PROPERTIES OUTPUT_NAME "dwmapi" PREFIX "")
endif()

# ── Тестовое окружение ───────────────────────────────────────────────────────
# Библиотека — это INCLUDE/graft и SRC. Всё ниже к ней не относится: это её тесты —
# два плагина и PBO со сьютой. Плагина именно два, и второй существует ради проверки:
# сьюта гоняет оба в одном процессе, поэтому «два плагина уживаются» проверяется
# обычным прогоном, а не отдельным ритуалом, который забудут запустить.
set(GRAFT_MOD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/mod/SIXW_GRAFT/scripts")

# Зеркало движкового API из скриптов игры. Генерится ДО сборки плагина, потому что тот
# им пользуется: целевой сценарий (собрать игроков, узнать steam id) написан против
# сгенерированных классов, а не против ref<"CGame"> вручную. Если PDrive нет — нет и
# зеркала, натив честно скажет об этом (см. __has_include в natives.cpp).
set(GRAFT_VANILLA_SCRIPTS "E:/DayZ/PDrive/scripts" CACHE PATH "скрипты игры для apigen")
set(GRAFT_API_DIR "${CMAKE_BINARY_DIR}/gen")

# Зеркало движкового API. Дефайны игры ему НЕ нужны: он печатает объединение всех веток
# препроцессора, а чего в этой сборке нет — то промахнётся в рантайме со строкой в
# журнале. Узнать дефайны запущенной игры всё равно неоткуда.
if(EXISTS "${GRAFT_VANILLA_SCRIPTS}")
    add_custom_command(
        OUTPUT "${GRAFT_API_DIR}/graft/dayz/3_Game.hpp"
        COMMAND "$<TARGET_FILE:graft_tool>" apigen "${GRAFT_VANILLA_SCRIPTS}"
                "${GRAFT_API_DIR}/graft/dayz"
        DEPENDS graft_tool
        COMMENT "graft apigen -> ${GRAFT_API_DIR}/graft/dayz"
        VERBATIM)
    add_custom_target(graft_api DEPENDS "${GRAFT_API_DIR}/graft/dayz/3_Game.hpp")
endif()

graft_plugin(sixw_graft
    NAME     SIXW_GRAFT
    VERSION  1
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/natives.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/ping.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/plugin.cpp"
    MOD_SCRIPTS "${GRAFT_MOD_SCRIPTS}")
target_include_directories(sixw_graft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins")
if(TARGET graft_api)
    add_dependencies(sixw_graft graft_api)
    target_include_directories(sixw_graft PRIVATE "${GRAFT_API_DIR}")
endif()

# Второй плагин — это пример examples/hashmap, собранный здесь же. Копии исходника нет:
# расходиться нечему. Он существует ради проверки, а не ради удобства — сьюта гоняет два
# плагина в одном процессе, поэтому «два плагина уживаются» краснеет обычным прогоном.
graft_plugin(example_hashmap
    NAME     EXAMPLE_HASHMAP
    VERSION  1
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/examples/hashmap/src/cpp_hashmap.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/examples/hashmap/src/plugin.cpp"
    MOD_SCRIPTS "${GRAFT_MOD_SCRIPTS}")

# Объявления класса Graft (лицо менеджера) — из реестра самого хоста.
if(EXISTS "${GRAFT_MOD_SCRIPTS}")
    add_custom_command(TARGET graft_tool POST_BUILD
        COMMAND "$<TARGET_FILE:graft_tool>" protogen --host "${GRAFT_MOD_SCRIPTS}"
        COMMENT "graft protogen --host -> ${GRAFT_MOD_SCRIPTS}"
        VERBATIM)
endif()

# Нативы примера в реестр этого мода не идут, но компилировать их надо: иначе правка
# публичного API ломает пример молча. Объектник никуда не линкуется — он сторож сборки.
add_library(example_syntax OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/minimal/src/natives.cpp")
target_link_libraries(example_syntax PRIVATE graft_client)

# Второй пример — обратного направления. Ему нужно зеркало движкового API, поэтому он
# сторожит заодно и его: сломается что-то в сгенерированных заголовках, и пример,
# написанный как пишет пользователь, перестанет собираться.
# Зеркало движкового API примеру нужно ВСЕГДА — он написан против настоящих классов
# игры, а не против ref<"CGame"> вручную. Нет PDrive, нет зеркала — нечего и сторожить.
if(TARGET graft_api)
    add_library(example_players_syntax OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/players/src/main.cpp")
    target_link_libraries(example_players_syntax PRIVATE graft_client)
    add_dependencies(example_players_syntax graft_api)
    target_include_directories(example_players_syntax PRIVATE "${GRAFT_API_DIR}")
endif()

# ── GoogleTest (TDD домена) ──────────────────────────────────────────────────
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2)
# проект собирается со статическим CRT (см. CMAKE_MSVC_RUNTIME_LIBRARY) — gtest тоже
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
enable_testing()
add_executable(seraph_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/ping.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/ping_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/scan_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/proto_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/types_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/callout_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/enfparse_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_test.cpp"
)
target_link_libraries(seraph_tests PRIVATE graft_host graft_gen GTest::gtest_main)
target_include_directories(seraph_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
include(GoogleTest)
gtest_discover_tests(seraph_tests)
