# ── graft: библиотека, инструмент, хост ──────────────────────────────────────
#
#  Хост (dwmapi.dll) — один на игру: скан движка, хуки, script-контексты, загрузчик.
#  Клиент — то, что компилируется в КАЖДЫЙ плагин: трамплины, разбор аргументов,
#  арена строк и переходники к сервисам хоста.
#
#  Водораздел проведён по одному признаку: нужно ли состояние движка. Всё, чему не
#  нужно, живёт inline в заголовках и компилируется прямо в плагин — поэтому горячий
#  путь границу между модулями НЕ ПЕРЕСЕКАЕТ (движок зовёт трамплин плагина напрямую).
#
#  Это единственный каталог, который приезжает в чужой проект: тесты и примеры
#  подключаются только когда graft собирают сам по себе.

# Общее для обеих сторон: зонд памяти, арена возвращаемых строк, логика загрузчика.
add_library(graft_common STATIC
    graft/probe.cpp
    graft/text.cpp
    graft/guard.cpp
    graft/plugins.cpp
    graft/loader_state.cpp
    graft/frame_state.cpp
    graft/log_script.cpp
)
target_include_directories(graft_common PUBLIC "${PROJECT_SOURCE_DIR}/INCLUDE")
# Версия проекта — одна на всех: ею представляется шапка в журналах.
target_compile_definitions(graft_common PUBLIC GRAFT_VERSION="${PROJECT_VERSION}")
target_link_libraries(graft_common PUBLIC graft_std)

# Только хост: поиск точек движка, доступ к script-контекстам, журнал.
add_library(graft_host STATIC
    graft/scan.cpp
    graft/script_host.cpp
    graft/objects.cpp
    graft/log_host.cpp
    graft/banner.cpp
    graft/stages.cpp
)
target_link_libraries(graft_host PUBLIC graft_common)

# Только плагин: переходники к хосту и экспорт graft_plugin_entry.
add_library(graft_client STATIC graft/client.cpp)
target_link_libraries(graft_client PUBLIC graft_common)

# Генерация объявлений: нужна инструменту и тестам, в игре не нужна никому.
add_library(graft_gen STATIC
    graft/proto.cpp
    graft/enfparse.cpp
)
target_link_libraries(graft_gen PUBLIC graft_common)

# Реестр самого хоста (нативы Graft*) слинкован в инструмент: так `protogen --host`
# обходится без загрузки dwmapi.dll.
add_executable(graft_tool graft/tool_main.cpp graft/host_natives.cpp)
target_link_libraries(graft_tool PRIVATE graft_gen graft_host)
set_target_properties(graft_tool PROPERTIES OUTPUT_NAME "graft")

# Куда кладём результат. У себя — в out/<конфиг>/graft; в чужом проекте — в его каталог
# сборки, чтобы graft.exe не мешался среди артефактов мода.
if(GRAFT_OUT)
    set(graft_bin "${GRAFT_OUT}/graft")
else()
    set(graft_bin "${CMAKE_BINARY_DIR}/graft")
endif()
set_target_properties(graft_tool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${graft_bin}")

add_library(graft::client ALIAS graft_client)
add_library(graft::host   ALIAS graft_host)
add_library(graft::gen    ALIAS graft_gen)
add_executable(graft::tool ALIAS graft_tool)

set_target_properties(graft_common graft_host graft_client graft_gen graft_tool
                      PROPERTIES FOLDER graft)

# ── Хост: dwmapi.dll ─────────────────────────────────────────────────────────
# Собирается только когда graft собирают сам по себе: чужому проекту нужен клиент и
# инструмент, а хост он ставит один раз готовым.
if(NOT GRAFT_BUILD_HOST)
    return()
endif()

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(minhook)

# v1.3.3 без корневого CMakeLists — компилируем его C-исходники прямо в DLL.
add_library(graft_proxy SHARED
    dll/proxy.cpp
    dll/intrinsics.c
    graft/engine.cpp
    graft/loader.cpp
    graft/frame.cpp
    graft/host_natives.cpp
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
)
target_link_libraries(graft_proxy PRIVATE graft_host)
target_include_directories(graft_proxy PRIVATE "${minhook_SOURCE_DIR}/include")
# Игра ищет dwmapi.dll рядом с exe -> так и называем, без префикса lib.
set_target_properties(graft_proxy PROPERTIES
    OUTPUT_NAME "dwmapi" PREFIX "" FOLDER graft
    RUNTIME_OUTPUT_DIRECTORY "${graft_bin}")
add_library(graft::proxy ALIAS graft_proxy)
