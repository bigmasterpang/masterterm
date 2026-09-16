# MasterTerm update publishing: package -> generate manifest -> upload to update servers.
#
# All server URLs, tokens, SSH connections, and keys are loaded dynamically from:
#   1. Explicit script parameters (e.g. -UpdateBaseUrl, -MasterPortalUrl, etc.)
#   2. Local untracked configuration file (tools/publish-update.local.json)
#   3. Environment variables ($env:MASTERTERM_UPDATE_BASE_URL, etc.)
#
# No sensitive credentials, hostnames, or server IPs are hardcoded in this script.
# See tools/publish-update.local.json.example for configuration instructions.
#
# Usage:
#   .\tools\publish-update.ps1 [-BuildDir build-release] [-Version x.y.z]
# Prints PUBLISH_OK: <version> on success.
param(
    [string]$BuildDir = "build-release",
    [string]$Version = "",
    [string]$ConfigFile = (Join-Path $PSScriptRoot "publish-update.local.json"),
    [string]$Identity = "",
    [string]$Connection = "",
    [string]$UpdateBaseUrl = "",
    [string[]]$PublishTargets = @(),
    [string]$MasterPortalUrl = "",
    [string]$MasterPortalToken = "",
    [string]$AliPortalUrl = "",
    [string]$AliPortalToken = "",
    [string]$NotifyScript = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Load configuration from local config file if present
if (Test-Path -LiteralPath $ConfigFile) {
    try {
        $localCfg = Get-Content -LiteralPath $ConfigFile -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $Identity -and $localCfg.Identity) { $Identity = [string]$localCfg.Identity }
        if (-not $Connection -and $localCfg.Connection) { $Connection = [string]$localCfg.Connection }
        if (-not $UpdateBaseUrl -and $localCfg.UpdateBaseUrl) { $UpdateBaseUrl = [string]$localCfg.UpdateBaseUrl }
        if ($PublishTargets.Count -eq 0 -and $localCfg.PublishTargets) { $PublishTargets = @($localCfg.PublishTargets) }
        if (-not $MasterPortalUrl -and $localCfg.MasterPortalUrl) { $MasterPortalUrl = [string]$localCfg.MasterPortalUrl }
        if (-not $MasterPortalToken -and $localCfg.MasterPortalToken) { $MasterPortalToken = [string]$localCfg.MasterPortalToken }
        if (-not $AliPortalUrl -and $localCfg.AliPortalUrl) { $AliPortalUrl = [string]$localCfg.AliPortalUrl }
        if (-not $AliPortalToken -and $localCfg.AliPortalToken) { $AliPortalToken = [string]$localCfg.AliPortalToken }
        if (-not $NotifyScript -and $localCfg.NotifyScript) { $NotifyScript = [string]$localCfg.NotifyScript }
    } catch {
        Write-Warning "Failed to load local config from $($ConfigFile): $_"
    }
}

# Fallback to environment variables
if (-not $Identity -and $env:MASTERTERM_UPDATE_IDENTITY) { $Identity = $env:MASTERTERM_UPDATE_IDENTITY }
if (-not $Connection -and $env:MASTERTERM_UPDATE_SSH_CONNECTION) { $Connection = $env:MASTERTERM_UPDATE_SSH_CONNECTION }
if (-not $UpdateBaseUrl -and $env:MASTERTERM_UPDATE_BASE_URL) { $UpdateBaseUrl = $env:MASTERTERM_UPDATE_BASE_URL }
if (-not $MasterPortalUrl -and $env:MASTERTERM_MASTER_PORTAL_URL) { $MasterPortalUrl = $env:MASTERTERM_MASTER_PORTAL_URL }
if (-not $MasterPortalToken -and $env:MASTERTERM_MASTER_PORTAL_TOKEN) { $MasterPortalToken = $env:MASTERTERM_MASTER_PORTAL_TOKEN }
if (-not $AliPortalUrl -and $env:MASTERTERM_ALI_PORTAL_URL) { $AliPortalUrl = $env:MASTERTERM_ALI_PORTAL_URL }
if (-not $AliPortalToken -and $env:MASTERTERM_ALI_PORTAL_TOKEN) { $AliPortalToken = $env:MASTERTERM_ALI_PORTAL_TOKEN }
if (-not $NotifyScript -and $env:MASTERTERM_NOTIFY_SCRIPT) { $NotifyScript = $env:MASTERTERM_NOTIFY_SCRIPT }

if (-not $UpdateBaseUrl) {
    $UpdateBaseUrl = "https://updates.example.com"
}

$env:http_proxy = ""
$env:https_proxy = ""
$env:ALL_PROXY = ""
$env:no_proxy = "*"

