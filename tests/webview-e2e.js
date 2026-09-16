// MasterTerm real WebView2 end-to-end harness (task T1-3).
//
// Drives the production UI over the Chrome DevTools Protocol (exposed when
// MasterTerm is started with MASTERTERM_CDP_PORT, see tools/webview-e2e.ps1):
// real clicks, real dialogs, a real SSH connection and the RDP failure
// lifecycle.  Failures save screenshots and a DOM excerpt into the artifact
// directory; the wrapper additionally copies diagnostic.log/crash.log.
//
// Usage: node tests/webview-e2e.js --port <cdp-port> --artifact-dir <dir>
//        [--ssh-name e2e-ssh] [--rdp-name e2e-rdp] [--timeout-ms 30000]
'use strict';

const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
function argValue(name, fallback = '') {
    const index = args.indexOf(name);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
}
const port = argValue('--port');
const artifactDir = argValue('--artifact-dir');
const sshName = argValue('--ssh-name', 'e2e-ssh');
const rdpName = argValue('--rdp-name', 'e2e-rdp');
const stepTimeout = parseInt(argValue('--timeout-ms', '30000'), 10);

if (!port || !artifactDir) {
    console.error('usage: node webview-e2e.js --port <cdp-port> --artifact-dir <dir>');
    process.exit(2);
}
fs.mkdirSync(artifactDir, { recursive: true });

let failures = 0;
function check(name, condition, detail = '') {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

const sleep = milliseconds =>
    new Promise(resolve => setTimeout(resolve, milliseconds));

async function waitForNodeFile(checker, timeout = 10000, interval = 300) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (checker()) return true;
        await sleep(interval);
    }
    return checker();
}

async function findPageTarget() {
    const deadline = Date.now() + 60000;
    while (Date.now() < deadline) {
        try {
            const response = await fetch(`http://127.0.0.1:${port}/json/list`);
            const targets = await response.json();
            const page = targets.find(target => target.type === 'page');
            if (page && page.webSocketDebuggerUrl) return page;
        } catch (error) {
            // The CDP endpoint appears once the WebView2 browser process is up.
        }
        await sleep(500);
    }
    return null;
}

class Cdp {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.nextId = 1;
        this.pending = new Map();
        this.listeners = new Map();
        this.ws.addEventListener('message', event => {
            let message;
            try {
                message = JSON.parse(event.data);
            } catch (error) {
                return;
            }
            if (message.id && this.pending.has(message.id)) {
                const { resolve, reject } = this.pending.get(message.id);
                this.pending.delete(message.id);
                if (message.error) reject(new Error(message.error.message));
                else resolve(message.result);
                return;
            }
            const handlers = this.listeners.get(message.method) || [];
            for (const handler of handlers) handler(message.params);
        });
    }

    open() {
        return new Promise((resolve, reject) => {
            this.ws.addEventListener('open', resolve, { once: true });
            this.ws.addEventListener('error', () => reject(new Error('CDP websocket error')), { once: true });
        });
    }

    send(method, params = {}) {
        return new Promise((resolve, reject) => {
            const id = this.nextId++;
            this.pending.set(id, { resolve, reject });
            this.ws.send(JSON.stringify({ id, method, params }));
        });
    }

    on(method, handler) {
        if (!this.listeners.has(method)) this.listeners.set(method, []);
        this.listeners.get(method).push(handler);
    }

    close() {
        try {
            this.ws.close();
        } catch (error) {
            // Ignore.
        }
    }
}

async function evaluate(cdp, expression) {
    const result = await cdp.send('Runtime.evaluate', {
        expression,
        returnByValue: true,
        awaitPromise: true,
    });
    if (result.exceptionDetails) {
        throw new Error('evaluate failed: ' +
            (result.exceptionDetails.exception?.description ||
                result.exceptionDetails.text || 'unknown'));
    }
    return result.result ? result.result.value : undefined;
}

async function waitFor(cdp, expression, timeout = stepTimeout, interval = 300) {
    const deadline = Date.now() + timeout;
    let lastValue;
    while (Date.now() < deadline) {
        lastValue = await evaluate(cdp, expression);
        if (lastValue) return lastValue;
        await sleep(interval);
    }
    return lastValue;
}

async function elementCenter(cdp, finderExpression) {
    return evaluate(cdp, `(() => {
        const element = ${finderExpression};
        if (!element) return null;
        const rect = element.getBoundingClientRect();
        if (rect.width <= 0 || rect.height <= 0) return null;
        return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 };
    })()`);
}

