@echo off
REM Build via clang-cl inside MSVC env (vcvars gives SDK + STL for clang-cl).
REM Usage: build.bat [deploy]  -- deploy copies dwmapi.dll and the plugins to the game dir.
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake --preset clang-release || exit /b 1
cmake --build --preset clang-release || exit /b 1

if /I "%~1"=="deploy" (
    copy /Y "build\clang-release\dwmapi.dll" "F:\SteamLibrary\steamapps\common\DayZ\dwmapi.dll" || exit /b 1
    REM Plugins too: host and plugin agree on GRAFT_ABI_VERSION, so a stale plugin is
    REM rejected wholesale and every native silently returns zero. Copying only the host
    REM once cost a full red suite -- never again.
    copy /Y "build\clang-release\*.grafted.dll" "F:\SteamLibrary\steamapps\common\DayZ\#GRAFTED\" || exit /b 1
    echo Deployed dwmapi.dll and plugins to game dir.
)
