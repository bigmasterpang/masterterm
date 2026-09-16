# MasterTerm 原生架构状态

## 当前状态

活动产品链路已统一为 Win32、WebView2 和标准 C++：

- `MasterTerm.exe`：原生窗口、WebView2、SSH、串口、SFTP 调度。
- `MasterTermSftpWorker.exe`：独立的 libssh2 文件传输进程。
- `MasterTerm-cli.exe`：原生控制台 SSH 客户端。

## 已验证边界

- 原生 JSON 请求、响应、事件、Base64 输出与 Unicode 代理对。
- UTF-8/UTF-16 Windows 路径转换，包括中文和非 BMP 字符。
- 配置缺失、损坏、并发写入、原子替换失败及不可替换目标路径。
- SSH、串口和 SFTP Worker 的原生构建目标。
- 监控采样与 known_hosts 解析的异常输入（纯函数解析 + 原生回归测试）。
- 全屏终端程序交互（vim/top/less 风格 VT 流在无界面 xterm 引擎下的回归脚本）。
- 可执行文件依赖扫描只包含系统组件、libssh2 及运行库。
- 本地终端会话基于 ConPTY（CreatePseudoConsole + 伪控制台进程属性），子进程
  不继承宿主控制台；关闭窗口隐藏到托盘时后台会话保持存活。
- 系统托盘、终端搜索、SSH 隧道、保活重连与命令收藏的回归均通过原生测试
  目标与 WebView2 端到端验证。

## 维护原则

1. 新功能只使用标准 C++、Win32 和 WebView2 API。
2. 前后端消息保持 JSON 协议兼容。
3. 异步会话、文件传输和窗口销毁必须保持明确的所有权和线程边界。
4. 每次修改均需构建原生目标并运行相关回归测试。

## 统一退出生命周期（T1-1）

主窗口收到 WM_CLOSE（确认退出或托盘“退出”）后按固定顺序拆除，任何一步都
不允许同步等待网络；整体由 8 秒看门狗兜底（超时终止 SFTP Worker 并结束进程，
下次启动清理孤儿 WebView2 进程）：

| 顺序 | 组件 | 拆除动作 | 阻塞边界 |
| --- | --- | --- | --- |
| 1 | 定时器 | 停止后端轮询、尺寸稳定、全屏控制栏定时器 | 无 |
| 2 | 后端门闸 | `beginShutdown()`：拒绝新请求、`poll()` 变空操作 | 无 |
| 3 | RDP | 退出全屏、销毁遮罩窗口；每个会话 `disconnect()` 只置标志，析构时先销毁容器 HWND 再释放 ActiveX（OleInPlace 反激活 + 释放客户端站点，站点比控件后销毁） | 不调用阻塞的 `Disconnect()` |
| 4 | WebView2 | 释放 controller/webview，解除消息/加速键回调 | 无 |
| 5 | SSH 隧道 | 先于 SSH 会话释放（借用了 libssh2 会话指针） | `stop()` 关套接字后 join |
| 6 | SSH 会话 | `closesocket`（不回切阻塞模式）+ libssh2 channel/session 释放 | 不等待网络握手 |
| 7 | 串口会话 | 置停止标志后 join 轮询线程，再关句柄 | 轮询线程 ≤12ms 退出 |
| 8 | ConPTY 会话 | `TerminateProcess` + `ClosePseudoConsole` + 管道句柄 | 等待子进程 ≤1s |
| 9 | SFTP Worker | `NativeProcess` 终止 + 写线程 join | 等待 ≤1s |
| 10 | 托盘/看门狗 | 删除托盘图标，通知看门狗完成 | — |

ProxyJump 转发器（`SshProxyRelay`）的跳板套接字是 `Impl` 成员：`stop()` 会
关闭跳板套接字，中断阻塞中的 connect/握手/认证，退出时 join 不再受 10 秒
套接字超时影响。

真实连接/断开/关闭压力由 `tools/connect-exit-stress.ps1` 驱动：MasterTerm
以隐藏诊断参数 `--exit-self-test` 启动，前端就绪后由后端执行一次真实的
SSH 连接 + 命令回读 + SFTP Worker 目录读取 + ConPTY 本地终端周期，随后完整
退出。退出码约定：`0` 通过、`10` 无可用 SSH 配置（跳过）、`11` 失败。
自检不会自动接受已变更的主机密钥，也不会改动用户配置。

## 统一诊断日志（T1-2）

`src/DiagnosticLog.h` 提供进程内统一脱敏诊断能力，全部写入设置页所示日志目录：

- `diagnostic.log`：追加式事件日志，每条含时间戳、线程 ID/名称、事件和详情；
  覆盖启动（版本/OS/WebView2 运行时版本）、WebView2 就绪、SSH/串口/ConPTY
  会话开闭与状态、SFTP Worker 启停、RDP 打开/关闭/状态、关闭各阶段与看门狗触发。
- 崩溃上下文：固定大小缓冲区记录「当前阶段」与「活动会话」，由未处理异常
  处理器在 `crash.log` 中连同线程 ID/名称一起记录。
- 48 条事件环形缓冲：崩溃时转储到 `crash.log`，用于无完整日志时定位。
- 线程名注册：main、串口轮询、ProxyJump 转发、SFTP 写线程、关闭看门狗。
- 脱敏：事件仅允许 ID、状态、阶段和方法名；控制字符被清洗、长度受限；
  `tests/log-redaction-check.js` 会扫描日志目录下全部 `*.log` 验证无明文凭据。

## 真实 WebView2 端到端测试（T1-3）

- 隐藏环境变量 `MASTERTERM_CDP_PORT`：仅在显式设置时让 WebView2 浏览器进程在
  127.0.0.1 暴露 Chrome DevTools 协议（正常启动不启用）。
- `tools/webview-e2e.ps1` 在隔离 `MASTERSSH_DATA_DIR` 中启动程序并驱动
  `tests/webview-e2e.js`（Node 内置 WebSocket 的 CDP 客户端）：真实点击、
  真实弹窗、真实 SSH 连接、SFTP 面板、ConPTY 本地终端、分屏、RDP 失败
  生命周期与 UI 驱动的退出；失败时保存截图、DOM 摘要并复制诊断日志到
  `build-*/e2e-artifacts`。
- 已接入 CTest（`MasterTermWebViewE2E`，无 SSH 凭据时自动 SKIP）。
- 注意：前端到宿主的桥消息必须是**对象**（WebView2 会对字符串再做一次 JSON
  编码）；自动化侧边栏固定会临时修改共享 WebView2 profile 的
  `masterterm.sidebarMode`，测试结束前恢复原值。

## 快速操作压力测试（T1-4）

`tools/webview-stress.ps1` + `tests/webview-stress.js` 复用 CDP 通道在单个
真实实例内循环：设置弹窗打开/滚动/关闭 → 重复打开同一 RDP 配置（失败生命
周期）→ SSH/RDP 标签来回切换（活动标记跟随）→ 关闭 RDP 标签（激活回落到
健康会话）→ 每 25 次关闭全部标签并重连。每轮校验无错误标签、遮罩、残留
弹窗与分屏状态；结束后经 UI 退出并校验退出码、进程残留与崩溃日志。
100 次循环已通过（`WEBVIEW_STRESS_OK`）。
