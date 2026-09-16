# MasterTerm 开发环境搭建（新电脑）

> 在另一台电脑上继续开发前的完整环境准备清单。
> 当前开发任务和执行顺序见 `docs/MASTERTERM-TASK-PLAN.md`。

## 1. 所需软件清单

| 软件 | 版本/位置（本机参考） | 说明 |
|---|---|---|
| Windows 10/11 x64 | — | 目标系统 |
| Visual Studio 2022 Build Tools | MSVC 14.44（`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`） | 需含 **C++ 桌面开发** 工作负载 |
| CMake | ≥3.21（本机 3.31） | 构建系统 |
| Git | ≥2.30（本机 2.55） | 拉取源码 |
| Node.js | ≥16（本机 v22） | 跑 JS 回归测试 |
| conda/miniforge（含 Python 3.x） | `D:\miniforge3`（本机） | **提供 libssh2 依赖**（见 §3） |
| WebView2 SDK | 仓库自带 `third_party/webview2/package-1.0.4078.44` | **无需单独安装** |

> 注意：**不推荐用 vcpkg**。CMakeLists 预置了 vcpkg 路径，但本机实际用 conda 的 libssh2 构建成功。保持与现有机器一致可避免 CMake 找不到包。

## 2. 安装步骤

### 2.1 VS 2022 Build Tools（含 C++ 工具链）
1. 下载 https://visualstudio.microsoft.com/visual-cpp-build-tools/
2. 勾选工作负载：**使用 C++ 的桌面开发**（含 MSVC x64 编译器、Windows SDK、CMake 工具）
3. 安装后确认：`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"` 存在

### 2.2 Git
```powershell
winget install Git.Git
```
配置（可选）：
```powershell
git config --global user.name "bigmasterwang"
git config --global user.email "你的邮箱"
```

### 2.3 Node.js
```powershell
winget install OpenJS.NodeJS.LTS
```

### 2.4 CMake
VS BuildTools 已含 CMake（`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`）。
若用独立 CMake：`winget install Kitware.CMake` 并加入 PATH。

### 2.5 conda（提供 libssh2）
```powershell
# 安装 miniforge：https://github.com/conda-forge/miniforge/releases
# 创建环境并安装 libssh2
conda create -n masterterm python=3.12
conda activate masterterm
conda install -c conda-forge libssh2
```
确认 libssh2 位置（conda 环境的 Library 目录）：
```
conda info --envs   # 记下环境路径，例如 D:\miniforge3\envs\masterterm
# libssh2 应在 <env>\Library\include\libssh2.h 和 <env>\Library\lib\cmake\libssh2\
```

## 3. 获取源码

```powershell
git clone https://gitee.com/bigmasterwang/masterterm.git
cd masterterm
git checkout main
```

仓库自带（无需下载）：
- `third_party/webview2/` — WebView2 SDK（1.0.4078.44）
- `resources/web/vendor/xterm/` — 内置 xterm.js
- `cloud/`、`tools/`、`tests/` — 脚本与测试

## 4. 首次构建

### 4.1 设置 conda 的 libssh2 可见
CMake 通过 `CMAKE_PREFIX_PATH` 找 libssh2。两种方式任选：

**方式 A（推荐，与现有机器一致）**：把 conda 的 Library 加入环境变量，再运行 cmake：
```powershell
conda activate masterterm
$env:CMAKE_PREFIX_PATH = "$env:CONDA_PREFIX\Library"
```

**方式 B**：cmake 时显式指定：
```powershell
cmake -S . -B build-msvc-webview2-native `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="<conda环境>\Library"
```

> 若找不到 libssh2（`find_package(libssh2 CONFIG REQUIRED)` 失败），确认环境路径正确。
> conda 的 libssh2 需要 zlib/openssl，`conda install libssh2` 会自动带。

### 4.2 构建（Debug）
```powershell
cmd /d /s /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"" -arch=x64 >nul && cmake --build build-msvc-webview2-native --config Debug --parallel 4"
```
产物：
- `build-msvc-webview2-native\MasterTerm.exe`（主程序）
- `MasterTermSftpWorker.exe`（SFTP worker）
- `MasterTerm-cli.exe`（命令行工具）
- 各 `MasterTermNative*Test.exe`（原生测试）
- `web\`（前端资源，构建时自动拷贝）

### 4.3 构建（Release，发布用）
```powershell
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<conda环境>\Library"
cmake --build build-release --config Release --parallel 4
```

## 5. 测试

```powershell
# ctest（含 JS 回归：全屏终端、分屏布局、日志脱敏）
cmd /d /s /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"" -arch=x64 >nul && ctest --test-dir build-msvc-webview2-native --output-on-failure"

# 原生测试
foreach ($t in @('MasterTermNativeFileTest','MasterTermNativeJsonTest','MasterTermNativeMonitorTest','MasterTermNativeProcessTest','MasterTermNativeSerialTest')) {
  & ".\build-msvc-webview2-native\$t.exe"
}

# 前端语法
node --check resources\web\app.js

# 打包（发布）
.\tools\package-release.ps1 -BuildDir .\build-release

# 发布更新到服务器（需 ssh 凭据）
.\tools\publish-update.ps1 -BuildDir .\build-release
```

## 6. RDP 功能特殊要求

- **RDP 控件注册**（嵌入式远程桌面需要）：
  ```
  regsvr32 C:\Windows\System32\mstscax.dll   （管理员）
  ```
  不注册时 RDP 连接会提示“RDP 控件不可用”。程序会在控件不可用时提示管理员注册命令；不应自动修改系统注册信息。
- RDP 测试目标：使用本地测试配置或由测试人员提供的地址和凭据；不要把密码写入文档。
- RDP/崩溃日志：以程序设置中显示的日志目录为准；不要在文档或日志中写入凭据。

## 7. 常用注意事项

- **改前端（app.js/index.html/css）后必须重新构建**，CMake 会把 `resources/web` 拷到 `build-*/web/`
- **运行中的程序会锁 exe**，重新构建前先结束 MasterTerm 进程：
  ```powershell
  Get-Process MasterTerm -ErrorAction SilentlyContinue | Stop-Process -Force
  ```
- 点窗口 × 只是最小化到托盘，**测试新构建务必完全退出**（托盘右键退出或任务管理器结束）
- 云同步/自动更新服务器：开发测试时可配置自己的测试服务或跳过
- SSH 测试服务器：可使用自己的 Linux 测试机或本地容器（配置对应私钥即可）

## 8. 关键文件导航

| 文件 | 内容 |
|---|---|
| `docs/MASTERTERM-TASK-PLAN.md` | 当前任务计划和验收标准 |
| `docs/NATIVE-MIGRATION-STATUS.md` | 原生架构边界和维护原则 |
| `docs/FEATURE-MIGRATION-PLAN.md` | 已完成的历史迁移记录 |
| `src/RdpSession.*` | RDP 嵌入（开发中） |
| `src/WebViewWindow.cpp` | 主窗口/RDP 协调 |
| `src/WebViewBackend.cpp` | 后端方法（rdp/cloud/update/sftp） |
| `src/masterterm_cli.cpp` | CLI（exec/SCP） |
| `resources/web/app.js` | 前端逻辑 |
| `CMakeLists.txt` | 构建配置（版本号 `project(MasterTerm VERSION x.y.z)`） |
