// Split-view layout and DPI regression for MasterTerm.  Headless sentinel:
// re-implements the split-ratio math used by app.js and asserts that the
// implementation, the CSS contract and the native DPI handling stay in sync
// (clamp bounds, percentage formatting, default ratio, split divider sizing,
// per-monitor DPI awareness).
//
// Run: node tests/split-layout-regression.js
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const appJs = fs.readFileSync(path.join(root, 'resources', 'web', 'app.js'), 'utf8');
const appCss = fs.readFileSync(path.join(root, 'resources', 'web', 'app.css'), 'utf8');
const mainCpp = fs.readFileSync(path.join(root, 'src', 'main.cpp'), 'utf8');
const windowCpp = fs.readFileSync(path.join(root, 'src', 'WebViewWindow.cpp'), 'utf8');

let failures = 0;

function check(name, condition, detail) {
    if (condition) {
        console.log('PASS: ' + name);
    } else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

// Mirror of app.js applySplitRatio: clamp into [0.18, 0.82], fall back to 0.5
// for non-numeric input, and format as a percentage rounded to 0.01%.
function applySplitRatio(ratio) {
    const parsed = Number(ratio);
    const value = Math.max(.18, Math.min(.82,
        Number.isFinite(parsed) ? parsed : .5));
    return { ratio: value, percent: `${Math.round(value * 10000) / 100}%` };
}

function checkRatioMath() {
    check('ratio: default is 0.5', applySplitRatio(undefined).ratio === .5);
    check('ratio: 0.5 stays 0.5', applySplitRatio(.5).ratio === .5);
    check('ratio: 0 clamps to 0.18', applySplitRatio(0).ratio === .18);
    check('ratio: negative clamps to 0.18', applySplitRatio(-5).ratio === .18);
    check('ratio: 1 clamps to 0.82', applySplitRatio(1).ratio === .82);
    check('ratio: 3 clamps to 0.82', applySplitRatio(3).ratio === .82);
    check('ratio: NaN falls back to 0.5', applySplitRatio(NaN).ratio === .5);
    check('ratio: numeric string parses', applySplitRatio('0.6').ratio === .6);
    check('ratio: percent rounds to 0.01%', applySplitRatio(.1823).percent === '18.23%');
    check('ratio: percent at clamp bound', applySplitRatio(.82).percent === '82%');
    check('ratio: percent at lower bound', applySplitRatio(.18).percent === '18%');
}

function checkAppImplementation() {
    check('impl: splitRatio default .5 in app.js', /let splitRatio = \.5;/.test(appJs));
    check('impl: clamp lower bound .18', /Math\.max\(\.18,/.test(appJs));
    check('impl: clamp upper bound .82', /Math\.min\(\.82,/.test(appJs));
    check('impl: non-finite input falls back to .5',
        /Number\.isFinite\(parsed\) \? parsed : \.5/.test(appJs));
    check('impl: percentage formatting rounds to 0.01%',
        /Math\.round\(splitRatio \* 10000\) \/ 100/.test(appJs));
    check('impl: sets --split-primary-size',
        /--split-primary-size/.test(appJs));
    check('impl: removes --split-primary-size when closing the split',
        /removeProperty\("--split-primary-size"\)/.test(appJs));
    check('impl: divider ratio derives from pointer position',
        /(clientX|clientY) - bounds\.(left|top)/.test(appJs)
        && /bounds\.(width|height)/.test(appJs));
    check('impl: divider is 8px wide in CSS',
        /\.split-divider \{ [^}]*flex: 0 0 8px/.test(appCss));
    check('impl: existing-connection picker includes RDP profiles',
        /\["ssh", "rdp", "serial"\]\.includes\(profile\.connectionType\)/.test(appJs));
    check('impl: existing-connection picker labels RDP profiles',
        /profile\.connectionType === "rdp" \? "RDP"/.test(appJs));
    check('impl: SSH split actions remain enabled when RDP exists',
        appJs.includes('terminalTabs.classList.toggle("has-rdp", hasRdp)')
        && appJs.includes('splitNavTab.textContent = "🔲 终端分屏"'));
    check('impl: RDP sessions are preserved alongside split terminals',
        appJs.includes('filter(tab => sessions.get(tab.dataset.sessionId)?.connectionType !== "rdp")')
        && appJs.includes('RDP 会话为原生桌面，不支持加入分屏'));
}

function checkCssContract() {
    check('css: vertical split lays out as a row',
        /\.terminal-panels\.split-vertical \{ display: flex; flex-direction: row; \}/
            .test(appCss));
    check('css: horizontal split lays out as a column',
        /\.terminal-panels\.split-horizontal \{ display: flex; flex-direction: column; \}/
            .test(appCss));
    check('css: primary pane sized by --split-primary-size',
        /flex: 0 0 var\(--split-primary-size, 50%\)/.test(appCss));
    check('css: focused pane has an inset highlight',
        /\.split-view\.focused \{/.test(appCss));
    check('css: dividers use col/row resize cursors',
        /\.split-divider\.vertical \{ cursor: col-resize; \}/.test(appCss)
        && /\.split-divider\.horizontal \{ cursor: row-resize; \}/.test(appCss));
}

function checkNativeDpi() {
    check('dpi: per-monitor V2 awareness requested',
        /SetProcessDpiAwarenessContext\(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2\)/
            .test(mainCpp));
    check('dpi: WM_DPICHANGED handled', /case WM_DPICHANGED:/.test(windowCpp));
    check('dpi: window DPI queried per window', /GetDpiForWindow\(m_window\)/.test(windowCpp));
    check('dpi: metrics scaled by DPI', /scaleForDpi\(/.test(windowCpp));
    check('dpi: min track size scaled for maximize/restore bounds',
        /ptMinTrackSize\.x = scaleForDpi\(640, dpi\)/.test(windowCpp));
    check('dpi: startup window bounds scale with system DPI',
        /scaleForDpi\(1280, dpi\)/.test(windowCpp));
}

function main() {
    checkRatioMath();
    checkAppImplementation();
    checkCssContract();
    checkNativeDpi();

    if (failures !== 0) {
        console.log(failures + ' check(s) failed');
        process.exit(1);
    }
    console.log('SPLIT_LAYOUT_REGRESSION_OK');
}

main();
