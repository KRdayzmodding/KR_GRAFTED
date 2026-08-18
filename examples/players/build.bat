@echo off
REM Сборка примера. Usage: build.bat [deploy]
set GAME_DIR=F:\SteamLibrary\steamapps\common\DayZ
set PDRIVE_SCRIPTS=E:/DayZ/PDrive/scripts
REM graft.exe версии библиотеки, против которой собираемся: инструмент и библиотека
REM обязаны быть из одной сборки. Скопировал пример наружу - поправь путь.
set GRAFT_EXE=%~dp0..\..\build\clang-release\graft.exe

call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

REM Зеркало движкового API: генерится из скриптов игры, дефайны ему не нужны.
if exist "%PDRIVE_SCRIPTS%" (
    if not exist "%GRAFT_EXE%" echo build.bat: нет %GRAFT_EXE% - собери библиотеку && exit /b 1
    "%GRAFT_EXE%" apigen "%PDRIVE_SCRIPTS%" src\graft\dayz || exit /b 1
)

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build build || exit /b 1

if /I "%~1"=="deploy" (
    if not exist "%GAME_DIR%\#GRAFTED" mkdir "%GAME_DIR%\#GRAFTED"
    copy /Y "build\EXAMPLE_PLAYERS.grafted.dll" "%GAME_DIR%\#GRAFTED\" || exit /b 1
    echo Deployed EXAMPLE_PLAYERS.grafted.dll to %GAME_DIR%\#GRAFTED
)
