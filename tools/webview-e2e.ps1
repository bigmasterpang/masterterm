[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$Identity = '',
    [string]$Connection = '',
    [int]$TimeoutSeconds = 300,
    [switch]$KeepArtifacts
)

if (-not $Identity) {
    if ($env:MASTERTERM_TEST_IDENTITY) { $Identity = $env:MASTERTERM_TEST_IDENTITY }
    elseif (Test-Path -LiteralPath (Join-Path $env:USERPROFILE 'Documents\id_rsa')) {
        $Identity = (Join-Path $env:USERPROFILE 'Documents\id_rsa')
    } elseif (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.ssh\id_rsa')) {
        $Identity = (Join-Path $env:USERPROFILE '.ssh\id_rsa')
    }
}
if (-not $Connection -and $env:MASTERTERM_TEST_CONNECTION) {
    $Connection = $env:MASTERTERM_TEST_CONNECTION
}

# T1-3 真实 WebView2 端到端测试：在隔离数据目录中启动 MasterTerm，
# 通过 MASTERTERM_CDP_PORT 暴露 Chrome DevTools 协议，由
# tests/webview-e2e.js 驱动真实 UI：
#   设置弹窗打开/滚动/关闭 -> 真实 SSH 连接 -> SFTP 面板加载 ->
#   分屏创建/关闭 -> RDP 失败生命周期（断开提示/关闭标签）->
#   关闭会话 -> UI 驱动退出。
# 失败时截图与 DOM 摘要写入 $BuildDir\e2e-artifacts，并复制
# diagnostic.log/crash.log。
#
# 依赖：%USERPROFILE%\Documents\id_rsa 等可用 SSH 凭据；没有可用
# 凭据时打印 SKIP 并以 0 退出（与其它回归脚本一致，便于 CI 空跑）。

$ErrorActionPreference = 'Stop'
if (-not $BuildDir) { $BuildDir = Join-Path $PSScriptRoot '..\build-release' }
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$resolvedBuildDir = (Resolve-Path $BuildDir).Path
$executable = Join-Path $resolvedBuildDir 'MasterTerm.exe'
$nodeScript = Join-Path $projectRoot 'tests\webview-e2e.js'
$artifactDir = Join-Path $resolvedBuildDir 'e2e-artifacts'

if (-not (Test-Path -LiteralPath $executable)) {
    throw "FAIL: 找不到 $executable"
}
if (Get-Process -Name 'MasterTerm' -ErrorAction SilentlyContinue) {
    throw 'FAIL: 已有 MasterTerm 正在运行，请先关闭它再执行端到端测试'
}
if (-not $Connection) {
    Write-Output 'SKIP: 未指定 SSH 测试连接目标 (可通过 -Connection 或 $env:MASTERTERM_TEST_CONNECTION 指定)，跳过 WebView2 端到端测试'
    exit 0
}
if (-not (Test-Path -LiteralPath $Identity)) {
    Write-Output 'SKIP: 找不到 SSH 私钥，跳过 WebView2 端到端测试'
    exit 0
}
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Output 'SKIP: 找不到 node.exe，跳过 WebView2 端到端测试'
    exit 0
}

# 选一个空闲端口作为 CDP 调试端口
$port = 0
for ($candidate = 9300; $candidate -le 9800; $candidate++) {
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback, $candidate)
    try {
        $listener.Start()
        $port = $candidate
        $listener.Stop()
        break
    }
    catch {
        $listener.Stop()
    }
}
if ($port -eq 0) {
    throw 'FAIL: 无法找到空闲端口启动 CDP'
}

