<#
  RE-обёртка над IDA Pro (headless) для исполняемых файлов DayZ.

  .\RESEARCH\re.ps1 import          — скопировать бинарь в RESEARCH\bin (Steam не трогаем)
  .\RESEARCH\re.ps1 analyze         — авто-анализ, создаёт .i64 (долго, 5-20 мин)
  .\RESEARCH\re.ps1 run <script.py> — прогнать IDAPython-скрипт по готовой .i64
  .\RESEARCH\re.ps1 gui             — открыть .i64 в графической IDA

  Пути к своей машине — в переменных окружения, в репозитории их нет:
    GRAFT_IDA_DIR      каталог IDA (там idat64.exe)
    GRAFT_GAME_DIR     каталог игры (DayZDiag_x64.exe)
    GRAFT_SERVER_DIR   каталог сервера (DayZServer_x64.exe), нужен только для -On server

  ponytail: одна обёртка вместо фреймворка. Ghidra здесь не нужна — она в C:\tools
  как запасной декомпилятор, подключим только если Hex-Rays на чём-то сломается.
#>
param(
    [Parameter(Mandatory)][ValidateSet('import', 'analyze', 'run', 'gui')]
    [string]$Cmd,
    [string]$Arg,
    # diag — то, на чём MCP гоняет тесты; server — боевой сервер
    [ValidateSet('diag', 'server')][string]$On = 'diag'
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$IdaDir = $env:GRAFT_IDA_DIR
if (-not $IdaDir) { throw 'Задай GRAFT_IDA_DIR — каталог IDA (там idat64.exe)' }
$Target = switch ($On) {
    'diag' {
        if (-not $env:GRAFT_GAME_DIR) { throw 'Задай GRAFT_GAME_DIR — каталог игры' }
        Join-Path $env:GRAFT_GAME_DIR 'DayZDiag_x64.exe'
    }
    'server' {
        if (-not $env:GRAFT_SERVER_DIR) { throw 'Задай GRAFT_SERVER_DIR — каталог сервера' }
        Join-Path $env:GRAFT_SERVER_DIR 'DayZServer_x64.exe'
    }
}
$Root = Split-Path -Parent $PSCommandPath
$BinDir = Join-Path $Root 'bin'
$OutDir = Join-Path $Root "out\$On"
$Local = Join-Path $BinDir (Split-Path $Target -Leaf)
$Idb = "$Local.i64"   # IDA дописывает расширение к полному имени, а не заменяет

New-Item -ItemType Directory -Force -Path $BinDir, $OutDir | Out-Null

switch ($Cmd) {
    'import' {
        if (-not (Test-Path $Target)) { throw "Не найден $Target" }
        Copy-Item $Target $Local -Force
        # версия бинаря — чтобы понимать, к какому патчу привязаны смещения
        (Get-Item $Local).VersionInfo | Format-List FileVersion, ProductVersion
        (Get-FileHash $Local -Algorithm SHA256).Hash | Out-File (Join-Path $OutDir 'target.sha256') -Encoding utf8
        Write-Output "OK: $Local"
    }
    'analyze' {
        if (-not (Test-Path $Local)) { throw 'Сначала: re.ps1 import' }
        if (Test-Path $Idb) { Write-Output "Уже есть $Idb (удали, чтобы переанализировать)"; break }
        & "$IdaDir\idat64.exe" -B -P+ $Local
        Remove-Item "$Local.asm" -ErrorAction SilentlyContinue   # -B попутно сыплет 200-МБ листинг, он не нужен
        if (-not (Test-Path $Idb)) { throw 'IDA did not produce .i64' }
        Write-Output "OK: $Idb"
    }
    'run' {
        if (-not (Test-Path $Idb)) { throw 'Сначала: re.ps1 analyze' }
        $script = Resolve-Path (Join-Path $Root "scripts\$Arg")
        $env:RE_OUT = $OutDir
        & "$IdaDir\idat64.exe" -A -S"`"$script`"" -L"$OutDir\ida.log" $Idb
        Get-Content "$OutDir\ida.log" -Tail 40
    }
    'gui' { & "$IdaDir\ida64.exe" $Idb }
}
