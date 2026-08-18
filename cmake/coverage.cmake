# Покрытие: прогнать тесты инструментированной сборки и свести отчёт.
# Запускается таргетом `coverage` (пресет coverage), а не руками.
#
# Ожидает: TESTS (graft_tests.exe), BUILD, SRC, PROFDATA, COV.

set(raw  "${BUILD}/graft.profraw")
set(data "${BUILD}/graft.profdata")

execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LLVM_PROFILE_FILE=${raw}" "${TESTS}"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PROFDATA}" merge -sparse "${raw}" -o "${data}"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${COV}" report "${TESTS}" -instr-profile=${data}
                        "${SRC}/INCLUDE" "${SRC}/SRC"
                COMMAND_ERROR_IS_FATAL ANY)

# Построчная раскраска — рядом, отдельным файлом: нужна не каждый раз, а когда цифра
# не сходится с ожиданиями.
execute_process(COMMAND "${COV}" show "${TESTS}" -instr-profile=${data}
                        -format=html -output-dir=${BUILD}/coverage
                        "${SRC}/INCLUDE" "${SRC}/SRC"
                COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "построчно: ${BUILD}/coverage/index.html")
