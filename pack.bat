@echo off
REM Собирает хост, инструмент и все примеры и раскладывает их в out\ - стенд, который
REM повторяет каталог игры один в один: хост в корне, плагины в #GRAFTED\. Поэтому
REM проверка в конце не своя: стенд прогоняется через `graft doctor`, то есть тем же
REM кодом и тем же вызовом, что и настоящая установка.
REM
REM Usage: pack.bat [deploy]
REM   без аргумента - только собрать и проверить, в игру не ставится ничего
REM   deploy        - плюс поставить хост и плагины в игру
setlocal
set ROOT=%~dp0
set BUILD=%ROOT%build\clang-release
set OUT=%ROOT%out
set GAME_DIR=F:\SteamLibrary\steamapps\common\DayZ
set PDRIVE_SCRIPTS=E:/DayZ/PDrive/scripts

call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

REM -- 1. Хост (dwmapi.dll), инструмент (graft.exe) и плагины репозитория --------
cmake --preset clang-release || exit /b 1
cmake --build --preset clang-release || exit /b 1

REM -- 2. Зеркало движкового API - оно нужно примеру players --------------------
REM Генерим ТОЛЬКО ЧТО собранным graft.exe: ставить хост в игру ради сборки примера
REM незачем, а версия инструмента обязана совпадать с версией библиотеки.
if exist "%PDRIVE_SCRIPTS%" (
    "%BUILD%\graft.exe" apigen "%PDRIVE_SCRIPTS%" "%ROOT%examples\players\src\graft\dayz" >nul || exit /b 1
)

REM -- 3. Примеры - каждый своим проектом, как их собирает пользователь ----------
for %%E in (minimal hashmap players) do (
    pushd "%ROOT%examples\%%E" || exit /b 1
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
          -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl >nul || exit /b 1
    cmake --build build || exit /b 1
    popd
)

REM -- 4. Стенд -----------------------------------------------------------------
REM #GRAFTED - хранилище плагинов, и в нём нет ничего кроме них. Ни инструмента, ни
REM доков, ни PBO-исходников: хост открывает оттуда КАЖДЫЙ .dll, и лишнему файлу там
REM делать нечего.
if exist "%OUT%" rmdir /S /Q "%OUT%"
mkdir "%OUT%\#GRAFTED" || exit /b 1
copy /Y "%BUILD%\dwmapi.dll" "%OUT%\" >nul || exit /b 1
for %%E in (minimal hashmap players) do (
    copy /Y "%ROOT%examples\%%E\build\*.grafted.dll" "%OUT%\#GRAFTED\" >nul || exit /b 1
)

REM -- 5. Проверка --------------------------------------------------------------
REM list - что вообще собралось; doctor - сходятся ли ABI, раскладка движка и имена
REM нативов между хостом и всеми плагинами разом. Ненулевой код = собранное не поедет.
echo.
"%BUILD%\graft.exe" list   "%OUT%" || exit /b 1
echo.
"%BUILD%\graft.exe" doctor "%OUT%" || exit /b 1

REM -- 6. Установка в игру ------------------------------------------------------
REM Отдельным шагом и только по слову deploy: собрать и проверить можно без игры, а
REM класть прокси-DLL рядом с чужим exe молча, за компанию, нельзя.
if /I not "%~1"=="deploy" goto :eof
echo.
REM Хост обязан лежать рядом с exe: игра грузит dwmapi.dll из своего каталога и больше
REM ниоткуда. Чужую install не перезапишет - скажет и остановится.
"%BUILD%\graft.exe" install "%GAME_DIR%" "%BUILD%\dwmapi.dll" || exit /b 1
REM Плагины кладутся В ТУ ЖЕ сборку, что и хост: они сверяют GRAFT_ABI_VERSION и
REM раскладку движка, и протухший плагин отвергается целиком - все его нативы после
REM этого молча вернут ноль. Скопировать один только хост уже стоило красной сьюты.
if exist "%GAME_DIR%\#GRAFTED" rmdir /S /Q "%GAME_DIR%\#GRAFTED"
xcopy /E /I /Y /Q "%OUT%\#GRAFTED" "%GAME_DIR%\#GRAFTED" >nul || exit /b 1
echo.
"%BUILD%\graft.exe" doctor "%GAME_DIR%" || exit /b 1
