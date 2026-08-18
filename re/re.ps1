<#
  RE-обёртка над IDA Pro 8.3 (headless) для DayZServer_x64.exe.

  .\re\re.ps1 import            — скопировать бинарь в re\bin (Steam-каталог не трогаем)
  .\re\re.ps1 analyze           — авто-анализ, создаёт re\bin\<name>.i64 (долго, 5-20 мин)
  .\re\re.ps1 run <script.py>   — прогнать IDAPython-скрипт по готовой .i64
  .\re\re.ps1 gui               — открыть .i64 в графической IDA

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

$IdaDir = 'F:\SOFT\IDA Pro 8.3'
$Target = switch ($On) {
    'diag' { 'F:\SteamLibrary\steamapps\common\DayZ\DayZDiag_x64.exe' }
    'server' { 'F:\SteamLibrary\steamapps\common\DayZServer\DayZServer_x64.exe' }
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
