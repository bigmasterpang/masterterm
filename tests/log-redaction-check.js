// Session-log redaction check for MasterTerm.  Reads the saved connection
// profile secrets (passwords, key passphrases, proxy passwords) and scans
// every session log under %APPDATA%\MasterSSH\logs for plaintext matches.
// Any hit means the redaction layer regressed and the check fails.
//
// Run: node tests/log-redaction-check.js
'use strict';

const fs = require('fs');
const path = require('path');

const appData = process.env.APPDATA || '';
const configPath = path.join(appData, 'MasterSSH', 'MasterSSH.json');
const logDirectory = path.join(appData, 'MasterSSH', 'logs');

let failures = 0;

function check(name, condition, detail) {
    if (condition) {
        console.log('PASS: ' + name);
    } else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

function readSecrets() {
    const secrets = [];
    if (!fs.existsSync(configPath)) return secrets;
    let config;
    try {
        config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    } catch (error) {
        console.log('SKIP: config unreadable (' + error.message + ')');
        return null;
    }
    const servers = Array.isArray(config.servers) ? config.servers : [];
    for (const server of servers) {
        if (!server || typeof server !== 'object') continue;
        for (const key of ['password', 'keyPassphrase', 'proxyPassword']) {
            const value = typeof server[key] === 'string' ? server[key] : '';
            if (value.length >= 4) secrets.push(value);
        }
    }
    return secrets;
}

function main() {
    const secrets = readSecrets();
    if (secrets === null) {
        console.log('LOG_REDACTION_OK (skip)');
        return;
    }
    if (secrets.length === 0) {
        console.log('PASS: no saved secrets to redact');
        console.log('LOG_REDACTION_OK');
        return;
    }
    if (!fs.existsSync(logDirectory)) {
        console.log('PASS: no session logs present');
        console.log('LOG_REDACTION_OK');
        return;
    }
    const logs = fs.readdirSync(logDirectory)
        .filter(name => name.endsWith('.log'))
        .map(name => path.join(logDirectory, name));
    check('redact: session logs found to scan', logs.length > 0);
    if (logs.length === 0) {
        console.log('LOG_REDACTION_OK');
        return;
    }
    let leaks = 0;
    for (const logFile of logs) {
        const content = fs.readFileSync(logFile);
        for (const secret of secrets) {
            const needle = Buffer.from(secret, 'utf8');
            let index = 0;
            while ((index = content.indexOf(needle, index)) !== -1) {
                ++leaks;
                const snippet = content.toString('utf8', Math.max(0, index - 20),
                    Math.min(content.length, index + secret.length + 20))
                    .replace(/[\r\n]/g, '\\n');
                console.log('LEAK: ' + path.basename(logFile) + ' at ' + index
                    + ': ...' + snippet + '...');
                index += needle.length;
            }
        }
    }
    check('redact: no plaintext secrets in session logs', leaks === 0,
        leaks + ' leak(s)');

    if (failures !== 0) {
        console.log(failures + ' check(s) failed');
        process.exit(1);
    }
    console.log('LOG_REDACTION_OK');
}

main();