$root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $cmakeLists = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    $match = [regex]::Match($cmakeLists, 'project\(MasterTerm VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $match.Success) { throw 'Unable to parse version from CMakeLists.txt' }
    $Version = $match.Groups[1].Value
}

# 1. Package using package-release.ps1.
& (Join-Path $PSScriptRoot 'package-release.ps1') -BuildDir $BuildDir -Version $Version
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }

$resolvedBuildDir = Join-Path $root $BuildDir
$zipPath = Join-Path (Join-Path $resolvedBuildDir 'dist') "MasterTerm-$Version-windows-x64.zip"
if (-not (Test-Path -LiteralPath $zipPath)) { throw "Update package not found: $zipPath" }

# 2. SHA-256 and manifest.
$sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
$zipName = Split-Path $zipPath -Leaf
$notesFile = Join-Path (Join-Path $resolvedBuildDir 'dist') "MasterTerm-$Version-windows-x64\RELEASE_NOTES.md"
$notes = if (Test-Path -LiteralPath $notesFile) {
    [string](Get-Content -LiteralPath $notesFile -Raw)
} else { "" }
if ([System.Text.Encoding]::UTF8.GetByteCount($notes) -gt 2800) {
    $truncated = @()
    $currentBytes = 0
    foreach ($line in ($notes -split "`n")) {
        $lBytes = [System.Text.Encoding]::UTF8.GetByteCount($line + "`n")
        if ($currentBytes + $lBytes -gt 2800) { break }
        $truncated += $line
        $currentBytes += $lBytes
    }
    $notes = ($truncated -join "`n").TrimEnd()
}
$manifest = [ordered]@{
    version = $Version
    url = "$UpdateBaseUrl/$zipName"
    sha256 = $sha256
    date = (Get-Date -Format 'yyyy-MM-dd')
    notes = $notes
}
$manifestPath = Join-Path $env:TEMP "masterterm-update-$Version.json"
$manifestJson = $manifest | ConvertTo-Json -Depth 4
[IO.File]::WriteAllBytes($manifestPath, [Text.Encoding]::UTF8.GetBytes($manifestJson))

$masterSuccess = $false

# 3. Upload to Primary Software Portal API
if ($MasterPortalUrl -and $MasterPortalToken) {
    Write-Host "Publishing to Primary Software Portal ($MasterPortalUrl)..."
    try {
        $curlArgs = @(
            "--noproxy", "*",
            "-4",
            "-s", "-S",
            "-X", "POST", "$MasterPortalUrl/api/upload",
            "-H", "Authorization: Bearer $MasterPortalToken",
            "-F", "file=@$zipPath",
            "-F", "app=masterterm",
            "-F", "platform=windows",
            "--ssl-no-revoke",
            "--connect-timeout", "15",
            "-F", "version=$Version",
            "-F", "release_notes=$notes"
        )
        $resp = & curl.exe @curlArgs
        if ($LASTEXITCODE -eq 0 -and $resp) {
            $json = $null
            try { $json = ($resp -join "`n") | ConvertFrom-Json } catch {}
            if ($json -and ($json.PSObject.Properties.Match('ok').Count -gt 0) -and $json.ok) {
                $masterSuccess = $true
                $retained = if ($json.PSObject.Properties.Match('retained_versions_count').Count -gt 0) { $json.retained_versions_count } else { '?' }
                "PUBLISH_MASTER_OK: $Version -> $MasterPortalUrl (retained $retained/10 versions)"
            } else {
                Write-Warning "Failed publishing to Primary Portal: $resp"
            }
        } else {
            Write-Warning "Failed publishing to Primary Portal (exit code $LASTEXITCODE): $resp"
        }
    } catch {
        Write-Warning "Error publishing to Primary Portal: $_"
    }
}

# 4. Upload to SSH / Legacy channel
if ($Connection -and ($PublishTargets -contains 'legacy' -or $PublishTargets -contains 'ssh' -or (-not $masterSuccess -and $PublishTargets.Count -gt 0))) {
    if ($masterSuccess) {
        Write-Host "Legacy channel synchronized on-server. Skipping redundant SCP upload." -ForegroundColor Green
    } else {
        $cli = Join-Path $resolvedBuildDir 'MasterTerm-cli.exe'
        if (Test-Path -LiteralPath $cli) {
            $execArgs = @()
            if ($Identity) { $execArgs += @('--identity', $Identity) }
            & $cli @execArgs --exec "install -d -m 755 /opt/masterterm-updates && chmod 755 /opt/masterterm-updates" $Connection
            if ($LASTEXITCODE -ne 0) { throw 'Unable to create the server update directory' }
            & $cli @execArgs --scp-upload $zipPath $Connection "/opt/masterterm-updates/$zipName"
            if ($LASTEXITCODE -ne 0) { throw 'Update package upload failed' }
            & $cli @execArgs --scp-upload $manifestPath $Connection "/opt/masterterm-updates/masterterm.json"
            if ($LASTEXITCODE -ne 0) { throw 'Manifest upload failed' }
            "PUBLISH_SSH_OK: $Version -> $Connection"
        } else {
            # Fallback to standard scp
            $scpArgs = @('-o', 'StrictHostKeyChecking=no')
            if ($Identity) { $scpArgs = @('-i', $Identity) + $scpArgs }
            scp @scpArgs $zipPath "${Connection}:/opt/masterterm-updates/$zipName"
            scp @scpArgs $manifestPath "${Connection}:/opt/masterterm-updates/masterterm.json"
            "PUBLISH_SSH_OK: $Version -> $Connection (via scp)"
        }
    }
}

