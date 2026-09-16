'use strict';

const fs = require('fs');
const path = require('path');

// Source files may carry CRLF line endings depending on the checkout
// (core.autocrlf).  Multi-line patterns below assume LF, so normalize.
const readSource = file =>
    fs.readFileSync(path.join(__dirname, '..', file), 'utf8')
        .replaceAll('\r\n', '\n');

const root = path.join(__dirname, '..');
const app = readSource('resources/web/app.js');
const windowCpp = readSource('src/WebViewWindow.cpp');
const rdpCpp = readSource('src/RdpSession.cpp');
const rdpHeader = readSource('src/RdpSession.h');
const hostCpp = readSource('src/WebViewHost.cpp');
const backendCpp = readSource('src/WebViewBackend.cpp');
const mainCpp = readSource('src/main.cpp');
const cmake = readSource('CMakeLists.txt');
const resourceRc = readSource('resources/icons/MasterTerm.rc');
const windowHeader = readSource('src/WebViewWindow.h');
const rdpOptionsHeader = readSource('src/RdpConnectionOptions.h');
const indexHtml = readSource('resources/web/index.html');
const appCss = readSource('resources/web/app.css');
const packageScript = readSource('tools/package-release.ps1');

let failures = 0;
function check(name, condition) {
    console.log((condition ? 'PASS: ' : 'FAIL: ') + name);
    if (!condition) ++failures;
}
function between(source, startText, endText) {
    const start = source.indexOf(startText);
    const end = source.indexOf(endText, start + startText.length);
    return start >= 0 && end > start ? source.slice(start, end) : '';
}

const layout = between(
    windowCpp, 'void WebViewWindow::layoutRdpSession(',
    'void WebViewWindow::notifyRdpState(');
const activeLayout = layout.slice(0, layout.indexOf('if (!hosted || !m_rdpHostedVisible)'));
const visibility = between(
    rdpCpp, 'void RdpSession::setHostedVisible(bool visible)',
    'void RdpSession::clearOcclusionRegion()');

check('active RDP occlusion is not cleared before asynchronous bounds query',
    (activeLayout.includes('if (clearOcclusion)') && activeLayout.includes('clearOcclusionRegion()'))
        || (!activeLayout.includes('clearOcclusionRegion()')
            && layout.includes('resize() replaces the\n    // old region')));
check('repeated active layout does not re-raise the RDP HWND',
    visibility.includes('const bool unchanged = m_hostedVisible == visible;')
        && visibility.includes(
            'if (unchanged && m_hostedVisible && IsWindowVisible(m_container))'));
check('already hidden menus are not mutated again during scroll',
    app.includes('const hideVisibleElement = element =>')
        && app.includes('if (!element || element.hidden) return false;')
        && app.includes('const wasVisible = hideVisibleElement(serverContextMenu);')
        && app.includes('hideVisibleElement(sftpContextMenu);'));
check('RDP mutation observer ignores unrelated settings content changes',
    app.includes('const rdpOverlayMutationSelector = [')
        && app.includes('const overlayRecords = records.filter(isRdpOverlayMutation);')
        && app.includes('if (!overlayRecords.length) return;')
        && app.includes('attributeOldValue: true')
        && app.includes('(record.oldValue !== null) !== target.hidden'));
check('host-key acceptance reads the action-dialog result object',
    app.includes('accept: choice.action === "accept"'));
check('initial SSH failures expose their reason in UI and notification',
    app.includes('SSH 连接失败：${session.name} · ${reason}')
        && app.includes('notify("SSH 连接失败", `${session.name}：${reason}`)'));
check('header operation and error messages are transient',
    app.includes('const showTransientStatus = (message, duration = 5000) =>')
        && app.includes('showTransientStatus(`SSH 连接失败：${session.name} · ${reason}`)')
        && app.includes('showTransientStatus(error instanceof Error ? error.message : String(error))')
        && app.includes('showTransientStatus(state.slice(6))')
        && app.includes('if (token === transientStatusToken && status.textContent === value)'));
check('modal layout keeps the RDP desktop and reports a native dim state',
    (hostCpp.includes("const m=document.querySelector('dialog[open]')?1:0;")
        || hostCpp.includes("const m=document.querySelector('dialog[open]:not(.rdp-editor-fallback-freeze)')?1:0;"))
        && !hostCpp.includes('[0,0,innerWidth*d,innerHeight*d]')
        && hostCpp.includes('snapshot.modalOpen = modalOpen != 0.0;'));
