// Cloud-sync schema versioning regression (task T2-1).
//
// Extracts the createCloudSchema block from resources/web/app.js (between
// the "begin cloud-schema" / "end cloud-schema" markers) and executes the
// real migration functions against legacy and malformed payloads:
//   - payloads without a version migrate to the current schema
//   - unknown/higher versions are refused instead of corrupting data
//   - every section is sanitized independently (servers, workspaces,
//     history, settings whitelist)
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

const begin = appJs.indexOf('// begin cloud-schema');
const end = appJs.indexOf('// end cloud-schema');
check('schema block markers exist', begin >= 0 && end > begin);
if (begin < 0 || end <= begin) {
    console.log('CLOUD_SCHEMA_REGRESSION_FAILED');
    process.exit(1);
}
const block = appJs.slice(begin, end);

// The block references `defaultSettings` from the surrounding scope.
const defaultSettings = {
    keepalive: 30,
    autoReconnect: false,
    reconnectAttempts: 3,
    fontSize: 14,
    scrollback: 5000,
    historyLimit: 500,
    completionLimit: 5,
    historyDays: 0,
    copyOnSelect: false,
    cursorBlink: true,
    sessionLogging: false
};
const evaluate = new Function('defaultSettings', `
    ${block}
    return cloudSchema;
`);
const schema = evaluate(defaultSettings);

check('schema exposes the current version', schema.CLOUD_SCHEMA_VERSION === 2);

// 1. Legacy payload without any version still applies.
const legacy = schema.migrateCloudPayload({
    servers: [{ name: "vm", address: "user@example.com", port: 22, keyPath: "/k" }],
    workspaces: ["我的服务器"],
    history: ["ls -la", 42, ""],
    settings: { fontSize: 18, unknownNewField: true }
});
check('legacy payload (no version) migrates to current schema',
    legacy && legacy.version === 2 && Array.isArray(legacy.servers));
check('legacy port number is normalized to a string',
    legacy && legacy.servers[0].port === "22");
check('legacy workspace list keeps custom workspace',
    legacy && legacy.workspaces.includes("我的服务器"));
check('legacy workspace list always starts with 未分配',
    legacy && legacy.workspaces[0] === "未分配");
check('legacy history drops non-string entries',
    legacy && legacy.history.length === 1 && legacy.history[0] === "ls -la");
check('settings whitelist drops unknown keys and applies known ones',
    legacy && legacy.settings.fontSize === 18
        && legacy.settings.unknownNewField === undefined);
check('settings whitelist keeps defaults for missing keys',
    legacy && legacy.settings.keepalive === 30);

// 2. Explicit version 1 payload behaves like the legacy one.
const v1 = schema.migrateCloudPayload({
    version: 1,
    servers: [],
    workspaces: [],
    history: [],
    settings: {}
});
check('version 1 payload migrates', v1 && v1.version === 2);
check('version 1 payload gains 未分配 workspace',
    v1 && v1.workspaces.length === 1 && v1.workspaces[0] === "未分配");

// 3. Newer schema versions are refused instead of partially applied.
const future = schema.migrateCloudPayload({
    version: 99,
    servers: [{ address: "root@example.com" }]
});
check('future version is refused with a marker',
    future && future.tooNew === true && future.version === 99);

// 4. Invalid payload shapes are rejected wholesale.
check('null payload rejected', schema.migrateCloudPayload(null) === null);
check('array payload rejected', schema.migrateCloudPayload([]) === null);
check('scalar payload rejected', schema.migrateCloudPayload("sync") === null);

// 5. Server sanitation keeps good entries and repairs bad fields.
const servers = schema.migrateCloudServers([
    { address: "root@ok.example.com", connectionType: "rdp",
        serialDataBits: 42, workspace: "" },
    { name: "no-address" },
    null,
    { address: "root@default.example.com", connectionType: "telnet" }
]);
check('servers without an address are dropped', servers.length === 2);
check('unknown connection type falls back to ssh',
    servers[1].connectionType === "ssh");
check('serial data bits are clamped into 5..8',
    servers[0].serialDataBits === 8);
check('empty workspace falls back to 未分配',
    servers[0].workspace === "未分配");
check('missing rdpOptions becomes an empty object',
    servers[1].rdpOptions && typeof servers[1].rdpOptions === "object");

// 6. Workspace sanitization: dedupe, order, junk removal.
const workspaces = schema.migrateWorkspaces([
    "未分配", "生产环境", "生产环境", "", 7, "开发环境", "未分配"
]);
check('workspace dedupe and junk removal',
    workspaces.length === 3);
check('workspace order keeps 未分配 first',
    workspaces[0] === "未分配"
        && workspaces.includes("生产环境")
        && workspaces.includes("开发环境"));

// 7. History sanitization.
check('history without array becomes empty list',
    JSON.stringify(schema.migrateHistory({ nope: 1 })) === "[]");
check('history caps at 2000 entries',
    schema.migrateHistory(new Array(2500).fill("cmd")).length === 2000);

// 8. Settings type mismatches are ignored per key.
const settings = schema.migrateSettings({
    fontSize: "huge",          // wrong type -> default
    keepalive: 45,             // right type -> applied
    scrollback: 3000
});
check('settings type mismatch falls back to default',
    settings.fontSize === 14);
check('settings correct type is applied',
    settings.keepalive === 45 && settings.scrollback === 3000);

// 9. savedAt passthrough.
const stamped = schema.migrateCloudPayload({
    version: 2, savedAt: "2026-08-14T00:00:00Z", servers: []
});
check('savedAt passes through', stamped.savedAt === "2026-08-14T00:00:00Z");
const unstamped = schema.migrateCloudPayload({ version: 2, servers: [] });
check('missing savedAt becomes empty string', unstamped.savedAt === "");

if (failures !== 0) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('CLOUD_SCHEMA_REGRESSION_OK');
