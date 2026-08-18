@echo off
REM Сборка примера. Usage: build.bat [deploy]
REM deploy кладёт плагин в <игра>\graft\ — путь ниже поправь под себя.
REM Хост (dwmapi.dll) ставится ОДИН РАЗ и здесь не пересобирается:
REM   graft.exe install %GAME_DIR%
set GAME_DIR=F:\SteamLibrary\steamapps\common\DayZ

call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build build || exit /b 1

if /I "%~1"=="deploy" (
    if not exist "%GAME_DIR%\#GRAFTED" mkdir "%GAME_DIR%\#GRAFTED"
    copy /Y "build\EXAMPLE_HASHMAP.grafted.dll" "%GAME_DIR%\#GRAFTED\" || exit /b 1
    echo Deployed EXAMPLE_HASHMAP.grafted.dll to %GAME_DIR%\#GRAFTED
)
