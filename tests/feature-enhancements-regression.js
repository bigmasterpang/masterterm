// Feature enhancements regression suite for MasterTerm
// Covers: Multi-local Shell, Quick Switcher, Terminal Theme Presets,
// Broadcast Input, SFTP CD in Terminal, Tab Duplicate & Rename
'use strict';

const fs = require('fs');
const path = require('path');

const readSource = file =>
    fs.readFileSync(path.join(__dirname, '..', file), 'utf8')
        .replaceAll('\r\n', '\n');

const html = readSource('resources/web/index.html');
const css = readSource('resources/web/app.css');
const app = readSource('resources/web/app.js');
const sessionHeader = readSource('src/LocalShellSession.h');
const sessionCpp = readSource('src/LocalShellSession.cpp');
const backendCpp = readSource('src/WebViewBackend.cpp');
const rdpCpp = readSource('src/RdpSession.cpp');
const windowCpp = readSource('src/WebViewWindow.cpp');
const hostCpp = readSource('src/WebViewHost.cpp');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else { ++failures; console.log('FAIL: ' + name); }
}

// 1. Quick Switcher UI & Hotkey
check('index.html exposes quick-switcher-button and quick-switcher-dialog',
    html.includes('id="quick-switcher-button"')
        && html.includes('id="quick-switcher-dialog"')
        && html.includes('id="quick-switcher-input"')
        && html.includes('id="quick-switcher-results"'));

check('app.css includes quick switcher dialog styling and themes',
    css.includes('.quick-switcher-dialog')
        && css.includes('.quick-switcher-item')
        && css.includes('.quick-switcher-item.selected'));

check('app.js implements quick switcher with Ctrl+P/Ctrl+K and fuzzy match',
    app.includes('toggleQuickSwitcher')
        && app.includes('filterQuickSwitcherItems')
        && app.includes('key.toLowerCase() === "p"')
        && app.includes('key.toLowerCase() === "k"'));

// 2. Multi-local Shell & ConPTY custom working directory
check('LocalShellSession supports workingDir parameter',
    sessionHeader.includes('const std::wstring &workingDir')
        && sessionCpp.includes('workingDir.c_str()'));

check('backend exposes local.detectShells and local.connect parameters',
    backendCpp.includes('"local.detectShells"')
        && backendCpp.includes('shellType')
        && backendCpp.includes('workingDir')
        && backendCpp.includes('SearchPathW')
        && backendCpp.includes('powershell.exe')
        && backendCpp.includes('cmd.exe'));

check('index.html and app.js support configurable local shell & working directory',
    html.includes('id="setting-default-local-shell"')
        && html.includes('id="setting-local-working-dir"')
        && html.includes('data-action="new-local-pwsh"')
        && html.includes('data-action="new-local-cmd"')
        && html.includes('data-action="new-local-wsl"')
        && html.includes('data-action="new-local-gitbash"')
        && app.includes('connectLocalTerminal')
        && app.includes('defaultLocalShell')
        && app.includes('localWorkingDir'));

// 3. Real-time Keystroke Broadcast Input
check('index.html and app.css expose broadcast-input-button and active pulsing styling',
    html.includes('id="broadcast-input-button"')
        && css.includes('#broadcast-input-button.active')
        && css.includes('pulse-broadcast')
        && css.includes('.terminal-tab.broadcast-active'));

check('app.js implements broadcast input toggle and session multiplexing',
    app.includes('broadcastInputActive')
        && app.includes('toggleBroadcastInput')
        && app.includes('broadcast-active')
        && app.includes('post("session.input", { sessionId: targetId, data: inputData })'));

// 4. Terminal Color Theme Presets
check('index.html provides setting-terminal-theme-preset with 7 presets',
    html.includes('id="setting-terminal-theme-preset"')
        && html.includes('value="one-dark"')
        && html.includes('value="dracula"')
        && html.includes('value="nord"')
        && html.includes('value="monokai"')
        && html.includes('value="solarized-dark"')
        && html.includes('value="catppuccin-mocha"')
        && html.includes('value="github-dark"'));

check('app.js defines terminalThemePresets and overlays colors in terminalTheme',
    app.includes('terminalThemePresets')
        && app.includes('"one-dark"')
        && app.includes('"dracula"')
        && app.includes('"nord"')
        && app.includes('"monokai"')
        && app.includes('"solarized-dark"')
        && app.includes('"catppuccin-mocha"')
        && app.includes('"github-dark"')
        && app.includes('terminalThemePreset'));

