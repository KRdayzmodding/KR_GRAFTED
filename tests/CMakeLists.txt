# ── Тесты ────────────────────────────────────────────────────────────────────
# Библиотека — это INCLUDE/ и SRC/. Всё здесь только проверяет её: подключается из
# корня по GRAFT_BUILD_TESTS и только когда репозиторий собирают сам по себе.
#
# В ALL не входит ничего: обычная сборка — это graft. Тесты собираются таргетом tests.
#
#   cmake --build build --target tests
#   ctest --preset release
#
# На examples/ отсюда не смотрит НИЧЕГО. Раньше вторым плагином собирался пример
# hashmap: правка примера роняла сьюту, а правка сьюты требовала лезть в пример.
# Теперь у тестов свой второй плагин — та же идея (шаблонный класс, состояние в C++),
# но свой класс SeraphHashMap и своё имя, поэтому мод примеров и мод тестов уживаются
# в одной игре: имена скриптовых классов у движка глобальны на всю установку.

# Всё тестовое кладётся в свой каталог — out/<конфиг>/tests, ни с чем не перемешиваясь.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${GRAFT_OUT}/tests")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${GRAFT_OUT}/tests")

# Скриптовая половина тестов лежит здесь же, в репозитории: рукописная сьюта в
# mod/SIXW_GRAFT. Объявления нативов туда НЕ пишутся — их печатает сборка рядом с
# плагинами (bin/<ИМЯ>.scripts), а забирает их в мод человек — как в любом моде.

# ── Плагины-фикстуры ─────────────────────────────────────────────────────────
# Плагина именно два, и второй существует ради проверки: сьюта гоняет оба в одном
# процессе, поэтому «два плагина уживаются» краснеет обычным прогоном, а не отдельным
# ритуалом, который забудут запустить.
graft_plugin(sixw_graft
    NAME    SIXW_GRAFT
    VERSION 1
    SOURCES plugins/natives.cpp plugins/ping.cpp plugins/plugin.cpp
    MODULES 1_Core 3_Game)
target_include_directories(sixw_graft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugins")

# Целевой сценарий фикстуры написан против настоящих классов игры. Зеркало готовится
# отдельно (`graft apigen P:/scripts <куда>`) и подключается путём; нет его — натив
# честно скажет об этом (см. __has_include в plugins/natives.cpp).
if(GRAFT_API_DIR)
    target_include_directories(sixw_graft PRIVATE "${GRAFT_API_DIR}")
endif()

graft_plugin(sixw_hashmap
    NAME    SIXW_HASHMAP
    VERSION 1
    SOURCES plugins/hashmap.cpp
    MODULES 1_Core)

# Объявления класса Graft (лицо менеджера) — из реестра самого хоста, а не из плагина.
# Ложатся туда же, где объявления плагинов: рядом с бинарями, каталогом на источник.
add_custom_target(graft_host_protos
    COMMAND "$<TARGET_FILE:graft::tool>" protogen --host "${GRAFT_OUT}/tests/graft.scripts"
    DEPENDS graft_tool
    COMMENT "graft protogen --host -> tests/graft.scripts"
    VERBATIM)

# ── Юнит-тесты ───────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    EXCLUDE_FROM_ALL)
# проект собирается со статическим CRT (см. CMAKE_MSVC_RUNTIME_LIBRARY) — gtest тоже
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(graft_tests
    plugins/ping.cpp
    ping_test.cpp
    scan_test.cpp
    proto_test.cpp
    types_test.cpp
    plugins_test.cpp
    callout_test.cpp
    enfparse_test.cpp
    runtime_test.cpp
)
target_link_libraries(graft_tests PRIVATE graft::host graft::gen GTest::gtest_main)
target_include_directories(graft_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(graft_tests)

# Один таргет на всё тестовое: фикстуры, объявления хоста и сами юнит-тесты.
add_custom_target(tests DEPENDS graft_tests sixw_graft sixw_hashmap graft_host_protos)
set_target_properties(graft_tests sixw_graft sixw_hashmap graft_host_protos tests
                      PROPERTIES FOLDER tests)

# ── Покрытие ─────────────────────────────────────────────────────────────────
# Включается пресетом coverage; llvm-cov и llvm-profdata лежат рядом с clang.
if(GRAFT_COVERAGE)
    find_program(LLVM_PROFDATA llvm-profdata REQUIRED)
    find_program(LLVM_COV      llvm-cov      REQUIRED)
    add_custom_target(coverage
        COMMAND "${CMAKE_COMMAND}"
                -DTESTS=$<TARGET_FILE:graft_tests> -DBUILD=${CMAKE_BINARY_DIR}
                -DSRC=${CMAKE_SOURCE_DIR} -DPROFDATA=${LLVM_PROFDATA} -DCOV=${LLVM_COV}
                -P "${CMAKE_SOURCE_DIR}/cmake/coverage.cmake"
        DEPENDS graft_tests
        COMMENT "покрытие по INCLUDE/ и SRC/"
        VERBATIM)
endif()
