// SFTP transfer center regression (task T3-4).
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const app = fs.readFileSync(
    path.join(root, 'resources', 'web', 'app.js'), 'utf8')
    .replaceAll('\r\n', '\n');
const html = fs.readFileSync(
    path.join(root, 'resources', 'web', 'index.html'), 'utf8')
    .replaceAll('\r\n', '\n');
const css = fs.readFileSync(
    path.join(root, 'resources', 'web', 'app.css'), 'utf8')
    .replaceAll('\r\n', '\n');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

check('transfer center has a keyword filter and batch retry action',
    html.includes('id="sftp-transfer-filter"')
        && html.includes('id="sftp-transfer-retry-failed"')
        && app.includes('let transferFilterText = "";')
        && app.includes('const transferMatchesFilter = task =>'));
check('filter searches paths, direction, state and error details',
    app.includes('task?.path, task?.destination, task?.remotePath')
        && app.includes('task?.message, transferDirection(task), transferStateLabel(task?.state)')
        && app.includes('rows = rows.filter(transferMatchesFilter);'));
check('batch retry removes failed results and reuses production retry paths',
    app.includes('const retryAllFailedTransfers = () =>')
        && app.includes('transferResults.filter(isFailedTransfer)')
        && app.includes('failed.forEach(retryTransferTask)')
        && html.includes('data-transfer-action="retry-failed-all"'));
check('failure details expose local, remote and checkpoint information',
    app.includes('const localPath = result.path || result.destination')
        && app.includes('const remotePath = result.remotePath || result.remoteDirectory')
        && app.includes('const checkpoint = result.checkpoint?.files')
        && app.includes('恢复信息：${checkpoint}'));
check('queue mutations persist immediately after destructive or ordering actions',
    app.includes('renderTransferPanel();\n      persistTransferQueue();\n      return;')
        && app.includes('sftpTransferRetryFailedButton?.addEventListener'));
check('transfer center has an empty state and theme-aware toolbar styling',
    app.includes('className = "sftp-transfer-empty"')
        && css.includes('.sftp-transfer-toolbar')
        && css.includes('html[data-theme="light"] .sftp-transfer-filter input')
        && css.includes('html[data-theme="blue"] .sftp-transfer-filter input'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('TRANSFER_CENTER_REGRESSION_OK');
