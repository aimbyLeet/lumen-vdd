# Pack build\package into the release archive build\release\lumen-vdd-<version>.zip (one folder lumen-vdd-<version>
# with the package and the license) and write its SHA-256 next to it. Run scripts\build.ps1 first. The version is
# the driver version from driver\lumen_vdd.vcxproj; the script refuses a package with another DriverVer.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Pkg = Join-Path $Root 'build\package'
$Out = Join-Path $Root 'build\release'

[xml]$proj = Get-Content (Join-Path $Root 'driver\lumen_vdd.vcxproj')
$props = $proj.Project.PropertyGroup | Where-Object { $_.LumenVddVersionMajor } | Select-Object -First 1
$Version = '{0}.{1}.{2}' -f $props.LumenVddVersionMajor, $props.LumenVddVersionMinor, $props.LumenVddVersionPatch

$Files = 'lumen_vdd.inf', 'lumen_vdd.cat', 'lumen_vdd.dll', 'lumen-vdd.cer', 'lumen-vdd-setup.exe', 'lumen-vdd-ctl.exe'
foreach ($f in $Files) {
    if (-not (Test-Path (Join-Path $Pkg $f))) { throw "$Pkg\$f not found: run scripts\build.ps1 first" }
}
$driverVer = Select-String (Join-Path $Pkg 'lumen_vdd.inf') -Pattern '^DriverVer\s*=\s*[^,]+,(\S+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
if ($driverVer -ne "$Version.0") { throw "package DriverVer is '$driverVer', the project says ${Version}: rebuild" }

$Name = "lumen-vdd-$Version"
$Stage = Join-Path $Out $Name
$Zip = Join-Path $Out "$Name.zip"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null
foreach ($f in $Files) { Copy-Item (Join-Path $Pkg $f) $Stage }
Copy-Item (Join-Path $Root 'LICENSE-MIT') $Stage

if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $Stage -DestinationPath $Zip
Remove-Item -Recurse -Force $Stage

$hash = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$Zip.sha256", "$hash  $Name.zip`n")
Write-Host "$Zip"
Write-Host "SHA-256 $hash"
