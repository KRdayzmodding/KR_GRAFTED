# ── Примеры ──────────────────────────────────────────────────────────────────
# Каждый пример — САМОСТОЯТЕЛЬНЫЙ проект, и собирается именно так: отдельным
# конфигурированием, как его собирает пользователь. add_subdirectory здесь не подходит:
# тогда «пример собирается сам по себе» перестало бы проверяться, а именно это здесь и
# проверяется — правка публичного API, ломающая пример, красит сборку.
#
# В ALL примеры не входят: собираются таргетом examples, когда их спросили.
#
#   cmake --build build --target examples

include(ExternalProject)

# players написан против настоящих классов игры, поэтому ему нужно готовое зеркало
# (graft apigen). Нет зеркала — пример не собираем, а не роняем сборку.
set(examples hello minimal hashmap)
if(EXISTS "${GRAFT_API_DIR}/graft/dayz/3_Game.hpp")
    list(APPEND examples players)
else()
    message(STATUS "graft: пример players пропущен — нет зеркала API (graft apigen)")
endif()

foreach(example IN LISTS examples)
    ExternalProject_Add(example_${example}
        SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/${example}"
        BINARY_DIR   "${CMAKE_BINARY_DIR}/examples/${example}"
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE          # свой проект — своя логика зависимостей, доверяем ей
        DEPENDS      graft_tool
        CMAKE_ARGS   -DCMAKE_BUILD_TYPE=$<CONFIG>
                     -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${GRAFT_OUT}/examples/${example}
                     -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${GRAFT_OUT}/examples/${example}
                     -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/examples/${example}/lib
                     -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                     -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DGRAFT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                     -DGRAFT_BOOT_FILE=${PROJECT_SOURCE_DIR}/cmake/graft.boot.cmake
                     -DGRAFT_API_DIR=${GRAFT_API_DIR})
    set_target_properties(example_${example} PROPERTIES FOLDER examples)
    list(APPEND example_targets example_${example})
endforeach()

# Один таргет на все примеры.
add_custom_target(examples DEPENDS ${example_targets})
set_target_properties(examples PROPERTIES FOLDER examples)
