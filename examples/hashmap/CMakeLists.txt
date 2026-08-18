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
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(GRAFT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "корень KR_GRAFT")
if(NOT EXISTS "${GRAFT_ROOT}/cmake/graft_plugin.cmake")
    message(FATAL_ERROR "GRAFT_ROOT=${GRAFT_ROOT} не похож на корень KR_GRAFT")
endif()
add_subdirectory("${GRAFT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/graft")

set(EXAMPLE_MOD_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/mod/EXAMPLE_HASHMAP/scripts"
    CACHE PATH "каталог scripts мода, куда graft пишет объявления")

graft_plugin(example_hashmap
    NAME     EXAMPLE_HASHMAP
    VERSION  1
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_hashmap.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cpp"
    MOD_SCRIPTS "${EXAMPLE_MOD_SCRIPTS}")