async function mouseClick(cdp, x, y, button = 'left', clickCount = 1) {
    // A double click is two full press/release cycles within the OS
    // double-click time window; a single pair with clickCount=2 does not
    // produce a "dblclick" DOM event.
    for (let count = 1; count <= clickCount; ++count) {
        await cdp.send('Input.dispatchMouseEvent', {
            type: 'mousePressed', x, y, button, clickCount: count,
        });
        await cdp.send('Input.dispatchMouseEvent', {
            type: 'mouseReleased', x, y, button, clickCount: count,
        });
        if (count < clickCount) await sleep(60);
    }
}

async function clickElement(cdp, finderExpression, clickCount = 1, button = 'left') {
    const center = await elementCenter(cdp, finderExpression);
    if (!center) throw new Error('element not clickable: ' + finderExpression);
    await mouseClick(cdp, center.x, center.y, button, clickCount);
    return true;
}

async function screenshot(cdp, name) {
    const capture = await cdp.send('Page.captureScreenshot', { format: 'png' });
    const file = path.join(artifactDir, name + '.png');
    fs.writeFileSync(file, Buffer.from(capture.data, 'base64'));
    console.log('ARTIFACT: ' + file);
}

async function saveDomExcerpt(cdp, name) {
    try {
        const html = await evaluate(cdp,
            'document.body ? document.body.innerHTML : "no body"');
        const excerpt = String(html).slice(0, 50000);
        const file = path.join(artifactDir, name + '.html.txt');
        fs.writeFileSync(file, excerpt, 'utf8');
        console.log('ARTIFACT: ' + file);
        const summary = await evaluate(cdp, `(() => {
            const tabs = Array.from(document.querySelectorAll('.terminal-tab'))
                .map(tab => tab.dataset.sessionId + ':' + (tab.dataset.state || '?'));
            const dialog = document.querySelector('#server-dialog')?.open
                ? 'open' : 'closed';
            const auth = document.querySelector('#action-dialog')?.open
                ? document.querySelector('#action-dialog-title')?.textContent : '';
            return JSON.stringify({ tabs, serverDialog: dialog, actionDialog: auth });
        })()`);
        console.log('STATE: ' + summary);
    } catch (error) {
        // The page may already be gone; the screenshot still documents state.
    }
}

async function recordFailure(cdp, name) {
    try {
        await screenshot(cdp, name);
        await saveDomExcerpt(cdp, name);
    } catch (error) {
        console.log('ARTIFACT FAILED: ' + name + ' ' + error.message);
    }
}

