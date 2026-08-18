# Разложить мод по игре: PBO в addons/, плагин в grafted/.
#
#   .\deploy.ps1 -Game "C:\DayZServer"
#
# graft этого не делает принципиально: мод — твой, инструмент упаковки — твой. Скрипт
# лежит здесь как рабочая заготовка; правь под себя.
param(
    [Parameter(Mandatory)] [string] $Game,
    [string] $Name    = "EXAMPLE_GRAFT",
    [string] $Preset  = "release",
    [string] $MakePbo = "C:\Program Files (x86)\Mikero\DePboTools\bin\MakePbo.exe"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out  = Join-Path $root "out\$Preset"
$mod  = Join-Path $root "mod\$Name"
$dist = Join-Path $root "dist\@$Name"

# 1. Собрать: DLL + <ИМЯ>.scripts с объявлениями.
cmake --build --preset $Preset
if ($LASTEXITCODE) { throw "сборка упала" }

# 2. Упаковать PBO и положить плагин рядом с addons. Объявления уже в моде: сборка пишет
# их туда сама (SCRIPTS_DIR в CMakeLists.txt).
New-Item -ItemType Directory -Force "$dist\addons", "$dist\grafted" | Out-Null
& $MakePbo -P -D $mod "$dist\addons\$Name.pbo"
if ($LASTEXITCODE) { throw "MakePbo упал" }
Copy-Item -Force (Join-Path $out "$Name.grafted.dll") "$dist\grafted\"

# 3. Положить мод в игру. Хост ставится один раз: graft install <каталог игры>.
Copy-Item -Recurse -Force $dist $Game
Write-Host "готово: $Game\@$Name  (запускать с -mod=@$Name)"
