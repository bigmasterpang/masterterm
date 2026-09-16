// Cloud sync pre-upload diff regression (task T2-3).
//
// Extracts computeCloudDiff (plus its cloudSchema dependency) from
// resources/web/app.js between the schema and diff markers and exercises
// the real comparison logic:
//   - added / modified / deleted connections are detected by type+address
//   - secret fields (passwords, key data) never mark a profile modified
//   - workspace additions and deletions are reported (未分配 excluded)
//   - unchanged data reports an empty diff
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const appJs = fs.readFileSync(
    path.join(root, 'resources', 'web', 'app.js'), 'utf8');

let failures = 0;
function check(name, condition, detail = '') {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

const schemaBegin = appJs.indexOf('// begin cloud-schema');
const schemaEnd = appJs.indexOf('// end cloud-schema');
const diffBegin = appJs.indexOf('// begin cloud-diff');
const diffEnd = appJs.indexOf('// end cloud-diff');
check('markers exist', schemaBegin >= 0 && schemaEnd > schemaBegin
    && diffBegin >= 0 && diffEnd > diffBegin);
if (schemaBegin < 0 || diffBegin < 0) {
    process.exit(1);
}
const block = appJs.slice(schemaBegin, schemaEnd)
    + '\n' + appJs.slice(diffBegin, diffEnd);

const defaultSettings = { keepalive: 30, fontSize: 14 };
const evaluate = new Function('defaultSettings', `
    ${block}
    return { schema: cloudSchema, diff: computeCloudDiff };
`);
const { schema, diff } = evaluate(defaultSettings);

const profile = (name, address, extra = {}) => ({
    connectionType: "ssh", name, address, port: "22",
    tag: "SSH", workspace: "未分配", color: "#8ab4f8",
    iconKey: "server", keyPath: "", keyPassphrase: "",
    proxyJump: "", proxyKeyPath: "", serialDataBits: 8,
    serialParity: "none", serialStopBits: "1", serialFlowControl: "none",
    rdpOptions: {}, tunnels: [], password: "", keyData: "",
    proxyKeyData: "", ...extra
});

const cloud = [
    profile("生产", "root@prod.example.com"),
    profile("测试", "root@test.example.com", { color: "#34d399" })
];
const local = [
    profile("生产", "root@prod.example.com", { color: "#f472b6" }),
    profile("新机", "root@new.example.com")
];

// 1. Basic diff: modified + added + deleted.
const result = diff(
    cloud, ["未分配", "生产环境"],
    local, ["未分配", "开发环境"]);
check('modified connection detected',
    result.modified.length === 1
        && result.modified[0].local.name === "生产");
check('added connection detected',
    result.added.length === 1 && result.added[0].name === "新机");
check('deleted connection detected',
    result.deleted.length === 1 && result.deleted[0].name === "测试");
check('added workspace detected',
    result.addedWorkspaces.includes("开发环境"));
check('deleted workspace detected',
    result.deletedWorkspaces.includes("生产环境"));
check('changed count aggregates',
    result.changed === 5);

// 2. Secret fields never mark a profile modified.
const secretCloud = [
    profile("生产", "root@prod.example.com", {
        password: "plain-on-cloud", keyData: "enc:device-a-blob"
    })
];
const secretLocal = [
    profile("生产", "root@prod.example.com", {
        password: "different-local", keyData: "enc:device-b-blob"
    })
];
const secretResult = diff(secretCloud, [], secretLocal, []);
check('different secrets do not mark modified',
    secretResult.modified.length === 0
        && secretResult.changed === 0);

// 3. Identical data reports an empty diff.
const same = diff(cloud, ["未分配", "生产环境"], cloud, ["未分配", "生产环境"]);
check('identical data reports no changes', same.changed === 0);

// 4. 未分配 never counts as a workspace change.
const unassigned = diff([], ["未分配"], [], ["未分配", "新工作区"]);
check('未分配 excluded from workspace diffs',
    unassigned.deletedWorkspaces.length === 0
        && unassigned.addedWorkspaces.length === 1);

// 5. Legacy/malformed cloud entries are sanitized before comparing.
const legacyCloud = [
    profile("生产", "root@prod.example.com"),
    { address: "root@bad.example.com", connectionType: "telnet", port: 999 }
];
const legacyResult = diff(legacyCloud, null, local, null);
check('malformed cloud data is sanitized before diffing',
    legacyResult.deleted.length === 1
        && legacyResult.deleted[0].connectionType === "ssh"
        && legacyResult.deleted[0].port === "999");

// 6. Merging cloud-only entries back produces an unchanged diff.
const mergedLocal = local.concat(result.deleted);
const mergedWorkspaces = [...new Set(["未分配", "开发环境", "生产环境"])];
const merged = diff(cloud, ["未分配", "生产环境"], mergedLocal, mergedWorkspaces);
check('merge upload payload keeps cloud-only entries (no deletions)',
    merged.deleted.length === 0 && merged.deletedWorkspaces.length === 0);

if (failures !== 0) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('CLOUD_DIFF_REGRESSION_OK');