// 5. SFTP "Open in Terminal (cd)" Integration
check('index.html exposes sftp-open-in-terminal and cd-terminal context action',
    html.includes('id="sftp-open-in-terminal"')
        && html.includes('data-action="cd-terminal"'));

check('app.js sends sanitized cd command to active terminal session',
    app.includes('cdInActiveTerminal')
        && app.includes('cd "${sanitizedPath}"')
        && app.includes('#sftp-open-in-terminal'));

// 6. Tab Duplication and Renaming
check('index.html context menu exposes duplicate and rename-tab buttons',
    html.includes('data-action="duplicate"')
        && html.includes('data-action="rename-tab"'));

check('app.js handles duplicate and rename-tab actions',
    app.includes('action === "duplicate"')
        && app.includes('action === "rename-tab"')
        && app.includes('requestText("重命名标签页"'));

// 7. Terminal Background Filling & Theme Coordination
check('app.css and app.js support full terminal background fill and light presets',
    css.includes('--terminal-background')
        && css.includes('var(--terminal-background')
        && app.includes('"github-light"')
        && app.includes('"one-light"')
        && app.includes('"solarized-light"')
        && app.includes('--terminal-tab-active-bg'));

// 8. Quick Switcher Badge First Order & Selection Highlight
check('Quick Switcher places type badge first and has high contrast selection',
    app.includes('el.appendChild(badge);')
        && app.includes('el.appendChild(icon);')
        && app.includes('el.appendChild(title);')
        && css.includes('.quick-switcher-item.selected')
        && css.includes('#2563eb'));

// 9. Multi-Shell Options in Terminal Create Dropdown
check('Terminal tab create menu supports direct opening of pwsh, powershell, cmd, wsl, gitbash',
    app.includes('data-shell')
        && app.includes('availableShells')
        && app.includes('PowerShell 7')
        && app.includes('Windows PowerShell')
        && app.includes('WSL (Linux'));

// 10. SFTP CD Associated SSH Terminal Binding
check('SFTP cd command strictly targets associated SSH session and warns if not found',
    app.includes('activeSftpSessionId')
        && app.includes('未找到该 SFTP 关联的活动 SSH 终端')
        && app.includes('targetSession.sessionId'));

// 11. PowerShell 7 Missing Shell Validation & Safe Handling
check('Backend validates shell executables and frontend detects uninstalled shells safely',
    backendCpp.includes('本地未安装 PowerShell 7 (pwsh.exe)')
        && backendCpp.includes('本地未检测到 WSL')
        && backendCpp.includes('本地未检测到 Git Bash')
        && sessionCpp.includes('m_errorHandler')
        && app.includes('refreshDetectedShells')
        && app.includes('(未安装)')
        && app.includes('btn.dataset.uninstalled'));

// 12. Two-level Cascading Theme Menu
check('Two-level cascading theme menu groups presets under dark, light, and blue with color dots',
    html.includes('class="theme-menu-item-wrap"')
        && html.includes('class="theme-menu-cat-btn"')
        && html.includes('class="theme-submenu"')
        && html.includes('class="theme-preset-btn"')
        && html.includes('class="theme-color-dot"')
        && css.includes('.theme-submenu')
        && css.includes('.theme-menu.open-left')
        && css.includes('.theme-preset-check')
        && app.includes('updateThemeMenuChecks')
        && app.includes('open-left'));

// 13. Quick Switcher Backdrop Click-to-Close & Light Theme UI
check('Quick Switcher closes on outside click and fully adapts badges/kbd in light mode',
    app.includes('isInDialog')
        && app.includes('document.addEventListener("mousedown"')
        && css.includes('html[data-theme="light"] .quick-switcher-esc-badge')
        && css.includes('html[data-theme="light"] .quick-switcher-footer kbd')
        && css.includes('html[data-theme="light"] .quick-switcher-dialog::backdrop'));

