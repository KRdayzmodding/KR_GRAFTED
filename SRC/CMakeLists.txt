# ── graft: библиотека, инструмент, хост ──────────────────────────────────────
#
#  Хост (hid.dll) — один на игру: скан движка, хуки, script-контексты, загрузчик.
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
    graft/scan.cpp
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
# обходится без загрузки hid.dll.
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

# ── Хост: hid.dll ────────────────────────────────────────────────────────────
# Собирается только когда graft собирают сам по себе: чужому проекту нужен клиент и
# инструмент, а хост он ставит один раз готовым.
if(NOT GRAFT_BUILD_HOST)
    return()
endif()

include(FetchContent)

# Zydis — дизассемблер, на котором стоит разбор пролога у safetyhook. Знает набор команд
# целиком, включая VEX и EVEX; HDE64 внутри MinHook, стоявший здесь раньше, на таком
# прологе честно отказывал, и это был потолок старой механики.
# Из Zydis нужен ТОЛЬКО декодер: safetyhook меряет длину инструкции и смотрит её
# операнды, но ничего не печатает и не собирает обратно. В hid.dll форматтер с энкодером
# и так не попадают — Zydis статический, и линкер не тянет из него объектники, на
# которые никто не ссылается (проверено: размер hid.dll от этих двух строк не меняется).
# Выключены они ради времени сборки: десять файлов с большими таблицами меньше.
#
# ZYDIS_MINIMAL_MODE здесь НЕ годится: он выбрасывает как раз операнды и категорию
# перехода, по которым safetyhook и решает, что переносить в трамплин.
set(ZYDIS_FEATURE_ENCODER   OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN  OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        v4.1.0
    GIT_SUBMODULES_RECURSE ON
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(zydis)

# safetyhook: тянем ТОЛЬКО исходники и компилируем нужные сами. SOURCE_SUBDIR указан в
# несуществующий каталог, поэтому его CMakeLists не выполняется — иначе он потащил бы
# свой Zydis через CPM, а он у нас уже собран выше со своими настройками.
#
# PATCH_COMMAND: одна правка в его слое ОС, см. patches/README.md. Заплаткой, а не копией
# файла в дереве, ровно ради обновляемости — подъём тега, тронувший тот же кусок, встанет
# здесь с текстом конфликта, а не проедет молча.
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG        v0.7.0
    SOURCE_SUBDIR  do-not-configure
    PATCH_COMMAND  "${CMAKE_COMMAND}"
                   "-DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/safetyhook-trap-lifetime.patch"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply-patch.cmake"
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(safetyhook)

# Врезка в чужой код — своя цель, а не кусок hid.dll. Её линкует не только хост, но и
# сьюта: иначе graft::hook проверялся бы только выездом на стенд. Механика врезки в
# процессе при этом остаётся одна — целей, которые её тянут, ровно две, и в одном
# процессе они не встречаются.
#
# Из safetyhook берём четыре файла из восьми: mid_hook, vmt_hook, easy и слой Linux не
# нужны никому, а в hid.dll ехали бы. Правок в них нет ни одной.
add_library(graft_hook STATIC
    graft/hook.cpp
    "${safetyhook_SOURCE_DIR}/src/allocator.cpp"
    "${safetyhook_SOURCE_DIR}/src/inline_hook.cpp"
    "${safetyhook_SOURCE_DIR}/src/os.windows.cpp"
    "${safetyhook_SOURCE_DIR}/src/utility.cpp"
)
target_link_libraries(graft_hook PUBLIC graft_common PRIVATE Zydis)
target_include_directories(graft_hook PRIVATE "${safetyhook_SOURCE_DIR}/include")
add_library(graft::hook ALIAS graft_hook)
set_target_properties(graft_hook PROPERTIES FOLDER graft)

add_library(graft_proxy SHARED
    dll/proxy.cpp
    graft/engine.cpp
    graft/battleye.cpp
    graft/loader.cpp
    graft/frame.cpp
    graft/defines.cpp
    graft/host_natives.cpp
)
target_link_libraries(graft_proxy PRIVATE graft_host graft_hook)
# Игра ищет hid.dll рядом с exe -> так и называем, без префикса lib.
set_target_properties(graft_proxy PROPERTIES
    OUTPUT_NAME "hid" PREFIX "" FOLDER graft
    RUNTIME_OUTPUT_DIRECTORY "${graft_bin}")
add_library(graft::proxy ALIAS graft_proxy)