# 5. Upload to Secondary / Mirror Portal
if ($AliPortalUrl -and $AliPortalToken) {
    Write-Host "Publishing to Secondary Portal ($AliPortalUrl)..."
    try {
        $curlArgs = @(
            "--noproxy", "*",
            "-4",
            "-s", "-S",
            "-X", "POST", "$AliPortalUrl/api/upload",
            "-H", "Authorization: Bearer $AliPortalToken",
            "-F", "file=@$zipPath",
            "-F", "app=masterterm",
            "-F", "platform=windows",
            "--connect-timeout", "15",
            "-F", "version=$Version",
            "-F", "release_notes=$notes"
        )
        $resp = & curl.exe @curlArgs
        if ($LASTEXITCODE -eq 0 -and $resp) {
            $json = $null
            try { $json = ($resp -join "`n") | ConvertFrom-Json } catch {}
            if ($json -and ($json.PSObject.Properties.Match('ok').Count -gt 0) -and $json.ok) {
                $retained = if ($json.PSObject.Properties.Match('retained_versions_count').Count -gt 0) { $json.retained_versions_count } else { '?' }
                "PUBLISH_MIRROR_OK: $Version -> $AliPortalUrl (retained $retained/10 versions)"
            } else {
                Write-Warning "Failed publishing to Secondary Portal: $resp"
            }
        } else {
            Write-Warning "Failed publishing to Secondary Portal (exit code $LASTEXITCODE): $resp"
        }
    } catch {
        Write-Warning "Error publishing to Secondary Portal: $_"
    }
}

"PUBLISH_OK: $Version ($sha256)"
"manifest: $UpdateBaseUrl/masterterm.json"
"zip: $UpdateBaseUrl/$zipName"
if ($MasterPortalUrl) { "master_portal: $MasterPortalUrl" }
if ($AliPortalUrl) { "secondary_portal: $AliPortalUrl" }

if ($NotifyScript -and (Test-Path -LiteralPath $NotifyScript)) {
    try {
        $notifyTitle = "MasterTerm v$Version 发布成功"
        $changelogFile = Join-Path $root 'CHANGELOG.md'
        $highlights = [System.Collections.Generic.List[string]]::new()
        if (Test-Path -LiteralPath $changelogFile) {
            $inSection = $false
            foreach ($line in (Get-Content -LiteralPath $changelogFile -Encoding UTF8)) {
                if ($line -match "^###\s+v?$Version\b") {
                    $inSection = $true
                    continue
                }
                if ($inSection -and $line -match "^###\s+v?\d") {
                    break
                }
                if ($inSection -and $line -match "^\s*-\s+\*\*([^*]+)\*\*") {
                    $highlights.Add($matches[1].Trim())
                    if ($highlights.Count -ge 5) { break }
                }
            }
        }
        
        $notifyContent = "安装包: $zipName"
        if ($highlights.Count -gt 0) {
            $items = @()
            for ($i = 0; $i -lt $highlights.Count; $i++) {
                $items += "$($i + 1). $($highlights[$i])"
            }
            $bulletText = $items -join "`n"
            $notifyContent += "`n核心特性升级：`n$bulletText"
        } elseif ($notes) {
            $cleanNotes = ($notes -split "`n" | Where-Object { $_ -match '^\s*-\s+' } | Select-Object -First 4 | ForEach-Object { $_.Trim() }) -join "`n"
            if ($cleanNotes) {
                $notifyContent += "`n更新摘要：`n$cleanNotes"
            }
        }
        $notifyContent += "`n点击卡片查看完整日志与直达下载"
        
        $targetUrl = if ($MasterPortalUrl) { $MasterPortalUrl } else { $UpdateBaseUrl }
        & $NotifyScript -Title $notifyTitle -Content $notifyContent -Url "$targetUrl"
    } catch {
        Write-Warning "发送发布通知异常: $_"
    }
}

Remove-Item -LiteralPath $manifestPath -Force -ErrorAction SilentlyContinue
exit 0
