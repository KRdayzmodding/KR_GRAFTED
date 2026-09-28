# Copyright (C) 2025-2026 6wingSerap
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Наложить заплатку на чужой исходник — идемпотентно и громко.
#
#   cmake -DPATCH=<файл.patch> -P apply-patch.cmake
#
# Рабочий каталог задаёт вызывающий: ExternalProject ставит его в каталог зависимости, и
# PATCH_COMMAND исполняется именно там.
#
# Идемпотентность нужна потому, что конфигурацию запускают повторно, а исходник к тому
# времени уже пропатчен: обратная проверка отвечает на вопрос «уже наложена?» точно, а не
# по приметам вроде наличия строки в файле.
#
# Провал НЕ проглатывается. Если апстрим тронул тот же кусок, сборка обязана встать здесь,
# с текстом конфликта, а не поехать дальше с наполовину применённой правкой.

cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED PATCH)
    message(FATAL_ERROR "apply-patch: не задан -DPATCH=<файл>")
endif()
if(NOT EXISTS "${PATCH}")
    message(FATAL_ERROR "apply-patch: заплатка не найдена: ${PATCH}")
endif()

find_package(Git REQUIRED)

# Сначала вернуть исходник зависимости к своему HEAD, и только потом накладывать.
#
# Это не педантизм, это проверено опытом. Без сброса подъём тега проходит МОЛЧА и мимо
# всех проверок: `git checkout` нового тега переносит уже наложенную правку на новый
# исходник как обычную локальную модификацию, обратная проверка после этого честно
# отвечает «уже наложена», и в сборке оказывается новая версия со старой заплаткой,
# слитой автоматически. Ровно то, ради чего заплатку и выбирали вместо копии файла,
# при этом теряется.
#
# После сброса состояние всегда одно и то же: чистый HEAD зависимости. Значит наложение
# либо проходит, либо падает с конфликтом, и третьего не дано.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" checkout -- .
    RESULT_VARIABLE dirty
    OUTPUT_QUIET ERROR_QUIET)
if(NOT dirty EQUAL 0)
    message(WARNING "apply-patch: не смог вернуть исходник зависимости к HEAD")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH}"
    RESULT_VARIABLE failed
    ERROR_VARIABLE why)
if(NOT failed EQUAL 0)
    message(FATAL_ERROR
        "apply-patch: заплатка НЕ наложилась.
"
        "  файл:    ${PATCH}
"
        "  причина: ${why}
"
        "Так бывает после подъёма версии зависимости: апстрим тронул тот же кусок.
"
        "Что делать — в SRC/patches/README.md.")
endif()
message(STATUS "apply-patch: наложена ${PATCH}")
