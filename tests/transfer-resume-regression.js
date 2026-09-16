// Directory transfer checkpoint/resume regression (task T3-2).
//
// The worker resumes each directory member from the existing partial file,
// skips complete members, and can verify completed members with SHA-256.
// The frontend stores the same per-file checkpoint with the persisted queue.
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8')
    .replaceAll('\r\n', '\n');
const app = read('resources/web/app.js');
const backend = read('src/WebViewBackend.cpp');
const worker = read('src/sftp_worker.cpp');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

check('persisted task carries a per-file checkpoint',
    app.includes('checkpoint: serializeTransferCheckpoint(task)')
        && app.includes('const serializeTransferCheckpoint = task =>'));
check('directory progress updates the checkpoint',
    app.includes('activeTransferTask.directory && fileTotal > 0')
        && app.includes('activeTransferTask.checkpoint.files[checkpointName] ='));
check('recovery enables directory resume',
    app.includes('task.index, true,')
        && app.includes('task.checkpoint || null'));
check('directory upload sends the resume flag',
    app.includes('resumeDirectory: !!directory && !!resumeDirectory'));
check('directory download sends the resume flag',
    app.includes('resumeDirectory: !!entry.directory && !!resumeDirectory'));
check('native bridge forwards the directory resume flag',
    backend.includes('MASTERSSH_RESUME_TREE')
        && backend.includes('NativeJsonDom::booleanValue(params, "resumeDirectory", false)'));
check('worker has a directory resume mode',
    worker.includes('const bool resumeTree =')
        && worker.includes('MASTERSSH_RESUME_TREE'));
check('download resumes from an existing local offset',
    worker.includes('localSize > 0 && localSize < file.size')
        && worker.includes('libssh2_sftp_seek64(remote'));
check('upload resumes from an existing remote offset',
    worker.includes('remoteSize > 0 && remoteSize < fileTotal')
        && worker.includes('LIBSSH2_FXF_APPEND'));
check('complete directory members are skipped by size',
    worker.includes('skipExisting = !verifyChecksum')
        && worker.includes('doneBytes += file.size')
        && worker.includes('doneBytes += fileTotal'));
check('directory members can be checksum-verified',
    worker.includes('localRemoteChecksumMatches(file.localPath, remotePath)')
        && worker.includes('localRemoteChecksumMatches(localFilePath,')
        && worker.includes('SHA-256 不一致'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('TRANSFER_RESUME_REGRESSION_OK');
