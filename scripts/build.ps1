# Build the driver and lumen-vdd-ctl (Release x64) and sign them into build\package.
# Runs as a regular user. Uses the 64-bit MSBuild: the 32-bit one looks for x86\InfVerif.dll, which WDK 10.0.28000
# does not ship.

param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$VsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -property installationPath
$MSBuild = Join-Path $VsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'

& $MSBuild (Join-Path $Root 'lumen-vdd.sln') /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

& (Join-Path $PSScriptRoot 'sign.ps1') -Configuration $Configuration