// 14. Shortcuts Cheatsheet Dialog & Global Shortcuts
check('Shortcuts dialog exposes cheatsheet, header button, hotkey Ctrl+/ and full styling',
    html.includes('id="shortcuts-dialog"')
        && html.includes('id="shortcuts-help-button"')
        && html.includes('id="shortcuts-close-badge"')
        && css.includes('.shortcuts-dialog')
        && css.includes('html[data-theme="light"] .shortcuts-dialog')
        && app.includes('openShortcutsDialog')
        && app.includes('toggleShortcutsDialog')
        && app.includes('event.key === "/"')
        && app.includes('event.key === "F1"'));

// 15. Workspace Connection Count Badges & Quick Switcher Integration
check('Workspace filter shows live profile counts and quick switcher supports workspace jumping',
    app.includes('renderWorkspaceFilter')
        && app.includes('totalCount')
        && app.includes('counts.get')
        && app.includes('title: `工作区: ${ws}`')
        && app.includes('activeWorkspace = ws;'));

// 16. RDP Dynamic Quality Profiles & Performance Presets
check('RDP options provide quality presets (high, balanced, speed) and automatic config sync',
    html.includes('id="server-rdp-quality-preset"')
        && html.includes('value="high"')
        && html.includes('value="balanced"')
        && html.includes('value="speed"')
        && app.includes('applyRdpQualityPreset')
        && app.includes('rdpQualityPresetSelect')
        && app.includes('rdpQualityPreset:'));

// 17. RDP Modal Isolation, Click-to-Dismiss & Esc Priority
check('RDP modal isolation disables container, forwards dim clicks and prioritizes Esc',
    app.includes('window.hasActiveModal')
        && app.includes('window.dismissActiveModal')
        && rdpCpp.includes('EnableWindow(m_container, m_modalDimmed ? FALSE : TRUE)')
        && rdpCpp.includes('session->m_dismissModalHandler()')
        && windowCpp.includes('dismissActiveModal()')
        && windowCpp.includes('wParam == VK_ESCAPE && m_rdpModalDimmed'));

// 18. Cloud Sync Token Invalidation & Recovery
check('Cloud sync automatically clears invalid tokens and recovers UI on 401',
    app.includes('handleCloudError')
        && app.includes('cloudState.token = ""')
        && app.includes('登录凭证已失效')
        && app.includes('cloudSyncPassword?.addEventListener("keydown"'));

// 19. Unified Dialog Design System & Blue Theme Support
check('Unified dialog design system enforces 10px radius, blur backdrop and blue theme',
    css.includes('dialog::backdrop')
        && css.includes('backdrop-filter: blur(4px)')
        && css.includes('html[data-theme="blue"] .quick-switcher-dialog')
        && css.includes('html[data-theme="blue"] .shortcuts-dialog')
        && css.includes('html[data-theme="blue"] .dialog-actions button'));

// 20. RDP Theme Submenu Parity & Hierarchical Preset Selection
check('RDP theme menu supports hierarchical cascading submenus for presets and updates UI',
    app.includes('post("app.themeMenu", {')
        && app.includes('preset: appSettings.terminalThemePreset || "custom"')
        && app.includes('message.event === "app.nativeThemeSelected"')
        && backendCpp.includes('method == NativeString("app.themeMenu")')
        && backendCpp.includes('currentPreset')
        && windowCpp.includes('HMENU darkMenu = CreatePopupMenu();')
        && windowCpp.includes('HMENU lightMenu = CreatePopupMenu();')
        && windowCpp.includes('HMENU blueMenu = CreatePopupMenu();')
        && windowCpp.includes('themePresetColor')
        && windowCpp.includes('ThemePresetDarkOneDark')
        && windowCpp.includes('ThemePresetLightGithubLight'));

// 21. Terminal Search UI & Engine
check('Terminal search supports keyword search, match counts, case/regex toggles and Ctrl+F',
    html.includes('id="terminal-search"')
        && html.includes('id="terminal-search-input"')
        && html.includes('id="terminal-search-prev"')
        && html.includes('id="terminal-search-next"')
        && html.includes('id="terminal-search-case"')
        && html.includes('id="terminal-search-regex"')
        && css.includes('.terminal-search-btn')
        && css.includes('.terminal-search-toggle')
        && app.includes('openTerminalSearch')
        && app.includes('runTerminalSearch')
        && app.includes('findTerminalMatches')
        && app.includes('createTerminalSearchDecoration'));

