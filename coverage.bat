@echo off
REM Покрытие юнит-тестов. Usage: coverage.bat [report|show]
REM
REM Меряется только ОФФЛАЙННАЯ половина: то, что исполняется в этом процессе. Трамплины,
REM доступ к script-контекстам и арена в игре зовутся ДВИЖКОМ, и никакой llvm-cov их не
REM увидит — их проверяет сьюта seraph::graft.
set LLVM=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\Llvm\x64\bin
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cmake -B build/cov -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_CXX_FLAGS="/clang:-fprofile-instr-generate /clang:-fcoverage-mapping" ^
  -DCMAKE_C_FLAGS="/clang:-fprofile-instr-generate /clang:-fcoverage-mapping" ^
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" || exit /b 1
cmake --build build/cov --target seraph_tests || exit /b 1

set LLVM_PROFILE_FILE=%CD%\build\cov\graft.profraw
build\cov\seraph_tests.exe || exit /b 1
"%LLVM%\llvm-profdata.exe" merge -sparse build\cov\graft.profraw -o build\cov\graft.profdata || exit /b 1

if /I "%~1"=="show" (
    "%LLVM%\llvm-cov.exe" show build\cov\seraph_tests.exe -instr-profile=build\cov\graft.profdata ^
        -format=html -output-dir=build\cov\html INCLUDE SRC || exit /b 1
    echo build\cov\html\index.html
) else (
    "%LLVM%\llvm-cov.exe" report build\cov\seraph_tests.exe -instr-profile=build\cov\graft.profdata ^
        INCLUDE SRC || exit /b 1
)
