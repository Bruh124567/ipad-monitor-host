# Packages the built driver, creates its catalog, and signs it with a self-made test certificate.
# Run from an administrator PowerShell, after building IpadMonitorIdd.vcxproj (Release x64).
#
#   .\package-and-sign.ps1 [-Configuration Release]

param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'

$here    = $PSScriptRoot
$dll     = Join-Path $here "build\x64\$Configuration\IpadMonitorIdd.dll"
$inf     = Join-Path $here 'IpadMonitorIdd.inf'
$package = Join-Path $here 'package'
$subject = 'CN=iPadMonitorTestCert'

if (-not (Test-Path $dll)) { throw "Build output not found: $dll. Build the project first." }

# Find the newest WDK/SDK tools.
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
function Find-Tool($name, $arch) {
    $hit = Get-ChildItem $kits -Directory | Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "$arch\$name" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $hit) { throw "$name not found under $kits. Is the Windows Driver Kit installed?" }
    $hit
}
$inf2cat  = Find-Tool 'Inf2Cat.exe' 'x86'
$signtool = Find-Tool 'signtool.exe' 'x64'

# Test certificate: created once, trusted by this machine.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
if (-not $cert) {
    Write-Host 'Creating test certificate...'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
        -NotAfter (Get-Date).AddYears(5) -KeyExportPolicy NonExportable
    $cer = Join-Path $env:TEMP 'iPadMonitorTestCert.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

# Package folder: INF + DLL, signed DLL, catalog, signed catalog.
if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item $package -ItemType Directory | Out-Null
Copy-Item $dll, $inf $package

& $signtool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint (Join-Path $package 'IpadMonitorIdd.dll')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on the DLL' }

& $inf2cat "/driver:$package" '/os:10_x64' '/uselocaltime'
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

& $signtool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint (Join-Path $package 'IpadMonitorIdd.cat')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on the catalog' }

Write-Host ''
Write-Host "Done. Install with:  vdctl install `"$package\IpadMonitorIdd.inf`""