// 22. Split Screen Zoom & RDP Coexistence
check('Split screen unblocks with RDP, provides Alt+Z focus zoom and restore banner',
    html.includes('id="split-zoom-banner"')
        && html.includes('id="split-zoom-restore-btn"')
        && css.includes('.split-zoom-banner')
        && app.includes('toggleSplitZoom')
        && app.includes('isSplitZoomed')
        && app.includes('splitZoomBanner')
        && app.includes('split-active')
        && app.includes('🔲 终端分屏'));

// 23. Smart Command Parameter Macros
check('Command snippet parameter macros support dynamic dialog, defaults and live preview',
    html.includes('id="command-macro-dialog"')
        && html.includes('id="command-macro-fields"')
        && html.includes('id="command-macro-preview"')
        && css.includes('.command-macro-dialog')
        && css.includes('.command-macro-preview')
        && app.includes('extractCommandMacros')
        && app.includes('sendFavoriteCommand')
        && app.includes('activeMacroTemplate')
        && app.includes('commandMacroSubmit'));

// 24. RDP System Key Toolbox & 5-Second Auto-Reconnect Countdown
check('RDP supports key toolbox menu, remote utility shortcuts and auto-reconnect state machine',
    html.includes('id="rdp-key-toolbox-btn"')
        && html.includes('id="rdp-key-menu"')
        && css.includes('.rdp-key-menu')
        && css.includes('.rdp-reconnect-countdown')
        && backendCpp.includes('"session.rdpAction"')
        && windowCpp.includes('tool-cad')
        && windowCpp.includes('tool-win')
        && windowCpp.includes('tool-run')
        && windowCpp.includes('tool-explorer')
        && windowCpp.includes('tool-cmd')
        && app.includes('rdpKeyToolboxBtn')
        && app.includes('toggleRdpKeyMenu')
        && app.includes('showRdpDisconnectNotice')
        && app.includes('rdpAutoReconnectAttempts'));

// 25. SFTP Real-time Filter, Type-to-Jump & Anti-Overwrite Conflict Protection
check('SFTP provides real-time filter, type-to-jump indicator and remote edit anti-overwrite protection',
    html.includes('id="sftp-filter-input"')
        && html.includes('id="sftp-jump-indicator"')
        && css.includes('.sftp-filter-input')
        && css.includes('.sftp-jump-indicator')
        && backendCpp.includes('edit.conflict == "prompt"')
        && backendCpp.includes('"conflict_prompt"')
        && app.includes('sftpFilterInput')
        && app.includes('handleSftpTypeToJump')
        && app.includes('sftpJumpBuffer')
        && app.includes('conflict_prompt')
        && app.includes('远程文件修改覆盖确认'));

// 26. Dedicated SFTP Filter Bar & Clear Button
check('SFTP provides dedicated filter bar with clear button and dynamic visibility',
    html.includes('class="sftp-filter-bar"')
        && html.includes('id="sftp-filter-clear"')
        && css.includes('.sftp-filter-bar')
        && css.includes('.sftp-filter-clear')
        && app.includes('sftpFilterClear')
        && app.includes('updateSftpFilterClearVisibility'));

// 27. Split Pane Zoom (Alt+M / Alt+Z) & Alt+1..9 / Ctrl+Tab Tab Switching
check('Shortcuts support Alt+M zoom (with Alt+Z fallback), Alt+1..9 direct tab jump, and Ctrl+Tab cycling',
    html.includes('还原分屏 (Alt+M)')
        && html.includes('<kbd>Alt</kbd>+<kbd>1..9</kbd>')
        && app.includes('switchToTabByIndex')
        && app.includes('getAllOpenTabs')
        && app.includes('cycleTab')
        && app.includes('event.key.toLowerCase() === "m"')
        && app.includes('event.key >= "1" && event.key <= "9"'));

// 28. Native RDP System Key Menu via TrackPopupMenuEx
check('RDP system key menu delegates to native Win32 TrackPopupMenuEx over ActiveX surface',
    backendCpp.includes('"app.rdpKeyMenu"')
        && windowCpp.includes('showRdpKeyMenu')
        && windowCpp.includes('TrackPopupMenuEx')
        && windowCpp.includes('RdpKeyCmdCad')
        && app.includes('"app.rdpKeyMenu"')
        && app.includes('activeRdpVisible()'));

