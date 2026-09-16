// SFTP conflict strategy regression (task T3-3).
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const app = fs.readFileSync(
    path.join(root, 'resources', 'web', 'app.js'), 'utf8')
    .replaceAll('\r\n', '\n');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

check('conflict dialog shows both sides before choosing',
    app.includes('const formatConflictDetails = (localEntry, remoteEntry)')
        && app.includes('比较信息')
        && app.includes('请比较后选择处理方式'));
check('conflict details include size and modified time',
    app.includes('大小 ${size}，修改时间 ${modified}')
        && app.includes('entry.lastModified'));
check('single-file upload supplies local and remote metadata',
    app.includes('formatConflictDetails(localEntry, remoteEntry)'));
check('single-file download supplies local and remote metadata',
    app.includes('formatConflictDetails(localEntry, entry)'));
check('external file conflicts supply remote metadata',
    app.includes('formatConflictDetails(file, remoteEntry)'));
check('all conflict policies remain available',
    app.includes('{ action: "skip", label: "跳过" }')
        && app.includes('{ action: "rename", label: "自动重命名" }')
        && app.includes('{ action: "overwrite", label: "覆盖", primary: true }')
        && app.includes('{ action: "overwrite-all", label: "全部覆盖" }'));
check('batch choices apply to the whole batch',
    app.includes('transferConflictPolicy = "overwrite"')
        && app.includes('startBatch("overwrite")')
        && app.includes('startBatch(policy || "skip")'));
check('rename policy uses a collision-free name',
    app.includes('const uniqueRemoteEntryName = name =>')
        && app.includes('if (conflict === "rename")'));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('TRANSFER_CONFLICT_REGRESSION_OK');
