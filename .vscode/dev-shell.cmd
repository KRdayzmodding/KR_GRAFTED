@echo off
rem Запускает команду в окружении Visual Studio: clang-cl берёт Windows SDK и STL из
rem переменных, которые ставит vcvars64.bat. Без этого конфигурация падает на поиске
rem windows.h, и виноват будет якобы CMake (см. CONTRIBUTING, раздел 1).
rem
rem   .vscode\dev-shell.cmd cmake --preset release
rem
rem Путь к VS не зашит: его находит vswhere, который ставится вместе с любой редакцией.

setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [dev-shell] vswhere не найден: "%VSWHERE%"
    echo [dev-shell] нужна Visual Studio 2022 с компонентом Desktop development with C++
    exit /b 1
)

rem Перебираем ВСЕ установки от новой к старой и берём первую, у которой есть
rem vcvars64.bat. Так надёжнее, чем -latest с фильтром по компоненту:
rem   * -latest молча выбирает установку без C++ (у Build Tools набор компонентов
rem     другой, и фильтр -requires отсекает как раз рабочую);
rem   * -prerelease нужен тем, у кого стоит Insiders-редакция;
rem   * существование самого vcvars64.bat — и есть признак, что C++ на месте.
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -prerelease -products * -sort -format value -property installationPath`) do (
    if not defined VCVARS if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo [dev-shell] не нашёл установку Visual Studio с C++-инструментами
    echo [dev-shell] нужен компонент Desktop development with C++
    exit /b 1
)

rem 2>&1 в тишину намеренно: vcvars64.bat сам зовёт vswhere из каталога, которого нет в
rem PATH, и ругается на это в stderr, хотя окружение при этом ставит. Настоящий отказ
rem ловится кодом возврата, а не текстом.
call "%VCVARS%" >nul 2>&1 || exit /b 1
%*