// 29. RDP Coexistence in Split Screen & Background Reconnect with Retry Persistence
check('RDP tabs coexist in split screen and auto-reconnects quietly in background with retry counter persistence',
    css.includes('.terminal-tabs.split-active.has-rdp')
        && app.includes('rdpAutoReconnectAttemptsByProfile')
        && app.includes('split-nav-tab')
        && app.includes('keepBackground')
        && app.includes('isRdpActive'));

// 30. Split Pane Tab Group Flex Horizontal Layout
check('Split pane tab groups enforce flex horizontal row layout and nowrap scrolling',
    css.includes('.split-view > .tab-group')
        && css.includes('flex-direction: row')
        && css.includes('white-space: nowrap')
        && app.includes('defaultGroup = activeGroup'));

// 31. Shortcuts Ctrl+W (Close Tab) & Ctrl+, (Open Settings)
check('Shortcuts Ctrl+W and Ctrl+, are wired across DOM listeners, terminal handler, and native WebView2 accelerators',
    app.includes('closeCurrentTab')
        && app.includes('openSettingsDialog')
        && app.includes('event.key.toLowerCase() === "w"')
        && app.includes('event.key === ","')
        && hostCpp.includes('close-tab')
        && hostCpp.includes('open-settings'));

// 32. Common Command Wildcard Templates & Auto-Completion
check('Terminal provides built-in command templates with wildcards (e.g. find) and styled completion popup',
    app.includes('BUILTIN_COMMAND_TEMPLATES')
        && app.includes('find . -name')
        && app.includes('tar -xzvf')
        && app.includes('isTemplate')
        && (app.includes('常用模板') || app.includes('分段补全'))
        && css.includes('.completion-badge')
        && css.includes('.completion-desc'));

// 33. Segmented Command Completion Engine
check('Terminal implements stepped segmented completion with ArrowRight and placeholder parsing',
    app.includes('parseTemplateSegments')
        && app.includes('activeSegmentedTemplate')
        && app.includes('isNextSegment')
        && app.includes('segmentPrefix')
        && app.includes('hasPlaceholders')
        && css.includes('.completion-placeholder'));

// 34. Restructured Header Actions & Secondary Dropdown Menus
check('Header organizes tools and help into secondary dropdown menus with indicators',
    html.includes('id="tools-menu-button"')
        && html.includes('id="tools-menu"')
        && html.includes('id="help-menu-button"')
        && html.includes('id="help-menu"')
        && html.includes('id="tools-broadcast-indicator"')
        && css.includes('.header-dropdown-menu')
        && css.includes('.header-dropdown-item')
        && app.includes('initHeaderMenus'));

// 35. Terminal Tab Key Decoupling & Native Linux Shell Passthrough
check('Terminal decouples Tab key to prevent DOM focus jumping and preserve native Linux shell completion',
    app.includes('event.key === "Tab"')
        && app.includes('sendSessionInput(session, "\\t")')
        && app.includes('explicitlySelected')
        && app.includes('event.preventDefault()'));

// 36. Multi-Token Semantic Command Matching & Incremental Segment Appending
const vm = require('vm');
const sandbox = {};
const startIdx = app.indexOf('const tokenizeCommandArgs =');
const endIdx = app.indexOf('const completionCandidates =');
let matchLogicOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const codeSnippet = app.slice(startIdx, endIdx);
        vm.runInNewContext(codeSnippet + '; this.tokenizeCommandArgs = tokenizeCommandArgs; this.evaluateTemplateMatch = evaluateTemplateMatch;', sandbox);
        const t1 = { cmd: 'find', value: 'find <path> -type f' };
        const t2 = { cmd: 'find', value: 'find . -name "<*.log>"' };
        const m1 = sandbox.evaluateTemplateMatch('find /root -type', t1);
        const m2 = sandbox.evaluateTemplateMatch('find /root -type', t2);
        matchLogicOk = m1 !== null && m1.incrementalText === ' f' && m2 === null;
    } catch (e) {
        console.error('Match logic eval error:', e);
    }
}
check('Multi-token matching filters out incompatible options and calculates incremental text correctly',
    app.includes('evaluateTemplateMatch')
        && app.includes('tokenizeCommandArgs')
        && app.includes('incrementalText')
        && matchLogicOk);

