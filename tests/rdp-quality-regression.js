// RDP quality snapshot/panel regression (task T4-1).
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8')
    .replaceAll('\r\n', '\n');
const sessionHeader = read('src/RdpSession.h');
const session = read('src/RdpSession.cpp');
const backendHeader = read('src/WebViewBackend.h');
const backend = read('src/WebViewBackend.cpp');
const windowHeader = read('src/WebViewWindow.h');
const window = read('src/WebViewWindow.cpp');
const app = read('resources/web/app.js');
const index = read('resources/web/index.html');
const css = read('resources/web/app.css');
const cmake = read('CMakeLists.txt');
const openRdpStart = window.indexOf('void WebViewWindow::openRdpSession');
const openRdpEnd = window.indexOf(
    'void WebViewWindow::closeRdpSession', openRdpStart);
const openRdpBlock = openRdpStart >= 0 && openRdpEnd > openRdpStart
    ? window.slice(openRdpStart, openRdpEnd) : '';

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

check('native RDP session stores negotiated quality fields',
    sessionHeader.includes('struct RdpQualitySnapshot')
        && sessionHeader.includes('clientProtocolSpec')
        && sessionHeader.includes('avcStatus')
        && session.includes('quality readback: ColorDepth=')
        && session.includes('m_quality.lastOperation = L"连接参数回读"')
        && session.includes('m_quality.fullFrameRefreshCount'));
check('quality snapshots cross the native WebView event boundary',
    backendHeader.includes('notifyRdpQuality')
        && backend.includes('sendNativeEvent("rdp.quality"')
        && windowHeader.includes('notifyRdpQuality')
        && window.includes('setQualityHandler')
        && window.includes('notifyRdpQuality(sessionId, snapshot)'));
check('fullscreen native overlay exposes actual quality without WebView z-order',
    window.includes('实际质量')
        && window.includes('RDP 实际质量')
        && window.includes('qualitySnapshot()')
        && window.includes('m_rdpFullscreenOverlayMenu == 4')
        && window.includes('index == 5 ? palette.closeText'));
check('frontend keeps a per-session quality panel and HRESULT display',
    index.includes('rdp-quality-panel')
        && app.includes('rdpQualityBySession')
        && app.includes('message.event === "rdp.quality"')
        && app.includes('formatRdpHresult')
        && app.includes('rdpQualityPanel.hidden = false')
        && css.includes('#rdp-quality-panel'));
check('connected RDP sessions expose compact quality metrics in the header',
    index.includes('id="server-metrics"')
        && app.includes('const renderRdpServerMetrics = session =>')
        && app.includes('["延迟", latencyText')
        && app.includes('["质量", performance')
        && app.includes('["分辨率", resolution')
        && app.includes('if (session.connectionType === "rdp")'));
check('quality regression is registered with CTest',
    cmake.includes('MasterTermRdpQualityRegression')
        && fs.existsSync(path.join(root, 'tests', 'rdp-quality-regression.js')));
check('fullscreen multimon keeps the monitor topology during resize',
    sessionHeader.includes('m_useMultimon')
        && sessionHeader.includes('m_containerHandledFullscreen')
        && session.includes('keep multimon topology negotiated at Connect()')
        && !session.includes('client9->SyncSessionDisplaySettings()')
        && openRdpBlock.includes('m_rdpSessions.emplace')
        && !openRdpBlock.includes('setUseMultimon(true)')
        && window.includes('m_rdpFullscreenUseMultimon = hosted->options.useMultimon != 0')
        && window.includes('recreateRdpSession(sessionId, true)')
        && window.includes('recreateRdpSession(sessionId, false)')
        && window.includes('multimon-container')
        && sessionHeader.includes('setContainerHandledFullscreen')
        && session.includes('put_ContainerHandledFullScreen(')
        && session.includes('MasterTerm container owns fullscreen')
        && window.includes('container-handled multimon; WebView hidden')
        && window.includes('setWebViewVisible(false)')
        && !window.includes('GetSystemMetrics(SM_CXVIRTUALSCREEN)'));
check('normal RDP starts as a single-monitor embedded session',
    openRdpBlock.includes('layoutRdpSession(sessionId, true, true)')
        && !openRdpBlock.includes('setContainerHandledFullscreen')
        && !openRdpBlock.includes('setUseMultimon'));
check('fullscreen restore pre-sizes the replacement before reconnecting',
    sessionHeader.includes('hostBounds(int &x, int &y, int &width, int &height)')
        && window.includes('m_rdpNormalHostRectValid')
        && window.includes('pre-sized single-monitor replacement')
        && window.includes('layoutAlreadyQueued')
        && window.includes('!layoutAlreadyQueued'));
check('fullscreen multimon fills the native container and rejects panning',
    session.indexOf('MsTscAx.MsTscAx.13') >= 0
        && session.indexOf('MsTscAx.MsTscAx.13')
            < session.indexOf('MsTscAx.MsTscAx.9')
        && sessionHeader.includes('validateNativeFullscreenPresentation')
        && session.includes('get_HorizontalScrollBarVisible')
        && session.includes('get_VerticalScrollBarVisible')
        && session.includes('container fullscreen validation: control=')
        && session.includes('intersectionArea(monitor.bounds, containerBounds)')
        && window.includes('queryVirtualMonitorLayout(layout)')
        && window.includes('HWND_TOPMOST')
        && windowHeader.includes('RdpFullscreenValidationTimer')
        && window.includes('validateNativeFullscreenPresentation()')
        && window.includes('restored single-monitor session after fullscreen')
        && window.includes('WS_EX_TOOLWINDOW')
        && window.includes('setTaskbarTabVisible(m_window, false)'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('RDP_QUALITY_REGRESSION_OK');
