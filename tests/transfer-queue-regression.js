// Transfer-queue persistence regression (task T3-1).
//
// Verifies the frontend structure required for cross-restart recovery of
// unfinished SFTP transfers:
//   - a versioned localStorage key stores unfinished tasks
//   - persistence is gated until the stored queue is hydrated, so the
//     startup render can never wipe the saved queue
//   - stream uploads (drag-drop) and remote edit syncs are excluded from
//     the snapshot because they cannot be re-created after a restart
//   - recovered tasks carry the profile index and are re-driven through
//     the same start paths, avoiding a dependency on the live SFTP panel
//   - recovery offers continue-all / discard choices and a confirmation
//     summary before re-running anything
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const appJs = fs.readFileSync(
    path.join(root, 'resources', 'web', 'app.js'), 'utf8')
    .replaceAll('\r\n', '\n');

let failures = 0;
function check(name, condition, detail = '') {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

check('versioned queue storage key exists',
    appJs.includes('const transferQueueStorageKey = "masterterm.transferQueue.v1"'));
check('hydration flag guards persistence',
    appJs.includes('let transferQueueHydrated = false;')
        && appJs.includes('const persistTransferQueue = () => {\n    if (!transferQueueHydrated) return;'));
check('persistence is throttled, not per-render',
    appJs.includes('const scheduleTransferPersist = () =>')
        && appJs.includes('}, 1500);'));
check('snapshot includes active task, queued tasks and failures',
    appJs.includes('if (activeTransferTask) pushTask(activeTransferTask);')
        && appJs.includes('for (const task of transferQueue) pushTask(task);')
        && appJs.includes('result.state === "error" || result.state === "cancelled"'));
check('snapshot only serializes upload and download tasks',
    appJs.includes('if (task.type === "upload")\n        tasks.push({')
        && appJs.includes('else if (task.type === "download" && task.entry)'));
check('snapshot carries the profile index for headless recovery',
    appJs.includes('index: task.index ?? sftpProfile?.index ?? -1'));
check('startSftpUpload accepts an explicit profile index',
    appJs.includes('verify = false, profileIndex = null,\n                                 resumeDirectory = false, checkpoint = null) => {\n    if (!sftpProfile && profileIndex == null) return;'));
check('startSftpDownload accepts an explicit profile index',
    appJs.includes('profileIndex = null, resumeDirectory = false,\n                                   checkpoint = null) => {\n    if ((!sftpProfile && profileIndex == null) || !entry) return;'));
check('queue pump passes the stored index through',
    appJs.includes('next.verify || false, next.index,\n        !!next.resumeDirectory, next.checkpoint || null);')
        && appJs.includes('0, next.verify || false, next.index,\n        !!next.resumeDirectory, next.checkpoint || null);'));
check('retry paths pass the stored index through',
    appJs.includes('task.verify || false, task.index);'));
check('recovery asks continue-all or discard before re-running',
    appJs.includes('const choice = await showActionDialog({')
        && appJs.includes('title: "发现未完成的传输任务"')
        && appJs.includes('{ action: "discard", label: "清理" }')
        && appJs.includes('{ action: "resume", label: "继续全部", primary: true }'));
check('recovery summary names the pending tasks',
    appJs.includes('上次运行时 ${tasks.length} 个传输任务未完成`'));
check('discard removes the stored queue',
    appJs.includes('localStorage.removeItem(transferQueueStorageKey);'));
check('recovery re-drives tasks through the production start paths',
    appJs.includes('startSftpUpload(task.path, !!task.temporary, true,')
        && appJs.includes('startSftpDownload({\n          name: task.entry?.name || "文件"'));
check('recovery hooks into the startup sequence',
    appJs.includes('.then(() => recoverPersistedTransfers().catch(reportError));'));
check('hydration happens after the stored queue is read',
    appJs.includes('transferQueueHydrated = true;\n    if (!tasks.length) return;'));

if (failures !== 0) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('TRANSFER_QUEUE_REGRESSION_OK');