// 37. Chained Right-Arrow Incremental Completion & -mtime Support
let chainMatchOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const tm = { cmd: 'find', value: 'find <path> -type f -mtime -7' };
        const mm1 = sandbox.evaluateTemplateMatch('find /root -type f -mtime', tm);
        const mm2 = sandbox.evaluateTemplateMatch('find /root -type f -mtime ', tm);
        const mm3 = sandbox.evaluateTemplateMatch('find /root -type f', tm);
        chainMatchOk = mm1 !== null && mm1.incrementalText === ' -7'
            && mm2 !== null && mm2.incrementalText === '-7'
            && mm3 !== null && (mm3.incrementalText === ' -mtime -7' || mm3.incrementalText === ' -mtime ');
    } catch (e) {
        console.error('Chain match eval error:', e);
    }
}
check('Chained completion advances parameters smoothly without reset and supports -mtime -7',
    chainMatchOk
        && app.includes('find <path> -type f -mtime -7')
        && app.includes('hideCompletion(session);')
        && !app.includes('"\\x7f".repeat(session.commandLine.length) + targetPrefix'));

// 38. Terminal Buffer Line Info Extraction & Prompt Synchronization
check('Terminal extracts active buffer line text to sync with shell prompt and track ArrowUp history recalls',
    app.includes('getTerminalBufferLineInfo')
        && app.includes('buffer.baseY + buffer.cursorY')
        && app.includes('textBeforeCursor.match')
        && app.includes('isAtEnd')
        && app.includes('session.commandLine = bufferInfo.commandLine'));

// 39. Whole-Chunk Segment Completion up to Placeholders (ps, journalctl)
let chunkMatchOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const tPs = {
            cmd: 'ps',
            value: 'ps -ef | grep <process_name>',
            segments: [{ prefix: 'ps -ef | grep ', placeholder: '<process_name>' }]
        };
        const tJournal = {
            cmd: 'journalctl',
            value: 'journalctl --since "<today>" -u <service>',
            segments: [
                { prefix: 'journalctl --since "', placeholder: '<today>' },
                { prefix: '" -u ', placeholder: '<service>' }
            ]
        };
        const mPs = sandbox.evaluateTemplateMatch('ps', tPs);
        const mJournal = sandbox.evaluateTemplateMatch('journal', tJournal);
        chunkMatchOk = mPs !== null && mPs.incrementalText === ' -ef | grep '
            && mJournal !== null && mJournal.incrementalText === 'ctl --since "';
    } catch (e) {
        console.error('Chunk match eval error:', e);
    }
}
check('Full continuous segment is completed up to placeholder in one shot (ps -> ps -ef | grep , journal -> ctl --since ")',
    chunkMatchOk
        && app.includes('seg0Lower.startsWith(inputLower)'));

// 40. Strict Template-First Ranking in Completion Candidates
check('Completion candidates prioritize system command templates above user history commands',
    app.includes('rightIsTpl !== leftIsTpl')
        && app.includes('return rightIsTpl ? 1 : -1'));

// 41. Full Multi-Token Option Completion on Selected Template
let fullOptionMatchOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const tFind1 = { cmd: 'find', value: 'find <path> -type f -mtime -1' };
        const mFind1 = sandbox.evaluateTemplateMatch('find /root', tFind1);
        fullOptionMatchOk = mFind1 !== null && mFind1.incrementalText === ' -type f -mtime -1';
    } catch (e) {
        console.error('Full option match eval error:', e);
    }
}
check('Selecting a template completes its full continuous option parameters in one shot (find /root -> -type f -mtime -1)',
    fullOptionMatchOk
        && app.includes('remainingTokens.join(" ")'));

