# MasterTerm release packaging: creates the ZIP staging directory and builds
# a formal NSIS installer from the same runtime files.
#
# Usage:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\package-installer.ps1
#       [-BuildDir build-release] [-Version x.y.z]
# Prints INSTALLER_OK: <exe path> on success.
param(
    [string]$BuildDir = "build-release",
    [string]$Version = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
}

$root = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = Join-Path $root $BuildDir
Assert-True (Test-Path -LiteralPath $resolvedBuildDir) "Build directory not found: $resolvedBuildDir"

if (-not $Version) {
    $cmakeLists = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    $match = [regex]::Match($cmakeLists, 'project\(MasterTerm VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
    Assert-True $match.Success 'Unable to parse version from CMakeLists.txt'
    $Version = $match.Groups[1].Value
}

# Reuse the ZIP packaging path so the installer and ZIP contain exactly the
# same binaries, runtime DLLs, README, release notes and web assets.
& (Join-Path $PSScriptRoot 'package-release.ps1') -BuildDir $BuildDir -Version $Version
if ($LASTEXITCODE -ne 0) { throw "ZIP staging failed" }

$distDir = Join-Path $resolvedBuildDir 'dist'
$staging = Join-Path $distDir "MasterTerm-$Version-windows-x64"
$outputFile = Join-Path $distDir "MasterTerm-$Version-Setup.exe"
$nsi = Join-Path $root 'installer\MasterTerm.nsi'
Assert-True (Test-Path -LiteralPath (Join-Path $staging 'MasterTerm.exe')) 'Staging is missing MasterTerm.exe'
Assert-True (Test-Path -LiteralPath (Join-Path $staging 'MasterTermSftpWorker.exe')) 'Staging is missing MasterTermSftpWorker.exe'
Assert-True (Test-Path -LiteralPath (Join-Path $staging 'MasterTerm-cli.exe')) 'Staging is missing MasterTerm-cli.exe'
Assert-True (Test-Path -LiteralPath (Join-Path $staging 'web\index.html')) 'Staging is missing web assets'
Assert-True (Test-Path -LiteralPath $nsi) "NSIS script not found: $nsi"

$makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (-not $makensis) {
    $nsisCandidates = @(
        'C:\Program Files\NSIS\makensis.exe',
        'C:\Program Files (x86)\NSIS\makensis.exe'
    )
    $nsisPath = $nsisCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    Assert-True ([bool]$nsisPath) 'makensis.exe was not found; install NSIS first'
} else {
    $nsisPath = $makensis.Source
}

$sourceForNsi = $staging -replace '\\', '/'
$outputForNsi = $outputFile -replace '\\', '/'
$rootForNsi = $root -replace '\\', '/'
& $nsisPath "/DAPP_VERSION=$Version" "/DSOURCE_DIR=$sourceForNsi" "/DOUTPUT_FILE=$outputForNsi" "/DROOT_DIR=$rootForNsi" $nsi
if ($LASTEXITCODE -ne 0) { throw "NSIS packaging failed with exit code $LASTEXITCODE" }
Assert-True (Test-Path -LiteralPath $outputFile) 'NSIS output was not created'

$size = (Get-Item -LiteralPath $outputFile).Length
Assert-True ($size -gt 1MB) "NSIS output is unexpectedly small: $size bytes"
"INSTALLER_OK: $outputFile"
"INSTALLER_SIZE: $size"
exit 0
