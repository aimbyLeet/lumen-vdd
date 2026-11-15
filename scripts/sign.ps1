# Sign the built driver package with the development certificate and collect it in build\package.
# Runs as a regular user. The certificate (CN=Lumen VDD Dev, Code Signing EKU only) is created on first use in
# CurrentUser\My with a non-exportable private key; only the .cer leaves the store.

# Signatures are timestamped (RFC 3161; only file hashes go to the server), so packages stay installable after the
# certificate expires. -TimestampUrl '' signs without a timestamp (offline builds; such a package stops installing
# when the certificate expires).

param([string]$Configuration = 'Release', [string]$TimestampUrl = 'http://timestamp.digicert.com')
$ErrorActionPreference = 'Stop'

$Subject  = 'CN=Lumen VDD Dev'
$SignTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe'
$Root     = Split-Path $PSScriptRoot -Parent
$Build    = Join-Path $Root "build\$Configuration"
$Pkg      = Join-Path $Root 'build\package'

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq $Subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(1)
    Write-Host "Created certificate $($cert.Thumbprint)"
}

if (Test-Path $Pkg) { Remove-Item -Recurse -Force $Pkg }
New-Item -ItemType Directory -Force $Pkg | Out-Null
Copy-Item (Join-Path $Build 'lumen_vdd\*') $Pkg
Copy-Item (Join-Path $Build 'lumen-vdd-ctl.exe'), (Join-Path $Build 'lumen-vdd-setup.exe') $Pkg
Export-Certificate -Cert $cert -FilePath (Join-Path $Pkg 'lumen-vdd.cer') | Out-Null

# The catalog holds the Authenticode PE hash, which excludes the embedded signature, so signing the DLL after
# catalog generation keeps the catalog valid.
# The timestamp server sometimes fails a request: retry (signing again replaces the signature).
$Timestamp = if ($TimestampUrl) { @('/tr', $TimestampUrl, '/td', 'sha256') } else { @() }
$Files = 'lumen_vdd.dll', 'lumen_vdd.cat', 'lumen-vdd-ctl.exe', 'lumen-vdd-setup.exe' | ForEach-Object { Join-Path $Pkg $_ }
$Attempts = if ($TimestampUrl) { 3 } else { 1 }
for ($i = 1; $i -le $Attempts; $i++) {
    & $SignTool sign /q /sha1 $cert.Thumbprint /s My /fd sha256 @Timestamp @Files
    if ($LASTEXITCODE -eq 0) { break }
    if ($i -eq $Attempts) { throw "signtool failed: $LASTEXITCODE" }
    Write-Host "signtool failed (attempt $i of $Attempts), retrying"
    Start-Sleep -Seconds 5
}

Write-Host "Package: $Pkg (certificate $($cert.Thumbprint))"