// 42. Generic Service Parameter Completion Safety
let sysctlMatchOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const tSys = { cmd: 'systemctl', value: 'systemctl status <service>' };
        const mSysFull = sandbox.evaluateTemplateMatch('systemctl status frps', tSys);
        const mSysSpace = sandbox.evaluateTemplateMatch('systemctl status ', tSys);
        sysctlMatchOk = mSysFull !== null && mSysFull.incrementalText === ''
            && mSysSpace !== null && mSysSpace.incrementalText === '';
    } catch (e) {
        console.error('Systemctl match eval error:', e);
    }
}
// 43. Positional Subcommand Mismatch Rejection
let subcmdMatchOk = false;
if (startIdx !== -1 && endIdx !== -1) {
    try {
        const tDaemon = { cmd: 'systemctl', value: 'systemctl daemon-reload' };
        const tStatus = { cmd: 'systemctl', value: 'systemctl status <service>' };
        const mDaemon = sandbox.evaluateTemplateMatch('systemctl status', tDaemon);
        const mStatus = sandbox.evaluateTemplateMatch('systemctl status', tStatus);
        subcmdMatchOk = mDaemon === null && mStatus !== null;
    } catch (e) {
        console.error('Subcmd match eval error:', e);
    }
}
check('Incompatible subcommands are strictly rejected (systemctl status does not match daemon-reload)',
    subcmdMatchOk
        && app.includes('!u.startsWith("-") && !isTemplatePlaceholder(t)'));

// 44. Ctrl+C / SIGINT Terminal Line Reset & Dismissal
check('Ctrl+C, Ctrl+U and Ctrl+L reset command line state and dismiss completion popup',
    app.includes('data === "\\x03" || data === "\\x15"')
        && app.includes('session.terminal?.hasSelection?.()'));

// 45. Native Win32 Tools & Help Menus for RDP
check('Tools and help menus delegate to native Win32 popup menus over RDP',
    app.includes('post("app.toolsMenu", {')
        && app.includes('post("app.helpMenu", {')
        && app.includes('message.event === "app.nativeToolsAction"')
        && app.includes('message.event === "app.nativeHelpAction"')
        && backendCpp.includes('method == NativeString("app.toolsMenu")')
        && backendCpp.includes('method == NativeString("app.helpMenu")')
        && windowCpp.includes('void WebViewWindow::showToolsMenu(')
        && windowCpp.includes('void WebViewWindow::showHelpMenu('));

// 46. Dedicated About MasterTerm Dialog with Project Platform
check('Dedicated About MasterTerm dialog introduces app and features master.dapang.wang portal',
    html.includes('id="about-dialog"')
        && html.includes('https://master.dapang.wang')
        && html.includes('id="about-open-portal-btn"')
        && css.includes('.about-dialog')
        && css.includes('.about-portal-section')
        && app.includes('openAboutDialog'));

// 47. Dedicated Standalone Update Dialog & Retained Settings Update
check('Standalone update dialog checks and updates version while settings retains update section',
    html.includes('id="update-dialog"')
        && html.includes('id="update-dialog-changelog"')
        && html.includes('id="check-update-now"')
        && css.includes('.update-dialog')
        && css.includes('.update-download-progress-wrap')
        && app.includes('openUpdateDialog')
        && app.includes('runStandaloneUpdateCheck'));

// 48. Split Screen Return Navigation, Bidirectional Tab Dragging & Recent 5 Updates
check('Split screen navigation allows un-split terminal return, bidirectional tab dragging, splitNav context menu and 5-version changelog limit',
    html.includes('id="split-nav-context-menu"')
        && app.includes('switchToSplitView')
        && app.includes('moveTabToTopLevel')
        && app.includes('showSplitNavContextMenu')
        && app.includes('limitChangelogTo5'));

// 49. Explicit RDP Defaults & Jump Server Dedicated Port and Latency Calculation
check('RDP options show explicit defaults, Jump Server has dedicated section & port, and latency probes jump gateway',
    html.includes('默认 (32 位真彩色)')
        && html.includes('默认 (关闭·原始比例)')
        && html.includes('默认 (自动识别·局域网极速/广域网自适应)')
        && html.includes('默认 (高画质·全特效)')
        && html.includes('默认 (仅全屏时传递)')
        && html.includes('默认 (启用剪贴板同步)')
        && html.includes('默认 (播放到本机)')
        && html.includes('默认 (禁用·保护本机磁盘安全)')
        && html.includes('默认 (禁用·防止多端登录冲突)')
        && html.includes('id="jump-server-fields"')
        && html.includes('id="server-proxy-port"')
        && app.includes('splitProxyJump')
        && app.includes('formatProxyJump')
        && app.includes('ms (跳板)')
        && backendCpp.includes('RemoteLatencyRequest{sessionId, isProxyJump, probeHost}')
        && backendCpp.includes('payload.values.emplace("isProxyJump", true);'));