# 隔离数据目录：含 closeBehavior=exit、SSH 测试连接和 RDP 失败路径连接
$dataDir = Join-Path $env:TEMP ("masterterm-e2e-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
$config = [ordered]@{
    closeBehavior = 'exit'
    servers = @(
        [ordered]@{
            name = 'e2e-ssh'
            connectionType = 'ssh'
            address = $Connection
            port = '22'
            keyPath = $Identity
            color = '#34d399'
            iconKey = 'server'
            tag = 'SSH'
            workspace = '未分配'
            proxyJump = ''
            proxyKeyPath = ''
            serialDataBits = 8
            serialParity = 'none'
            serialStopBits = '1'
            serialFlowControl = 'none'
            tunnels = @()
        },
        [ordered]@{
            name = 'e2e-rdp'
            connectionType = 'rdp'
            address = '127.0.0.1'
            port = '3389'
            keyPath = ''
            color = '#fbbf24'
            iconKey = 'server'
            tag = 'RDP'
            workspace = '未分配'
            proxyJump = ''
            proxyKeyPath = ''
            rdpOptions = [ordered]@{
                audioRedirectionMode = -1
                autoReconnect = -1
                colorDepth = -1
                keyboardHookMode = -1
                maxReconnectAttempts = 3
                performanceFlags = -1
                redirectClipboard = -1
                redirectDrives = -1
                redirectPrinters = -1
                smartSizing = -1
            }
            serialDataBits = 8
            serialParity = 'none'
            serialStopBits = '1'
            serialFlowControl = 'none'
            tunnels = @()
        }
    )
    workspaces = @('未分配')
}
$configJson = ConvertTo-Json -InputObject $config -Depth 12
[System.IO.File]::WriteAllText(
    (Join-Path $dataDir 'MasterSSH.json'), $configJson,
    (New-Object System.Text.UTF8Encoding $false))

$previousDataDir = $env:MASTERSSH_DATA_DIR
$previousCdpPort = $env:MASTERTERM_CDP_PORT
$process = $null
try {
    # T2-2 导入验证材料：把测试私钥用 DPAPI 加密成云同步密文格式，
    # 由页面驱动 cloud.importServers 后，后端应能本机解密并写入
    # 数据目录的 keys 文件夹。
    Add-Type -AssemblyName System.Security
    $keyBytes = [System.IO.File]::ReadAllBytes($Identity)
    $keyBase64 = [Convert]::ToBase64String($keyBytes)
    # 与 src/NativeCrypto.h 的 CloudSyncEntropy 一致（含结尾空字符）；
    # 加密内容与后端导出一致：先 Base64 再 DPAPI（导入端解密后再 Base64 解码）
    $entropyBytes = [System.Text.Encoding]::Unicode.GetBytes(
        "MasterTerm.CloudSync.v1" + [char]0)
    $encryptedKey = [Convert]::ToBase64String(
        [System.Security.Cryptography.ProtectedData]::Protect(
            [System.Text.Encoding]::ASCII.GetBytes($keyBase64), $entropyBytes,
            [System.Security.Cryptography.DataProtectionScope]::CurrentUser))
    $testKeySecret = "enc:$encryptedKey"
    $testKeyName = (Split-Path -Leaf $Identity)

    $env:MASTERSSH_DATA_DIR = $dataDir
    $env:MASTERTERM_CDP_PORT = [string]$port
    $process = Start-Process -FilePath $executable -PassThru

    & node $nodeScript --port $port --artifact-dir $artifactDir `
        --data-dir $dataDir --test-key-secret $testKeySecret `
        --test-key-name $testKeyName `
        --ssh-name 'e2e-ssh' --rdp-name 'e2e-rdp'
    $nodeExit = $LASTEXITCODE

    if ($nodeExit -eq 0) {
        # 通过 UI 关闭后进程应在 20 秒内自己退出
        if ($process.WaitForExit(20000)) {
            if ($process.ExitCode -eq 0) {
                Write-Output 'WEBVIEW_E2E_OK'
            }
            else {
                throw "FAIL: MasterTerm 退出码为 $($process.ExitCode)（期望 0）"
            }
        }
        else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw 'FAIL: UI 已发送退出但进程 20 秒内未结束'
        }
    }
    else {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        foreach ($log in @('diagnostic.log', 'crash.log')) {
            $source = Join-Path $dataDir ("logs\" + $log)
            if (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination $artifactDir -Force
            }
        }
        throw "FAIL: WebView2 端到端测试失败（node 退出码 $nodeExit），截图与日志见 $artifactDir"
    }
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $env:MASTERSSH_DATA_DIR = $previousDataDir
    $env:MASTERTERM_CDP_PORT = $previousCdpPort
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