check('native modal dim overlay stays above RDP and leaves dialog holes',
    rdpCpp.includes('kDimOverlayClass')
        && rdpCpp.includes('SetLayeredWindowAttributes(m_dimOverlay, 0, 170, LWA_ALPHA)')
        && rdpCpp.includes('updateModalDimOverlay(')
        && rdpCpp.includes('WS_EX_TOOLWINDOW')
        && rdpCpp.includes('kDimOverlayClass, L"", WS_POPUP')
        && rdpCpp.includes('320, 240, m_parent, nullptr, instance, nullptr)')
        && rdpCpp.includes('ClientToScreen(m_parent, &screenOrigin)')
        && rdpCpp.includes('createVisibleRegion(\n        x, y, width, height, occlusionRects)'));
check('native RDP region transitions cannot expose the black container',
    rdpHeader.includes('m_appliedOcclusionRects')
        && rdpCpp.includes('sameOcclusionRegions(')
        && rdpCpp.includes('redundant SetWindowRgn calls are a major source of flashes')
        && rdpCpp.indexOf('SetWindowRgn(m_controlWindow, nullptr, FALSE)')
            < rdpCpp.indexOf('SetWindowRgn(m_container, nullptr, FALSE)')
        && rdpCpp.includes('void RdpSession::redrawOcclusionSurface()')
        && rdpCpp.includes('RDW_NOCHILDREN | RDW_NOERASE'));
check('crash and RDP diagnostics use the Settings log directory',
    mainCpp.includes('NativeDataDir::sessionLogDirectory()')
        && mainCpp.includes('prepareCrashLogging()')
        && mainCpp.includes('defaultCrashLogDirectory()')
        && mainCpp.includes('A portable/custom data root is intentionally isolated')
        && rdpCpp.includes('NativeDataDir::sessionLogDirectory()')
        && rdpCpp.includes('directory / L"rdp.log"'));
check('local and private-LAN RDP sessions force a crisp first full frame',
    rdpCpp.includes('isLoopbackHost')
        && rdpCpp.includes('isLanHost')
        && rdpCpp.includes('octets[0] == 192 && octets[1] == 168')
        && rdpCpp.includes('EnableFrameBufferRedirection')
        && rdpCpp.includes('put_ClientProtocolSpec(FullMode)')
        && rdpCpp.includes('networkType >= 1 ? VARIANT_FALSE : VARIANT_TRUE')
        && rdpCpp.includes('put_NetworkConnectionType(')
        && rdpCpp.includes('client9->UpdateSessionDisplaySettings(')
        && rdpCpp.includes('if (updateDisplaySettings)')
        && rdpCpp.includes('RDW_NOERASE')
        && rdpCpp.includes('RdpInitialSurfaceRepaintTimer')
        && rdpCpp.includes('m_surfaceRefreshPass < 3')
        && rdpCpp.includes('scheduleSurfaceRefresh()')
        && rdpCpp.includes('put_Compress(0)')
        && rdpCpp.includes('if (m_lanConnection)')
        && rdpCpp.includes('m_useMultimon')
        && rdpCpp.includes('keep multimon topology negotiated at Connect()')
        && !rdpCpp.includes('RdpInitialFrameRefreshTimer'));
check('release bundles the matching MSVC runtime for STL synchronization',
    cmake.includes('include(InstallRequiredSystemLibraries)')
        && cmake.includes('CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS')
        && cmake.includes('MASTERTERM_RUNTIME_DLLS'));
check('crash logs include module-relative addresses',
    mainCpp.includes('writeSymbolicAddress')
        && mainCpp.includes('MASTERTERM_VERSION'));
check('frontend assets are embedded for self-recovery',
    resourceRc.includes('IDR_WEB_INDEX RCDATA')
        && resourceRc.includes('IDR_WEB_APP_JS RCDATA')
        && hostCpp.includes('recoverEmbeddedWebAssets()'));
check('updater validates a staging directory before replacing the app',
    backendCpp.includes('masterterm-update-stage-')
        && backendCpp.includes("package web/index.html missing")
        && backendCpp.includes("validated staged install copied"));
