# MasterTerm

<div align="center">

**极致轻量、现代全能的 Windows 桌面终端工作台与 Linux/DevOps 运维利器**

[![Version](https://img.shields.io/badge/version-v0.1.129-blue.svg)](https://github.com/bigmasterpang/masterterm/releases/tag/v0.1.129)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6.svg)](https://github.com/bigmasterpang/masterterm)
[![Architecture](https://img.shields.io/badge/architecture-Win32%20%2B%20WebView2-success.svg)](https://github.com/bigmasterpang/masterterm)
[![Size](https://img.shields.io/badge/portable%20size-~4.8%20MB-orange.svg)](https://github.com/bigmasterpang/masterterm/releases)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[📦 立即下载 (GitHub Releases)](https://github.com/bigmasterpang/masterterm/releases) &nbsp;|&nbsp; [🚀 Gitee 镜像下载](https://gitee.com/bigmasterwang/masterterm/releases) &nbsp;|&nbsp; [📝 历史更新日志](CHANGELOG.md)

</div>

---

## 💡 为什么选择 MasterTerm？

在日常开发与服务器运维中，常见的终端软件往往存在**体积臃肿、启动缓慢（动辄几百兆 Electron 内存黑洞）**，或者**功能割裂（终端、SFTP、远程桌面、本地脚本无法统一管理）**的问题。

**MasterTerm** 专为追求极致性能与工作效率的工程师打造：
- **🚀 极度轻巧**：便携版仅约 **4.8 MB**，安装包仅约 **3.8 MB**！原生 Win32 + Evergreen WebView2 驱动，毫秒级瞬间冷启动，后台待机内存占用极低。
- **☁️ 开箱即用云端备份**：内置配置、会话列表与偏好设置的云端同步与多历史快照备份，提供官方公益免费节点（支持一键回滚历史），亦支持零依赖私有化部署。
- **⚡ 生产级运维命令库**：内置 70+ 个高频运维命令模板，独创**智能分段补全（Segmented Completion）**与动态参数宏，运维排障行云流水。
- **💻 `MasterTerm-cli.exe`**：无界面原生命令行工具，**超越并替代 Windows 自带的 OpenSSH 客户端**。支持会话别名秒连、中文路径无乱码、智能超时控制与专门针对 AI Agent / 自动化运维设计的单行结构化 JSON 输出模式。
- **🔲 多协议自适应分屏工作台**：深度融合 SSH、串口 (Serial)、本地多 Shell（PowerShell 7 / CMD / WSL / Git Bash）与嵌入式 Windows RDP 远程桌面，支持灵活的双向自由拖拽分屏。
- **📁 内置高性能 SFTP 文件管理器**：连接自动挂载、双向拖拽传输、断点续传、后台传输中心调度，并支持本地软件双击远程编辑与静默回写。

---

## ✨ 核心特性一览

### 1. ⚡ 极致小巧，纯粹性能
- **超小体积**：无需安装庞大的运行时，绿色解压便携包仅 **4.8 MB**，安装包仅 **3.8 MB**。
- **瞬时唤醒**：告别 Electron 类终端 150MB+ 的冷启动与数百兆内存占用，采用 Win32 原生底层消息循环结合系统内置 WebView2 渲染，极低系统开销。
- **流畅渲染**：基于成熟的 xterm.js 现代终端引擎，全彩 ANSI/VT 序列支持、GPU 硬件加速渲染，面对百万行高速日志输出不卡顿、不丢字。

### 2. ☁️ 安全可靠的云端备份与恢复
- **多设备无缝同步**：一键将服务器连接列表、工作区分组、软件设置及自定义命令库备份至云端，换电脑或重装系统秒级还原。
- **公益免费备份服务**：社区官方提供稳定可靠的公益免费同步服务（`https://vm.dapang.wang/api`），即开即用。
  > **公益服务声明与配额机制**：为防止恶意脚本和无序上传刷满存储，普通用户账号默认**限额保留最近 3 个历史备份快照**。可在【版本历史】中自由重命名、一键回滚或删除旧快照；
- **零知识硬件级机密保护**：所有敏感凭据（服务器密码、SSH 私钥内容、跳板机认证信息等）在离开本机前**全部由 Windows 原生 DPAPI 硬件密钥高强度加密**，云端仅存储不可逆密文，服务端无法解密，杜绝明文泄露风险。
- **支持私有化自建节点**：内置零依赖 Python 3 标准库后端服务（仅需一个文件 [`cloud/cloud-sync.py`](cloud/cloud-sync.py) 与 SQLite），企业或个人可分钟级完成私有节点搭建。

### 3. 🛠️ 运维命令矩阵与智能分段输入（杀手级功能）
- **70+ 高频生产级运维命令库**：全面涵盖系统巡检、性能分析、网络排障、容器 Docker/K8s、文件归档（tar/zip/find）、日志检索（grep/journalctl）与权限控制（chmod/chattr），配有精准的中文释义与语法高亮。
- **独创智能分段自动补全（Segmented Tab Completion）**：
  - 自动识别命令中的固定关键字与 `<placeholder>` 变量占位符；
  - 按下 `Tab` 或方向右键 `→` 时，优先自动补全固定前缀（例如 `tar -xzvf `），用户输入具体参数后，再次按右键继续步进补全下一段固定选项（如 ` -C `），彻底告别传统终端回退退格修改 dummy 字符的痛苦体验！
  - 严格子命令匹配与过滤机制，输入 `systemctl status` 绝不会错误提示 `daemon-reload`。
- **动态参数宏模板（`{{VAR}}` / `{{VAR:default}}`）**：支持在自定义命令片段中预埋动态参数，点击执行时自动弹出参数填入对话框，提供实时预览与历史参数记忆。
- **全局按键实时广播（Broadcast Input）**：顶部一键开启广播输入模式，键盘敲击的每一个按键实时同步推送至所有连接的命令行终端，集群批量巡检与部署利器。

### 4. 💻 `MasterTerm-cli.exe` —— Windows 自带 SSH 的更强替代者
`MasterTerm-cli.exe` 是项目中随附编译的纯原生命令行可执行文件，可完全取代系统自带的 `ssh.exe`：
- **会话别名秒级直连**：无需每次输入繁琐的 IP、端口和密钥路径，直接读取已保存的会话名快速登录：
  ```powershell
  MasterTerm-cli.exe --profile 生产服务器
  ```
- **彻底告别 Windows 中文乱码**：底层深度处理 UTF-8、ANSI 代码页与路径转义，无论是远程命令输出还是包含中文的文件传输，均能保持字符绝对无损。
- **专为 AI Agent 与自动化脚本设计的 `--agent` 模式**：
  - 在自动化流水线、脚本调度或 LLM Agent（如 Claude / OpenCode / ChatGPT）协同调用时，传统 ssh 常因交互式密码提示或未知提示符导致整个调用卡死。
  - `--agent` 模式在后台静默执行单条命令，**严格输出单行结构化 JSON**（包含 `exitCode`、`stdout`、`stderr`、`durationMs`），杜绝交互弹窗与输出污染：
  ```powershell
  $env:MASTERTERM_SSH_PASSWORD = "your_password"
  MasterTerm-cli.exe --agent --profile 生产服务器 --exec "docker ps -a"
  ```
- **增强型无交互 SCP 传输**：支持大文件断点续传检测、连接超时保护与全自动跳板机穿透传输：
  ```powershell
  MasterTerm-cli.exe --scp-upload ./dist.zip user@remote.server.com /data/releases/
  MasterTerm-cli.exe --scp-download /var/log/syslog.log user@remote.server.com ./syslog.log
  ```

### 5. 🔲 现代工作台与全能多协议分屏
- **多协议原生融合**：
  - **SSH / SFTP**：支持密码、私钥、ProxyJump 多级跳板机、自动保活与断线重连；
  - **串口 (Serial COM)**：自动探测 Windows 当前可用 COM 端口，即插即用；
  - **本地多 Shell 支持**：基于 Windows ConPTY 架构原生集成 Windows PowerShell、PowerShell 7 (pwsh)、CMD、WSL Linux 与 Git Bash；
  - **嵌入式 Windows 原生 RDP**：深度集成系统级远程桌面控件，自适应分辨率缩放，局域网高画质加速。
- **双向自适应拖拽分屏**：
  - 支持左右垂直分屏、上下水平分屏；
  - 分屏窗格与顶层独立连接栏之间支持**双向标签自由拖拽**，分屏窗格拖空时自动优雅合并，独立全屏终端与分屏视图一键快速切换；
  - 支持快捷键 `Alt+M` 对当前活动子分屏进行单窗格全屏放大/还原；
  - 针对 RDP 远程桌面研发了 Win32 原生层级菜单代理，彻底解决 ActiveX 画面遮挡下拉菜单的技术痛点。
- **全局快速跳转与命令面板 (`Ctrl+P` / `Ctrl+K`)**：
  - 支持全拼、简拼与模糊关键词匹配，键入即可在数毫秒内跳转到任意远程会话、本地 Shell、触发批量脚本或直达功能设置。

### 6. 📁 内置高性能 SFTP 文件管理器
- **SSH 连带自动挂载**：登录成功自动在右侧展开远端用户主目录，支持独立双栏模式与远程单栏模式自由切换。
- **独立后台传输中心**：底部分离式传输面板，包含当前排队队列、传输中任务速率曲线、断点续传、智能重试与传输历史归档。
- **本地软件双击远程打开**：双击远程配置文件即可使用系统默认关联程序（如 VS Code、Notepad++ 等）打开，本地保存后 MasterTerm 后台自动无感将修改安全回写远程主机。
- **右键智能导航与资源管理器拖拽上传**：从 Windows 资源管理器将任意文件拖入窗口即可自动加入上传队列；支持一键在当前终端快速 `cd` 进入当前 SFTP 所在目录。

---

## 📥 下载与安装

### 正式发布版本 (v0.1.129)

| 版本类型 | 文件名称 | 适用场景 | 下载地址 |
| :--- | :--- | :--- | :--- |
| **Windows 一键安装包** | `MasterTerm-0.1.129-Setup.exe` (~3.8 MB) | 推荐日常使用，自动配置开始菜单、桌面图标与系统集成 | [下载 Setup.exe](https://github.com/bigmasterpang/masterterm/releases/download/v0.1.129/MasterTerm-0.1.129-Setup.exe) |
| **绿色便携免安装版** | `MasterTerm-0.1.129-windows-x64.zip` (~4.8 MB) | 解压即用，适合放入 U 盘随身携带或移动办公 | [下载 Portable.zip](https://github.com/bigmasterpang/masterterm/releases/download/v0.1.129/MasterTerm-0.1.129-windows-x64.zip) |

> 镜像加速：若访问 GitHub 较慢，亦可在 [Gitee Releases](https://gitee.com/bigmasterwang/masterterm/releases) 镜像源下载。

### 运行环境要求
- **操作系统**：Windows 10 / Windows 11 (64位)
- **WebView2 运行时**：Windows 11 及更新版 Edge 均已内置 Evergreen WebView2 Runtime。如果系统缺失，可在首次运行时安装官方引导程序。

---

## ⚡ 快速使用指南

### 启动 MasterTerm
解压绿色压缩包或安装后运行 `MasterTerm.exe`。发布包结构清晰自洽：
```text
MasterTerm/
├─ MasterTerm.exe             # 图形化主程序（极速启动）
├─ MasterTermSftpWorker.exe   # 高性能多线程 SFTP 传输工作进程
├─ MasterTerm-cli.exe         # 无界面增强型 SSH/SCP 命令行工具
└─ web/                       # 前端渲染资源与主题组件
```

### 快捷键速查

| 快捷键 | 功能描述 |
| :--- | :--- |
| `Ctrl+P` / `Ctrl+K` | 唤出全局快速跳转面板（模糊搜索服务器、会话与工具） |
| `Ctrl+,` | 快速打开系统设置对话框 |
| `Ctrl+W` | 关闭当前会话标签页 |
| `Ctrl+F` | 终端内原生长文关键词高亮查找（支持大小写与正则） |
| `Alt+M` | 快速放大当前分屏子窗格（再按一次还原） |
| `Ctrl+Tab` | 在已打开的各个会话标签页间顺序切换 |
| `Alt+1 ~ 9` | 快速直接跳转至对应序号的会话标签 |
| `Tab` / `→` | 自动补全当前高亮或推荐的常用运维命令 |

### `MasterTerm-cli.exe` 常用命令示例

```powershell
# 1. 查看帮助文档
MasterTerm-cli.exe --help

# 2. 直接根据配置好的会话名登录（无需记忆 IP/密码）
MasterTerm-cli.exe --profile 数据库生产服

# 3. 直连远程执行快速命令并返回输出
MasterTerm-cli.exe --exec "free -m && df -h" root@192.168.1.100

# 4. JSON 纯净模式（去除所有 ANSI 颜色控制字符）
MasterTerm-cli.exe --json --strip-ansi --exec "uname -a" user@server.com

# 5. 面向自动化流水线 / AI 智能体的 Agent 模式（输出单行格式化 JSON）
$env:MASTERTERM_SSH_PASSWORD = "your_password"
MasterTerm-cli.exe --agent --exec "docker ps" user@server.com

# 6. 使用跳板机及指定私钥上传文件
MasterTerm-cli.exe --identity "C:\keys\id_rsa" --scp-upload ./app.tar.gz root@10.0.0.5 /data/
```

---

## 📋 最近版本更新记录（近 5 次迭代）

### v0.1.129 (2026-09-16)
- **启动时 TDZ ReferenceError 异常修复与全链路防御加固**：彻底消除前端脚本启动时访问未提升变量引发的异常，修复启动界面加载超时提示；为侧栏各面板增加 try-catch 隔离与 3.5 秒启动兜底守护；
- **旧版本检查更新卡死根因彻底修复**：严格收敛服务器清单 Release Notes 体积至 2800 字节以内，保障包括 v0.1.126 在内的早期版本顺畅检查更新与升级。

### v0.1.128 (2026-09-16)
- **侧边栏新增三大核心运维面板**：集成常用运维命令库 (`snippets`)、SSH 端口转发中心 (`tunnels`) 与云备份历史 (`cloud`)；
- **RDP 远程桌面高级设置默认项语义明确化**：针对全屏键盘钩子、剪贴板、音频传递、磁盘重定向等关键参数全面补齐中文行为说明；
- **跳板机 (Jump Server) 独立端口输入与网关延迟探测**：独立端口输入框与网关延迟探测管线。

### v0.1.127 (2026-09-15)
- **公益备份服务声明与安全防护全面落地**：官方文档与界面正式明确 `vm.dapang.wang` 公益免费服务性质与使用指南；
- **普通用户云端备份 3 个配额限制**：服务端与前端双向建立配额拦截与提示，防止恶意刷量占用存储空间；
- **全链路漏洞防护与安全加固**：增加客户端/服务端 5MB 大包上限拦截防内存 DoS、私钥文件名白名单防 Windows 路径穿越注入，以及服务端 IP 滑动窗口防暴力破解限流。

### v0.1.126 (2026-09-15)
- **顶栏独立终端切回分屏视图修复**：彻底修复活动窗口处于顶栏未分屏终端时点击“🔲 终端分屏”无法切入的问题，实现鼠标点击与 `Ctrl+Tab` 行为一致；
- **分屏窗格与顶层未分屏栏标签双向自由拖拽**：打通分屏内部标签直接拖至顶层独立栏，以及顶栏标签拖入分屏窗格的双向拖拽排版链路；
- **“🔲 终端分屏”标签专属右键菜单**：为顶栏分屏标签绑定右键快捷切换、关闭与批量命令功能。

### v0.1.125 (2026-09-15)
- **模态弹窗垂直居中视觉统一**：关于、检查更新、快速跳转与快捷键速查中心在所有高分辨率宽屏下全面统一为居中排版；
- **更新检测对话框布局自适应重构**：底部操作按钮永久固定可视，消除长日志溢出；
- **分屏模式顶栏独立新建会话**：在下方已开启分屏时，顶栏新建的终端精准作为全屏独立终端呈现，互不干扰。

### v0.1.124 (2026-09-15)
- **RDP 视图 Win32 原生菜单代理深度优化**：彻底解决 Direct3D 硬件加速下底层 ActiveX 画面造成的下拉菜单遮挡、裁切黑边及残影缺陷；
- **专属独立“关于 MasterTerm”与“检查更新”弹窗解耦**：设立毛玻璃质感专属关于弹窗与独立一键版本检测升级流程。

### v0.1.122 (2026-09-15)
- **子命令严格匹配过滤（解决 systemctl status 误显 daemon-reload 问题）**：模板匹配引擎建立位置参数打分，杜绝不相关子命令误推荐；
- **Ctrl+C / Ctrl+U / Ctrl+L 终端中断信号实时重置**：彻底消除终端打断输入后补全残留与光标错位。

> 📖 查看完整历史版本记录请参阅 [CHANGELOG.md](CHANGELOG.md)。

---

## 🛠️ 源码构建与开发

本项目采用现代标准 C++（C++20）与原生 Win32 API 构建，不依赖任何重型第三方 GUI 库。

### 构建准备
- 操作系统：Windows 10 / Windows 11 x64
- 编译器：Visual Studio 2022 (MSVC x64)
- 工具链：CMake 3.21+、Ninja、Node.js（运行自动化回归套件）
- 依赖库：通过 CMake 链接已配置的 `libssh2`、`OpenSSL` 与 `zlib`

### 一键构建与打包
在 **Developer PowerShell for VS 2022** 中执行：

```powershell
# 1. 配置并编译 Release 版本
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release --parallel 4

# 2. 运行自动化全量测试套件（22 项原生及端到端测试）
ctest --test-dir build-release -C Release --output-on-failure

# 3. 打包绿色便携版 ZIP 与 Setup.exe 安装包
powershell -ExecutionPolicy Bypass -File .\tools\package-installer.ps1 -BuildDir build-release -Version 0.1.127
```

构建生成的安装包与绿色包将输出至 `build-release\dist\` 目录下。

---

## 📄 开源许可证

本项目遵循 [MIT License](LICENSE) 开源许可证。欢迎提交 Issue 与 Pull Request 共同完善 MasterTerm！
