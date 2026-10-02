# Сборка и запуск харнесса проверки спектра.
#
#   .\tests\run_tests.ps1                  — Release
#   .\tests\run_tests.ps1 -Configuration Debug
#
# Код возврата 0 — все проверки пройдены, 1 — есть расхождения.

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$projectDir = $PSScriptRoot
$vcxproj    = Join-Path $projectDir 'SpectrumTests.vcxproj'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath  = & $vswhere -latest -property installationPath
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'

# Qt VS Tools ставит свои .targets внутрь каталога расширения VS.
if (-not $env:QtMsBuild) {
    $qt = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\VisualStudio" -Recurse -Filter 'qt.targets' `
              -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $qt) { throw 'Не найден QtMsBuild — установлен ли Qt VS Tools?' }
    $env:QtMsBuild = $qt.Directory.FullName
}

& $msbuild $vcxproj /t:Build /p:Configuration=$Configuration /p:Platform=x64 /m /v:quiet
if ($LASTEXITCODE -ne 0) { throw "Сборка тестов не удалась (код $LASTEXITCODE)" }

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
& (Join-Path $projectDir "x64\$Configuration\SpectrumTests.exe")
exit $LASTEXITCODE