async function main() {
    const target = await findPageTarget();
    check('CDP: WebView2 page target appears', !!target);
    if (!target) {
        console.log(failures + ' check(s) failed');
        process.exit(1);
    }

    const cdp = new Cdp(target.webSocketDebuggerUrl);
    await cdp.open();
    await cdp.send('Runtime.enable');
    await cdp.send('Page.enable');
    cdp.on('Runtime.exceptionThrown', p => {
        console.error('BROWSER_EXCEPTION:', JSON.stringify(p.exceptionDetails));
    });
    cdp.on('Runtime.consoleAPICalled', p => {
        if (p.type === 'error') console.error('BROWSER_CONSOLE_ERROR:', p.args.map(a => a.value || a.description).join(' '));
    });
    let step = 'startup';

    try {
        step = 'startup';
        const ready = await waitFor(cdp,
            `document.querySelectorAll('.server').length > 0 && !!document.querySelector('#terminal-tabs')`, 30000);
        check('UI: frontend rendered and interactive', !!ready);
        if (!ready) await recordFailure(cdp, '01-startup-failed');

        step = 'settings-dialog';
        // The click can race the frontend's initial render; retry briefly.
        let settingsOpen = false;
        for (let attempt = 0; attempt < 3 && !settingsOpen; ++attempt) {
            await clickElement(cdp, `document.querySelector('#settings-button')`);
            settingsOpen = await waitFor(cdp,
                `document.querySelector('#settings-dialog')?.open === true`, 5000);
            if (!settingsOpen) await sleep(1000);
        }
        check('UI: settings dialog opens', !!settingsOpen);
        if (!settingsOpen) {
            await recordFailure(cdp, '02-settings-open-failed');
        } else {
            await evaluate(cdp, `(() => {
                const form = document.querySelector('#settings-form');
                if (form) { form.scrollTop = form.scrollHeight; }
                return true;
            })()`);
            await clickElement(cdp, `document.querySelector('#settings-dialog-close')`);
            const settingsClosed = await waitFor(cdp,
                `document.querySelector('#settings-dialog')?.open !== true`, 10000);
            check('UI: settings dialog closes after scroll', !!settingsClosed);
            if (!settingsClosed) await recordFailure(cdp, '03-settings-close-failed');
        }

        step = 'sidebar-pin';
        // The WebView2 profile is shared with the real app, so the sidebar
        // may start auto-hidden (cards are clipped and not clickable).
        // Hover the nav rail to open the flyout, pin the sidebar, and
        // restore the persisted mode before exit.
        const originalSidebarMode = await evaluate(cdp,
            `localStorage.getItem('masterterm.sidebarMode')`);
        await cdp.send('Input.dispatchMouseEvent', {
            type: 'mouseMoved', x: 20, y: 400,
        });
        const flyoutOpen = await waitFor(cdp,
            `document.querySelector('#workspace').classList.contains('sidebar-flyout-open')`, 5000);
        if (flyoutOpen) {
            await clickElement(cdp, `document.querySelector('#sidebar-pin')`);
        }
        const sidebarDocked = await waitFor(cdp,
            `!document.querySelector('#workspace').classList.contains('sidebar-auto-hide')`, 5000);
        check('UI: sidebar pins so connection cards are clickable', !!sidebarDocked);
        if (!sidebarDocked) await recordFailure(cdp, '01b-sidebar-pin-failed');

        step = 'cloud-crypto-import';
        // T2-2: drive cloud.importServers through the real bridge with a
        // DPAPI-encrypted private key blob (prepared by the wrapper with
        // matching entropy).  The backend must decrypt it on this device
        // and materialize the key file inside the isolated data directory.
        const dataDir = argValue('--data-dir');
        const testKeySecret = argValue('--test-key-secret');
        const testKeyName = argValue('--test-key-name', 'id_rsa');
        if (dataDir && testKeySecret) {
            await evaluate(cdp, `(() => {
                window.chrome.webview.postMessage({
                    id: 'e2e-crypto-import', method: 'cloud.importServers',
                    params: {
                        servers: [{
                            name: 'e2e-crypto-test',
                            connectionType: 'ssh',
                            address: 'e2e@127.0.0.1',
                            port: '22',
                            keyPath: ${JSON.stringify(testKeyName)},
                            keyData: ${JSON.stringify(testKeySecret)},
                            workspace: 'e2e-生产'
                        }],
                        workspaces: ['e2e-生产', 'e2e-测试']
                    }
                });
                return true;
            })()`);
            const keyMaterialized = await waitForNodeFile(
                () => {
                    try {
                        const keyDir = path.join(dataDir, 'keys');
                        if (!fs.existsSync(keyDir)) return false;
                        return fs.readdirSync(keyDir)
                            .some(name => name.endsWith('-' + testKeyName));
                    } catch (error) {
                        return false;
                    }
                }, 20000);
            check('Cloud: DPAPI-encrypted key decrypts and materializes on this device',
                !!keyMaterialized);
            if (!keyMaterialized) await recordFailure(cdp, '04b-cloud-import-failed');
            // T2-5: workspace order follows the cloud list (未分配 first),
            // and the imported profile lands in its cloud workspace.
            const workspaceState = await waitForNodeFile(
                () => {
                    try {
                        const config = JSON.parse(fs.readFileSync(
                            path.join(dataDir, 'MasterSSH.json'), 'utf8'));
                        const list = Array.isArray(config.workspaces)
                            ? config.workspaces : [];
                        return list[0] === '未分配'
                            && list[1] === 'e2e-生产'
                            && list[2] === 'e2e-测试'
                            && list.length === 3;
                    } catch (error) {
                        return false;
                    }
                }, 15000);
            check('Cloud: imported workspace list keeps cloud order (未分配 first)',
                !!workspaceState);
            if (!workspaceState) await recordFailure(cdp, '04c-workspace-order-failed');
        } else {
            console.log('SKIP: cloud crypto import (missing --data-dir/--test-key-secret)');
        }

        step = 'ssh-connect';
        const sshCard = await elementCenter(cdp, `Array.from(
            document.querySelectorAll('.server')).find(card =>
                (card.querySelector('.server-label')?.textContent || '').trim() === '${sshName}')`);
        check('UI: SSH connection card found', !!sshCard);
        if (!sshCard) {
            await recordFailure(cdp, '04-ssh-card-missing');
        } else {
            await mouseClick(cdp, sshCard.x, sshCard.y, 'left', 2);
            const connected = await waitFor(cdp,
                `Array.from(document.querySelectorAll('.terminal-tab'))
                    .some(tab => tab.dataset.state === 'connected')`, 45000);
            check('SSH: session reaches connected state', !!connected);
            if (!connected) {
                await recordFailure(cdp, '05-ssh-connect-failed');
            } else {
                const sftpLoaded = await waitFor(cdp,
                    `document.querySelectorAll('#sftp-files tr').length > 0`, 30000);
                check('SFTP: panel loads the remote directory', !!sftpLoaded);
                if (!sftpLoaded) await recordFailure(cdp, '06-sftp-load-failed');

                step = 'split-layout';
                // Splitting needs a second session: open a local ConPTY
                // terminal from the tab-strip context menu.
                const tabStrip = await evaluate(cdp, `(() => {
                    const rect = document.querySelector('#terminal-tabs').getBoundingClientRect();
                    return { x: rect.right - 20, y: rect.y + rect.height / 2 };
                })()`);
                await mouseClick(cdp, tabStrip.x, tabStrip.y, 'right', 1);
                const tabMenuVisible = await waitFor(cdp,
                    `!document.querySelector('#terminal-tabs-context-menu').hidden`, 10000);
                check('UI: tab strip context menu opens', !!tabMenuVisible);
                if (tabMenuVisible) {
                    await clickElement(cdp,
                        `document.querySelector('#terminal-tabs-context-menu [data-action="new-local"]')`);
                }
                const twoConnected = await waitFor(cdp,
                    `Array.from(document.querySelectorAll('.terminal-tab'))
                        .filter(tab => tab.dataset.state === 'connected').length >= 2`, 20000);
                check('ConPTY: local terminal session opens', !!twoConnected);
                if (!twoConnected) {
                    await recordFailure(cdp, '06b-local-terminal-failed');
                } else {
                    // Activate the SSH tab first: the split logic uses the
                    // active tab as the primary pane, and right-clicking
                    // alone does not change activation.
                    await clickElement(cdp,
                        `Array.from(document.querySelectorAll('.terminal-tab'))
                            .find(tab => tab.dataset.state === 'connected')`);
                    await clickElement(cdp,
                        `Array.from(document.querySelectorAll('.terminal-tab'))
                            .find(tab => tab.dataset.state === 'connected')`, 1, 'right');
                    const menuVisible = await waitFor(cdp,
                        `!document.querySelector('#terminal-context-menu').hidden`, 10000);
                    check('UI: tab context menu opens', !!menuVisible);
                    if (!menuVisible) {
                        await recordFailure(cdp, '07-tab-menu-failed');
                    } else {
                        await clickElement(cdp,
                            `document.querySelector('#terminal-context-menu [data-action="split-vertical"]')`);
                        const choiceDialog = await waitFor(cdp,
                            `document.querySelector('#action-dialog')?.open === true`, 10000);
                        check('UI: split session picker dialog opens', !!choiceDialog);
                        if (!choiceDialog) {
                            await recordFailure(cdp, '07b-split-picker-failed');
                        } else {
                            await clickElement(cdp,
                                `document.querySelector('#action-dialog-actions button.primary')`);
                            const splitCreated = await waitFor(cdp,
                                `!!document.querySelector('.split-view.right')`, 10000);
                            check('UI: vertical split creates the right pane', !!splitCreated);
                            if (!splitCreated) {
                                await recordFailure(cdp, '08-split-create-failed');
                            } else {
                                await clickElement(cdp,
                                    `document.querySelector('.split-view.right .terminal-tab')`, 1, 'right');
                                await waitFor(cdp,
                                    `!document.querySelector('#terminal-context-menu').hidden`, 10000);
                                await clickElement(cdp,
                                    `document.querySelector('#terminal-context-menu [data-action="split-close"]')`);
                                const splitClosed = await waitFor(cdp,
                                    `!document.querySelector('.split-view')`, 10000);
                                check('UI: split closes back to a single pane', !!splitClosed);
                                if (!splitClosed) await recordFailure(cdp, '09-split-close-failed');
                            }
                        }
                    }
                }
            }
        }

        step = 'rdp-failure-lifecycle';
        // The SFTP panel became the active sidebar panel after the SSH
        // connection; switch back to the connection list before locating
        // the RDP card.
        await clickElement(cdp,
            `document.querySelector('.function-tab[data-panel="connections"]')`);
        await sleep(500);
        const rdpCard = await elementCenter(cdp, `Array.from(
            document.querySelectorAll('.server')).find(card =>
                (card.querySelector('.server-label')?.textContent || '').trim() === '${rdpName}')`);
        check('UI: RDP connection card found', !!rdpCard);
        if (!rdpCard) {
            await recordFailure(cdp, '10-rdp-card-missing');
        } else {
            const tabsBefore = await evaluate(cdp,
                `document.querySelectorAll('.terminal-tab').length`);
            await mouseClick(cdp, rdpCard.x, rdpCard.y, 'left', 2);
            const noticeShown = await waitFor(cdp,
                `(() => {
                    const notice = document.querySelector('.rdp-disconnect-notice');
                    return !!notice && !notice.hidden;
                })()`, 45000);
            check('RDP: unreachable target surfaces the disconnect notice', !!noticeShown);
            if (!noticeShown) {
                await recordFailure(cdp, '11-rdp-notice-failed');
            } else {
                await clickElement(cdp, `Array.from(
                    document.querySelectorAll('.rdp-disconnect-notice button'))
                        .find(button => button.textContent.trim() === '关闭标签')`);
                const tabsRestored = await waitFor(cdp,
                    `document.querySelectorAll('.terminal-tab').length <= ${tabsBefore}`, 10000);
                check('RDP: closing the failed tab restores the tab list', !!tabsRestored);
                if (!tabsRestored) await recordFailure(cdp, '12-rdp-close-failed');
            }
        }

        step = 'ssh-disconnect';
        await clickElement(cdp,
            `Array.from(document.querySelectorAll('.terminal-tab'))
                .find(tab => tab.dataset.state === 'connected')`, 1, 'right');
        await waitFor(cdp, `!document.querySelector('#terminal-context-menu').hidden`, 10000);
        await clickElement(cdp,
            `document.querySelector('#terminal-context-menu [data-action="close-all"]')`);
        const allClosed = await waitFor(cdp,
            `document.querySelectorAll('.terminal-tab').length === 0`, 15000);
        check('SSH: closing all sessions leaves a clean empty tab area', !!allClosed);
        if (!allClosed) await recordFailure(cdp, '13-ssh-close-failed');

        step = 'exit';
        // Restore the sidebar mode the real app profile had before the test.
        const restoreSidebarMode = originalSidebarMode === null
            ? `localStorage.removeItem('masterterm.sidebarMode');`
            : `localStorage.setItem('masterterm.sidebarMode',
                ${JSON.stringify(originalSidebarMode)});`;
        await evaluate(cdp, `(() => { ${restoreSidebarMode} return true; })()`)
            .catch(() => {});
        // The WebView2 bridge expects an OBJECT message (the app itself never
        // sends JSON strings; a string is JSON-encoded again by the bridge).
        await evaluate(cdp, `(() => {
            window.chrome.webview.postMessage({
                id: 'e2e-exit', method: 'app.closeDecision',
                params: { decision: 'exit' },
            });
            return true;
        })()`);
        let appGone = false;
        const exitDeadline = Date.now() + 20000;
        while (Date.now() < exitDeadline) {
            await sleep(500);
            try {
                const response = await fetch(`http://127.0.0.1:${port}/json/list`);
                const targets = await response.json();
                if (!targets.some(candidate => candidate.type === 'page')) {
                    appGone = true;
                    break;
                }
            } catch (error) {
                appGone = true;
                break;
            }
        }
        check('Exit: app tears down after UI-driven close decision', appGone);
        if (!appGone) await recordFailure(cdp, '14-exit-failed');
    } catch (error) {
        ++failures;
        console.log('FAIL: unexpected error in step ' + step + ': ' + error.message);
        await recordFailure(cdp, '99-' + step + '-error');
    } finally {
        cdp.close();
    }

    if (failures !== 0) {
        console.log(failures + ' check(s) failed');
        process.exit(1);
    }
    console.log('WEBVIEW_E2E_OK');
}

main();
