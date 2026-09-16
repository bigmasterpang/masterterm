// Sidebar auto-hide regression sentinel. The flyout must overlay the terminal
// and clip the native RDP surface without resizing or refreshing it.
'use strict';

const fs = require('fs');
const path = require('path');

// Source files may carry CRLF line endings depending on the checkout
// (core.autocrlf).  Multi-line patterns below assume LF, so normalize.
const readSource = file =>
    fs.readFileSync(path.join(__dirname, '..', file), 'utf8')
        .replaceAll('\r\n', '\n');

const root = path.join(__dirname, '..');
const html = readSource('resources/web/index.html');
const css = readSource('resources/web/app.css');
const app = readSource('resources/web/app.js');
const host = readSource('src/WebViewHost.cpp');
const backend = readSource('src/WebViewBackend.cpp');
const windowCpp = readSource('src/WebViewWindow.cpp');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else { ++failures; console.log('FAIL: ' + name); }
}

function functionBody(source, signature, nextSignature) {
    const start = source.indexOf(signature);
    const end = source.indexOf(nextSignature, start + signature.length);
    return start >= 0 && end > start ? source.slice(start, end) : '';
}

const showBody = functionBody(
    app, 'const showSidebarFlyout =', 'const hideSidebarFlyout =');
const hideBody = functionBody(
    app, 'const hideSidebarFlyout =', 'const scheduleSidebarFlyoutOpen =');
const scheduleCloseBody = functionBody(
    app, 'const scheduleSidebarFlyoutClose =', 'const setSidebarAutoHide =');
const activateBody = functionBody(
    app, 'const activateFunctionPanel =', 'const setLocalPanelVisible =');
const autoHideBody = functionBody(
    app, 'const setSidebarAutoHide =', 'const activateFunctionPanel =');
const reflowBody = functionBody(
    windowCpp, 'void WebViewWindow::reflowAllRdpSessions(',
    'void WebViewWindow::notifyRdpState(const std::wstring &state)');
const focusBody = functionBody(
    app, 'const focusSession =', 'const refitSplitViews =');

check('sidebar exposes pin, SFTP source selector, badge and occlusion marker',
    html.includes('id="sidebar-pin"')
        && html.includes('id="sidebar-sftp-session"')
        && html.includes('id="sftp-function-badge"')
        && html.includes('id="sidebar-flyout-occlusion"'));
check('auto-hide layout leaves only the icon rail beside the terminal',
    css.includes('.sidebar-workspace.sidebar-auto-hide { grid-template-columns: 42px minmax(0, 1fr); }')
        && css.includes('.sidebar-workspace.sidebar-auto-hide > .terminal-card { grid-column: 2; }'));
check('auto-hidden panel is an absolute overlay',
    css.includes('.sidebar-workspace.sidebar-auto-hide > .function-panels { position: absolute;')
        && css.includes('width: var(--sidebar-flyout-width, 340px)'));
check('native RDP reserves the dedicated sidebar overlay rectangle',
    host.includes('#sidebar-flyout-occlusion:not([hidden])'));
check('RDP clipping is installed before the flyout is revealed',
    showBody.includes('sidebarFlyoutOcclusion.hidden = false')
        && showBody.includes('notifyRdpLayout(false)')
        && showBody.includes('classList.add("sidebar-flyout-open")'));
check('RDP clipping remains until the close animation finishes',
    hideBody.includes('classList.remove("sidebar-flyout-open")')
        && hideBody.includes('sidebarFlyoutOcclusion.hidden = true')
        && hideBody.includes('notifyRdpLayout(false, true)'));
check('brief edge hovers cancel pending flyout and native clipping',
    scheduleCloseBody.includes('clearTimeout(sidebarFlyoutOpenTimer)')
        && scheduleCloseBody.includes('hideSidebarFlyout(true)'));
check('flyout resizer counts as part of the hover surface',
    app.includes('sidebarResizer.addEventListener("pointerenter", clearSidebarFlyoutTimers)')
        && app.includes('sidebarResizer.addEventListener("pointerleave"'));
check('ordinary panel switching does not renegotiate RDP resolution',
    activateBody.includes('notifyRdpLayout(false)')
        && !activateBody.includes('notifyRdpLayout(true)'));
check('pinning or unpinning the sidebar reflows every RDP session once',
    autoHideBody.includes('notifyRdpLayout(true, true, true)')
        && app.includes('reflowAll: shouldReflowAll')
        && backend.includes('nativeParams, "reflowAll", false')
        && backend.includes('clearOcclusion, reflowAll)'));
check('native all-session reflow uses the settled active panel bounds',
    host.includes("document.querySelector('.terminal-panel.active')")
        && reflowBody.includes('for (auto &entry : m_rdpSessions)')
        && reflowBody.includes('session->resize(')
        && reflowBody.includes('snapshot.width, snapshot.height,\n                    true')
        && reflowBody.includes('if (entry.second->connectPending)')
        && reflowBody.includes('session->connect('));
check('SFTP state survives switching focus to RDP',
    focusBody.includes('target.connectionType === "rdp"')
        && !focusBody.includes('clearSftpView()'));
check('sidebar mode and panel widths are persisted',
    app.includes('masterterm.sidebarMode')
        && app.includes('masterterm.sidebarFlyoutWidth.'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('SIDEBAR_AUTOHIDE_REGRESSION_OK');
