# MasterTerm release packaging: assembles the runtime directory (exe, SFTP
# worker, CLI, runtime DLLs, web assets, README) and produces a versioned
# ZIP plus a RELEASE_NOTES.md derived from git history since the last tag.
#
# Usage:
#   .\tools\package-release.ps1 [-BuildDir build-release] [-OutDir <dir>]
#                               [-Version x.y.z]
# Exit codes: 0 = packaged, 1 = error. Prints PACKAGE_OK: <zip path>.
param(
    [string]$BuildDir = "build-release",
    [string]$OutDir = "",
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

if (-not $OutDir) { $OutDir = Join-Path $resolvedBuildDir 'dist' }
$staging = Join-Path $OutDir "MasterTerm-$Version-windows-x64"
$zipPath = Join-Path $OutDir "MasterTerm-$Version-windows-x64.zip"
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging -Force | Out-Null

foreach ($name in @('MasterTerm.exe', 'MasterTermSftpWorker.exe', 'MasterTerm-cli.exe')) {
    $source = Join-Path $resolvedBuildDir $name
    Assert-True (Test-Path -LiteralPath $source) "Missing $name (build first)"
    Copy-Item -LiteralPath $source -Destination $staging
}

foreach ($dll in Get-ChildItem -LiteralPath $resolvedBuildDir -Filter '*.dll') {
    Copy-Item -LiteralPath $dll.FullName -Destination $staging
}

# The GUI and workers are built with the current MSVC STL. Shipping these
# app-local files prevents a target PC's older Visual C++ redistributable from
# being selected and crashing synchronization primitives during SSH startup.
foreach ($runtime in @(
    'msvcp140.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll'
)) {
    Assert-True (Test-Path -LiteralPath (Join-Path $staging $runtime)) `
        "Missing compiler-matched VC++ runtime: $runtime"
}

# Web assets are loaded from a "web" directory next to the executable.
Copy-Item -LiteralPath (Join-Path $root 'resources\web') -Destination $staging -Recurse
Assert-True (Test-Path -LiteralPath (Join-Path $staging 'web\index.html')) 'web assets are incomplete'

$readme = Join-Path $root 'README.md'
if (Test-Path -LiteralPath $readme) { Copy-Item -LiteralPath $readme -Destination $staging }

# Release notes: extract the latest single version section from CHANGELOG.md (capped at 2800 bytes
# to guarantee older clients with legacy 4000-byte limits will never encounter UTF-8 truncation issues),
# falling back to git log (top 5 commits) if not available.
$notes = Join-Path $staging 'RELEASE_NOTES.md'
$summary = @()
$changelogPath = Join-Path $root 'CHANGELOG.md'
if (Test-Path -LiteralPath $changelogPath) {
    $clLines = Get-Content -LiteralPath $changelogPath -Encoding UTF8
    $versionCount = 0
    $totalBytes = 0
    foreach ($line in $clLines) {
        if ($line -match '^###\s+v?\d+\.\d+') {
            $versionCount++
            if ($versionCount -gt 1) { break }
        }
        if ($versionCount -ge 1) {
            $lineBytes = [System.Text.Encoding]::UTF8.GetByteCount($line + "`n")
            if ($totalBytes + $lineBytes -gt 2800) { break }
            $summary += $line
            $totalBytes += $lineBytes
        }
    }
}
if ($summary.Count -eq 0) {
    $tag = (& git -C $root tag --list | Select-Object -Last 1)
    if ($tag) {
        $logLines = @(& git -C $root log --oneline --no-merges -5 "$tag..HEAD")
    } else {
        $logLines = @(& git -C $root log --oneline --no-merges -5)
    }
    $summary += "# MasterTerm $Version Release Notes"
    $summary += ""
    if ($logLines) {
        $summary += "## Changes"
        $summary += ""
        foreach ($line in $logLines) { $summary += "- $line" }
    } else {
        $summary += "No commit history found; see README for details."
    }
}
Set-Content -LiteralPath $notes -Value $summary -Encoding utf8

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$tar = Get-Command tar.exe -ErrorAction Stop
& $tar.Source -a -cf $zipPath -C $staging .
if ($LASTEXITCODE -ne 0) { throw "Compatible ZIP creation failed" }
Assert-True (Test-Path -LiteralPath $zipPath) 'ZIP packaging failed'

"PACKAGE_OK: $zipPath"
"RELEASE_NOTES: $notes"
exit 0