check('release packaging uses the compatible system ZIP writer',
    packageScript.includes('Get-Command tar.exe')
        && packageScript.includes('-a -cf $zipPath -C $staging .')
        && !packageScript.includes('Compress-Archive -Path'));
check('packaging rejects a release missing the app-local VC runtime',
    packageScript.includes("'msvcp140.dll'")
        && packageScript.includes("'vcruntime140_1.dll'")
        && packageScript.includes('Missing compiler-matched VC++ runtime'));
check('RDP fullscreen native toolbar follows the selected theme',
    windowHeader.includes('m_rdpFullscreenOverlayTheme')
        && windowCpp.includes('rdpFullscreenOverlayPalette')
        && windowCpp.includes('setApplicationTheme')
        && backendCpp.includes('app.theme')
        && app.includes('post("app.theme", { theme })'));
check('RDP advanced settings expose quality, visual and multi-monitor options',
    rdpOptionsHeader.includes('networkConnectionType')
        && rdpOptionsHeader.includes('desktopComposition')
        && rdpOptionsHeader.includes('useMultimon')
        && indexHtml.includes('server-rdp-network-type')
        && indexHtml.includes('server-rdp-font-smoothing')
        && indexHtml.includes('server-rdp-use-multimon')
        && app.includes('networkConnectionType: document.querySelector')
        && backendCpp.includes('"networkConnectionType"'));
check('RDP advanced settings can restore default overrides and log effective values',
    indexHtml.includes('server-rdp-reset-defaults')
        && indexHtml.includes('恢复 RDP 默认设置')
        && indexHtml.indexOf('server-rdp-reset-defaults') > indexHtml.indexOf('</details>')
        && app.includes('resetRdpAdvancedOptions')
        && app.includes('rdpCheckboxKeys.has(key)')
        && app.includes('field.checked = true')
        && app.includes('rdpResetDefaultsButton.hidden = !rdp')
        && appCss.includes('.dialog-actions .rdp-reset-defaults-action')
        && rdpCpp.includes('quality readback: ColorDepth=')
        && rdpCpp.includes('LAN: EnableFrameBufferRedirection not enabled (loopback only)')
        && rdpCpp.includes('get_NetworkConnectionType(&actualNetworkType)')
        && rdpCpp.includes('get_BandwidthDetection(&actualBandwidthDetection)'));
check('RDP settings dialog keeps the advanced area independently scrollable',
    appCss.includes('#server-dialog { width: min(840px')
        && appCss.includes('#server-form > .form-grid { flex: 1 1 auto; min-height: 0; max-height: none; overflow-y: auto')
        && appCss.includes('overscroll-behavior: contain'));
check('configuration dialogs size to content and scroll long forms internally',
    appCss.includes('dialog[open] { display: flex; flex-direction: column; }')
        && appCss.includes('#settings-form > .settings-body { flex: 1 1 auto;')
        && !appCss.includes('#server-dialog { width: min(840px, calc(100vw - 32px)); height: min(760px'));
check('RDP advanced editor is always visible in a fixed, scrollable viewport',
    appCss.includes('#server-dialog.rdp-advanced-open { position: fixed; inset: 16px 0;')
        && appCss.includes('height: min(720px, calc(100vh - 32px));')
        && appCss.includes('margin: auto; transform: none;')
        && appCss.includes('#server-dialog.rdp-advanced-open > form { flex: 1 1 auto; height: 100%; max-height: none; min-height: 0; }')
        && appCss.includes('#server-form > .form-grid { flex: 1 1 auto; min-height: 0; max-height: none; overflow-y: auto;')
        && app.includes('rdpAdvancedSettings.hidden = !rdp;')
        && app.includes('serverDialog.classList.toggle("rdp-advanced-open", rdp);')
        && indexHtml.includes('<section id="rdp-advanced-settings"')
        && indexHtml.includes('<div class="rdp-advanced-heading">高级配置（可选）</div>'));
check('RDP advanced editor no longer performs a click-driven transition',
    !app.includes('rdpAdvancedSettings.addEventListener("toggle"')
        && !app.includes('rdpAdvancedTransitionPending')
        && !app.includes('post("session.rdpEditorTransition"')
        && !app.includes('createRdpEditorFallbackFreeze')
        && !app.includes('document.startViewTransition(applyTarget)'));
