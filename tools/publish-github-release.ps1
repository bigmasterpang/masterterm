param(
    [string]$Version = "",
    [string]$Token = "",
    [string]$TokenPath = "",
    [string]$Repo = "bigmasterpang/masterterm"
)

$ErrorActionPreference = 'Stop'
$pyScript = Join-Path $PSScriptRoot "publish-github-release.py"

$pyArgs = @($pyScript)
if ($Version) {
    $pyArgs += @("--version", $Version)
}
if ($Token) {
    $pyArgs += @("--token", $Token)
}
if ($TokenPath) {
    $pyArgs += @("--token-path", $TokenPath)
}
if ($Repo) {
    $pyArgs += @("--repo", $Repo)
}

& python $pyArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

