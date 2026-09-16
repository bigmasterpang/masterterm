// Native RDP tab-switch regression sentinel. The embedded mstscax control
// must stay alive while it is inactive: SW_HIDE/SW_SHOW destroys its current
// presentation surface and produces a black frame on the next tab switch.
'use strict';

const fs = require('fs');
const path = require('path');

// Source files may carry CRLF line endings depending on the checkout
// (core.autocrlf).  Multi-line patterns below assume LF, so normalize.
const readSource = file =>
    fs.readFileSync(path.join(__dirname, '..', file), 'utf8')
        .replaceAll('\r\n', '\n');

const root = path.join(__dirname, '..');
const rdpCpp = readSource('src/RdpSession.cpp');
const rdpHeader = readSource('src/RdpSession.h');
const windowCpp = readSource('src/WebViewWindow.cpp');
const windowHeader = readSource('src/WebViewWindow.h');
const appJs = readSource('resources/web/app.js');
const appCss = readSource('resources/web/app.css');

let failures = 0;

function check(name, condition) {
    if (condition) {
        console.log('PASS: ' + name);
    } else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

function functionBody(source, signature, nextSignature) {
    const start = source.indexOf(signature);
    const end = source.indexOf(nextSignature, start + signature.length);
    return start >= 0 && end > start ? source.slice(start, end) : '';
}

const visibilityBody = functionBody(
    rdpCpp, 'void RdpSession::setHostedVisible(bool visible)',
    'void RdpSession::clearOcclusionRegion()');
const emitStateBody = functionBody(
    rdpCpp, 'void RdpSession::emitState(const std::wstring &state)',
    'bool RdpSession::create()');
const resizeStart = rdpCpp.indexOf('void RdpSession::resize(');
const resizeBody = resizeStart >= 0 ? rdpCpp.slice(resizeStart) : '';
const layoutBody = functionBody(
    windowCpp, 'void WebViewWindow::layoutRdpSession(',
    'void WebViewWindow::notifyRdpState(');
const stateBody = functionBody(
    windowCpp, 'void WebViewWindow::notifyRdpState(',
    'void WebViewWindow::setRdpFullscreen(');
const failedCleanupBody = functionBody(
    windowCpp, 'void WebViewWindow::cleanupFailedRdpSession(',
    'void WebViewWindow::notifyRdpQuality(');
const closeBody = functionBody(
    windowCpp, 'void WebViewWindow::closeRdpSession(',
    'void WebViewWindow::layoutRdpSession(');
const frontendStateStart = appJs.indexOf('if (message.event === "rdp.state")');
const frontendStateEnd = appJs.indexOf(
    'if (message.event === "tunnel.state")', frontendStateStart);
const frontendStateBody = frontendStateStart >= 0 && frontendStateEnd > frontendStateStart
    ? appJs.slice(frontendStateStart, frontendStateEnd) : '';

check('inactive connected surface is parked at HWND_BOTTOM',
    visibilityBody.includes('m_hostedVisible ? HWND_TOP : HWND_BOTTOM'));
check('parked connected surface remains a visible HWND',
    visibilityBody.includes('SWP_SHOWWINDOW'));
check('empty or disconnected ActiveX host is still hidden',
    visibilityBody.includes('if (!m_visible)')
        && visibilityBody.includes('SW_HIDE'));
check('unchanged native host bounds skip SetWindowPos',
    resizeBody.includes('if (boundsChanged)'));
check('unchanged ActiveX client size skips SetObjectRects',
    resizeBody.includes('if (controlSizeChanged)'));

const handoff = layoutBody.indexOf('} else if (switchingFromVisibleRdp) {');
const raiseTarget = layoutBody.indexOf(
    'current->session->setHostedVisible(true);', handoff);
const lowerPrevious = layoutBody.indexOf(
    'previous->session->setHostedVisible(false);', handoff);
const connectingTarget = layoutBody.indexOf(
    'if (switchingFromVisibleRdp && !targetReady)');
const backgroundInitialResize = layoutBody.indexOf(
    'current->session->resize(', connectingTarget);
const pendingConnect = layoutBody.indexOf(
    'if (current->connectPending)', backgroundInitialResize);
check('RDP handoff raises target before lowering previous surface',
    handoff >= 0 && raiseTarget > handoff && lowerPrevious > raiseTarget);
check('selected and actually presented RDP sessions are tracked separately',
    windowHeader.includes('m_presentedRdpSessionId')
        && layoutBody.includes('previousPresentedSessionId =')
        && layoutBody.includes('m_presentedRdpSessionId = sessionId'));
check('handoff source is the last presented surface, not the latest click',
    layoutBody.includes('findRdpSession(previousPresentedSessionId)')
        && layoutBody.includes('entry.first != previousPresentedSessionId')
        && !layoutBody.includes('previousActiveSessionId'));
check('background RDP host is sized before its connection starts',
    connectingTarget >= 0 && backgroundInitialResize > connectingTarget
        && pendingConnect > backgroundInitialResize);
check('connecting RDP target hides the previous presented desktop',
    layoutBody.includes('if (!targetReady)')
        && layoutBody.includes('previous->session->setHostedVisible(false);')
        && layoutBody.includes('m_presentedRdpSessionId.clear();'));
check('connecting RDP target is raised for its own status overlay',
    layoutBody.includes(
        'if (switchingFromVisibleRdp && !targetReady) {')
        && layoutBody.includes(
            'current->session->setHostedVisible(true);')
        && appJs.includes('rdpConnectingNotice'));
check('RDP connecting overlay is included in native occlusion queries',
    appJs.includes('"#rdp-fullscreen-hot-zone", ".rdp-connecting-notice"')
        && windowCpp.includes('targetReady = hosted->session->isConnected()')
        && appCss.includes('.rdp-connecting-notice:not([hidden])'));
check('pending RDP always initializes DesktopWidth and DesktopHeight',
    layoutBody.includes(
        'const bool initializeDisplaySize = current->connectPending;')
        && layoutBody.includes(
            'refreshDisplaySettings || initializeDisplaySize'));
check('SSH-to-RDP waits for native surface preactivation',
    appJs.includes('const preactivateNativeRdp = target.connectionType === "rdp"')
        && appJs.includes('activeSession?.connectionType !== "rdp"')
        && /post\("session\.rdpLayout", \{[\s\S]*?visible: true,[\s\S]*?refresh: false,[\s\S]*?\}\)\.catch\(reportError\)\.then\(commitActivation\)/
            .test(appJs));
check('stale asynchronous tab activations cannot commit',
    appJs.includes('let sessionActivationGeneration = 0;')
        && appJs.includes(
            'activationGeneration !== sessionActivationGeneration'));
check('ordinary tab activation never requests display renegotiation',
    /const activateSession = sessionId =>[\s\S]*?notifyRdpLayout\(false\);/
        .test(appJs));
check('failed RDP tabs are excluded from native visibility requests',
    appJs.includes('activeSession.state !== "closed"')
        && appJs.includes('activeSession.state !== "error"')
        && appJs.includes('visible: activeRdpVisible()'));
check('selecting a stale RDP tab exits native hosted layout',
    layoutBody.includes('!hosted->session->isConnected()')
        && layoutBody.includes('!hosted->session->isConnecting()')
        && layoutBody.includes('m_rdpHostedVisible = false;'));
check('native disconnect cancels pending layout and hides the surface',
    stateBody.includes('const bool terminalState')
        && stateBody.includes('++m_rdpLayoutGeneration;')
        && stateBody.includes('hosted->session->setHostedVisible(false);'));
check('terminal RDP failures queue cleanup after the ActiveX callback returns',
    windowHeader.includes('RdpFailedCleanupMessage')
        && windowCpp.includes('PostMessageW(m_window, RdpFailedCleanupMessage')
        && stateBody.includes('scheduleFailedRdpCleanup(sessionId)')
        && windowCpp.includes(
            'if (message == RdpFailedCleanupMessage)'));
check('failed RDP cleanup releases the native session instead of waiting for reconnect',
    windowHeader.includes('m_rdpFailedCleanupTokens')
        && failedCleanupBody.includes('failureCleanupToken != token')
        && failedCleanupBody.includes('m_rdpSessions.erase(found)')
        && failedCleanupBody.includes('native-host-released'));
check('deferred cleanup cannot delete a replacement with the same session id',
    windowCpp.includes('failureCleanupQueued')
        && windowCpp.includes('m_rdpFailureCleanupToken')
        && windowCpp.includes('m_rdpFailedCleanupTokens.erase(sessionId)'));
check('RDP control warnings are informational, not terminal failures',
    rdpCpp.includes('warning:远程桌面控件报告警告')
        && appJs.includes('state.startsWith("warning:")')
        && !emitStateBody.includes('warning:'));
check('RDP interruption triggers a Windows notification once',
    frontendStateBody.includes('notify("RDP 连接已中断"')
        && frontendStateBody.includes('!session.rdpDisconnectNotified')
        && frontendStateBody.includes('session.closing'));
check('RDP interruption immediately clears native menu occlusion',
    frontendStateBody.includes('notifyRdpLayout(false, true);'));
check('stale RDP tab renders reconnect and close actions',
    appJs.includes('className = "rdp-disconnect-notice"')
        && appJs.includes('rdpReconnectButton.textContent = "重新连接"')
        && appJs.includes('rdpCloseNoticeButton.textContent = "关闭标签"')
        && appCss.includes('.rdp-disconnect-notice:not([hidden])'));
check('native state emitter suppresses duplicate lifecycle events',
    emitStateBody.includes('state == m_lastState')
        && rdpHeader.replaceAll(' ', '').includes('std::wstringm_lastState;'));
check('explicit native close does not synthesize a disconnect event',
    !closeBody.includes('notifyRdpState(sessionId, L"disconnected")'));
check('native Escape routes through the frontend close lifecycle',
    windowCpp.includes(
        'm_backend->notifyRdpContextAction(sessionId, "close")')
        && windowCpp.includes(
            'm_backend->notifyRdpContextAction(\n                    m_activeRdpSessionId, "close")'));
check('closing and stale sessions cannot update RDP UI state',
    frontendStateBody.includes('if (!session || session.closing) {')
        && frontendStateBody.includes('clearRdpConnectingStatus(message.sessionId);')
        && frontendStateBody.includes('if (session.rdpLastState === state) {'));
check('only the active RDP session can update the global status',
    frontendStateBody.includes('if (activeSession === session) {'));
check('closing a tab prefers a healthy remaining session',
    appJs.includes('const chooseHealthySession = sessionIds =>')
        && appJs.includes('session.state === "connected"')
        && appJs.includes('session.state === "connecting"')
        && appJs.includes('!session.closing'));

if (failures !== 0) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('RDP_SWITCH_REGRESSION_OK');