check('RDP advanced transition is committed by native host before acknowledgement',
    hostCpp.includes('int expectedRdpAdvancedEditorState')
        && hostCpp.includes("!v.classList.contains('rdp-advanced-open')")
        && hostCpp.includes("v.classList.contains('rdp-advanced-open')")
        && backendCpp.includes('session.rdpEditorTransition')
        && backendCpp.includes('holds a compositor snapshot')
        && windowCpp.includes('void WebViewWindow::transitionRdpAdvancedEditor(')
        && windowCpp.includes('current->session->resize(')
        && windowCpp.includes('completion(true);'));
check('dialogs clip their rounded corners without a rectangular background',
    appCss.includes('dialog { box-sizing: border-box;')
        && appCss.includes('background-clip: padding-box;')
        && appCss.includes('border-radius: 10px;')
        && appCss.includes('dialog > form { box-sizing: border-box;')
        && appCss.includes('background: inherit; border-radius: inherit;')
        && appCss.includes('#server-dialog, #settings-dialog { background: transparent; border-color: transparent; }')
        && hostCpp.includes('parseFloat(s.borderTopLeftRadius)')
        && hostCpp.includes('RdpOcclusionRegion{')
        && rdpCpp.includes('CreateRoundRectRgn(')
        && rdpCpp.includes('occlusionRegion.cornerRadius'));
check('fullscreen RDP transition does not renegotiate twice from the frontend',
    app.includes('setRdpFullscreen() already performed the single native display')
        && app.includes('notifyRdpLayout(false);')
        && rdpHeader.includes('m_displaySettingsSynchronized')
        && rdpCpp.includes('shouldUpdateDisplaySettings')
        && rdpCpp.includes('scheduleSurfaceRefresh(false)'));
check('terminal settings dialog keeps a compact fixed viewport',
    appCss.includes('.settings-dialog { width: min(620px, calc(100vw - 32px)); height: min(720px, calc(100vh - 32px)); }')
        && appCss.includes('#settings-dialog > form { flex: 1 1 auto; height: 100%; }')
        && appCss.includes('#settings-form .dialog-title, #settings-form .dialog-actions { position: static; }')
        && appCss.includes('#settings-form > .settings-body { flex: 1 1 auto; min-height: 0; overflow-y: auto'));
check('RDP advanced settings are grouped by purpose',
    indexHtml.includes('rdp-advanced-groups')
        && indexHtml.includes('rdp-advanced-group-display')
        && indexHtml.includes('显示与画质')
        && indexHtml.includes('输入与重定向')
        && indexHtml.includes('连接与重连')
        && appCss.includes('.rdp-advanced-groups { display: grid;')
        && appCss.includes('.rdp-advanced-group-display { grid-column: 1 / -1; }'));
check('RDP advanced settings are applied to ActiveX performance flags',
    rdpCpp.includes('defaultVisualOption(options.desktopBackground')
        && rdpCpp.includes('defaultVisualOption(options.fontSmoothing')
        && rdpCpp.includes('PerformanceFlags=')
        && rdpCpp.includes('put_UseMultimon')
        && rdpCpp.includes('configuredNetworkType'));
check('RDP multi-monitor mode is negotiated when the session opens',
    windowHeader.includes('m_rdpFullscreenUseMultimon')
        && windowCpp.includes('m_rdpFullscreenUseMultimon = hosted->options.useMultimon != 0')
        && windowCpp.includes('recreateRdpSession(sessionId, true)')
        && windowCpp.includes('recreateRdpSession(sessionId, false)')
        && windowCpp.includes('multimon-container')
        && windowCpp.includes('container-handled multimon; WebView hidden')
        && windowCpp.includes('setWebViewVisible(false)')
        && !windowCpp.includes('GetSystemMetrics(SM_CXVIRTUALSCREEN)')
        && rdpCpp.includes('MasterTerm container owns fullscreen')
        && rdpCpp.includes('put_ContainerHandledFullScreen(')
        && rdpCpp.includes('container fullscreen validation: control=')
        && windowCpp.includes('WS_EX_TOPMOST')
        && windowCpp.includes('restored single-monitor session after fullscreen')
        && !rdpCpp.includes('if (options.useMultimon >= 0)'));

if (failures) process.exit(1);
console.log('OVERLAY_SSH_REGRESSION_OK');
