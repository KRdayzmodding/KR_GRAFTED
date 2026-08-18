cmake_minimum_required(VERSION 3.30)
project(example_graft CXX C)

# Пример подключения библиотеки graft к своему моду.
# Копируется целиком: правишь src/natives.cpp и mod/, остальное трогать не нужно.
#
# Раньше здесь было шестьдесят строк: FetchContent MinHook, поимённый список исходников
# библиотеки, свой генератор объявлений, переименование результата в dwmapi.dll. Ничего
# этого больше нет — плагин собирается одним вызовом graft_plugin(), а хост (dwmapi.dll)
# ставится один раз на игру и никем не пересобирается.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # без зависимости от MSVC-рантайма
endif()

# ── Где лежит graft ───────────────────────────────────────────────────────────
# По умолчанию — на уровень выше (пока каталог лежит внутри репозитория). После
# копирования шаблона в другое место путь задаётся снаружи:
#   cmake -B build -DGRAFT_ROOT=E:/source/KR_GRAFT
set(GRAFT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "корень KR_GRAFT")
if(NOT EXISTS "${GRAFT_ROOT}/cmake/graft_plugin.cmake")
    message(FATAL_ERROR
        "GRAFT_ROOT=${GRAFT_ROOT} не похож на корень KR_GRAFT "
        "(нет cmake/graft_plugin.cmake). Передай путь: -DGRAFT_ROOT=<корень>")
endif()
# Клиентская часть библиотеки, генератор и функция graft_plugin(). Хост, MinHook, gtest
# и плагины самого репозитория сюда не приезжают — их CMakeLists отдаёт только при
# сборке самого себя.
add_subdirectory("${GRAFT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/graft")

# ── Куда генерить скриптовые объявления ──────────────────────────────────────
# Каталог scripts мода: генератор сам разложит объявления по модулям (1_Core / 3_Game),
# и файл получит имя по плагину — в одном моде может жить несколько плагинов.
set(EXAMPLE_MOD_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/mod/EXAMPLE_GRAFT/scripts"
    CACHE PATH "каталог scripts мода, куда graft пишет объявления")

# ── Сам плагин ───────────────────────────────────────────────────────────────
graft_plugin(example_graft
    NAME     EXAMPLE_GRAFT
    VERSION  1
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/natives.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cpp"
    MOD_SCRIPTS "${EXAMPLE_MOD_SCRIPTS}")