// 50. Leftmost Sidebar Function Panels Expansion (Snippets, Tunnels, Cloud)
check('Sidebar exposes tabs and panels for snippets, tunnels, and cloud with full interaction logic',
    html.includes('data-panel="snippets"')
        && html.includes('data-panel="tunnels"')
        && html.includes('data-panel="cloud"')
        && html.includes('data-card="snippets"')
        && html.includes('data-card="tunnels"')
        && html.includes('data-card="cloud"')
        && html.includes('id="snippets-list"')
        && html.includes('id="sidebar-tunnels-list"')
        && html.includes('id="cloud-sidebar-logged-in"')
        && css.includes('.snippets-panel-body')
        && css.includes('.tunnels-panel-body')
        && css.includes('.cloud-panel-body')
        && app.includes('renderSnippetsPanel')
        && app.includes('refreshTunnelsPanel')
        && app.includes('updateSidebarCloudUi')
        && app.includes('validPanels = ["connections", "sftp", "snippets", "tunnels", "cloud"]'));

// 51. Startup Safety & TDZ Prevention
check('Startup initialization avoids TDZ and provides defense-in-depth safety',
    app.includes('let renderSnippetsPanel = null;')
        && app.includes('let refreshSidebarTunnelSessions = null;')
        && app.includes('let refreshTunnelsPanel = null;')
        && app.includes('let updateSidebarCloudUi = null;')
        && app.includes('try { renderTransferPanel(); }')
        && app.includes('try { initCloudSidebarPanel(); }')
        && app.includes('startupFallbackTimer')
        && html.includes('前端脚本执行异常：'));

// 52. Terminal Bottom Status Bar & Prompt Elevation
check('Terminal bottom status bar elevates input row and shows session metrics',
    html.includes('id="terminal-status-bar"')
        && html.includes('id="terminal-status-identity"')
        && html.includes('class="terminal-status-separator"')
        && css.includes('.terminal-status-bar')
        && css.includes('.terminal-status-identity')
        && css.includes('.terminal-status-separator')
        && css.includes('padding-bottom: 28px')
        && app.includes('terminalStatusBar = document.querySelector("#terminal-status-bar")')
        && app.includes('terminalStatusBar.hidden = false'));

// 53. RDP Tab Hover Card & Unobstructed Remote Taskbar
check('RDP sessions auto-hide bottom status bar and provide hover preview card',
    html.includes('id="rdp-tab-hover-card"')
        && css.includes('.rdp-tab-hover-card')
        && app.includes('showRdpTabHover')
        && app.includes('hideRdpTabHover')
        && app.includes('rdpTabHoverCard')
        && app.includes('#rdp-tab-hover-card')
        && hostCpp.includes('#rdp-tab-hover-card:not([hidden])'));

// 54. Terminal Tabs Context Menu RDP Delegation & Direct RDP Creation
check('Terminal tabs context menu delegates to native Win32 popup menu in RDP mode and exposes new-rdp option',
    html.includes('data-action="new-rdp"')
        && app.includes('post("session.rdpTabsContextMenu"')
        && app.includes('message.event === "app.nativeTabsContextAction"')
        && app.includes('handleTerminalTabsAction')
        && backendCpp.includes('"session.rdpTabsContextMenu"')
        && windowCpp.includes('showRdpTabsContextMenu')
        && windowCpp.includes('RdpTabsNewRdp')
        && windowCpp.includes('app.nativeTabsContextAction'));

// 55. Snippet Category Badge Light and Blue Theme Adaptation
check('Snippet category badges properly adapt background and borders in light and blue themes',
    css.includes('html[data-theme="light"] .snippet-category-badge')
        && css.includes('html[data-theme="blue"] .snippet-category-badge'));

// 56. Port Forwarding Persistence, Stop-to-Stopped Lifecycle, and Refresh Button
check('Port forwarding persists rules in localStorage, provides stop-to-stopped lifecycle with start/delete, and refresh animation',
    app.includes('masterterm.savedTunnels')
        && app.includes('tunnel-start-btn')
        && app.includes('tunnel-delete-btn')
        && css.includes('.tunnel-start-btn')
        && css.includes('.tunnel-delete-btn')
        && css.includes('.icon-button.refreshing svg')
        && app.includes('sidebar-tunnel-refresh-btn')
        && app.includes('已刷新端口转发通道'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('FEATURE_ENHANCEMENTS_REGRESSION_OK');




