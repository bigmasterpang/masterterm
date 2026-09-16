// Chinese and special-path regression (task T3-5).
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8')
    .replaceAll('\r\n', '\n');
const app = read('resources/web/app.js');
const backend = read('src/WebViewBackend.cpp');
const worker = read('src/sftp_worker.cpp');
const cmake = read('CMakeLists.txt');
const manifest = read('resources/icons/MasterTerm.manifest');
const workerRc = read('resources/icons/MasterTermSftpWorker.rc');

let failures = 0;
function check(name, condition) {
    if (condition) console.log('PASS: ' + name);
    else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

check('frontend preserves exact local and remote path components',
    app.includes('const localJoinPath = (directory, name)')
        && app.includes('normalizeRemotePath(requestedPath)')
        && app.includes('new Map(visibleEntries.map(entry => [entry.path, entry]))'));
check('backend normalizes remote paths without using the local code page',
    backend.includes('NativeString cleanRemotePath(NativeString value)')
        && backend.includes('NativeString remoteChildPath')
        && backend.includes('utf8Text(remotePath)')
        && backend.includes('NativeString::fromUtf8'));
check('remote listing and progress encode non-ASCII names safely',
    worker.includes('NativeString::fromUtf8(name, count)')
        && worker.includes('base64Encode(file.relativePath)')
        && worker.includes('base64Encode(NativeFileInfo(localPath).fileName())'));
check('recursive transfers retain Unicode relative paths',
    worker.includes('relative.generic_wstring()')
        && worker.includes('rootName + \'/\' +')
        && worker.includes('remoteJoinPath(targetDirectory, file.relativePath)'));
check('Windows worker arguments use wide argv before converting to UTF-8',
    worker.includes('int wmain(int argc, wchar_t *argv[])')
        && worker.includes('NativeString::fromStdWString(argv[index])'));
check('long paths are enabled for both the host and SFTP worker',
    manifest.includes('<ws2:longPathAware>true</ws2:longPathAware>')
        && workerRc.includes('1 RT_MANIFEST "MasterTerm.manifest"')
        && cmake.includes('resources/icons/MasterTermSftpWorker.rc'));
check('native path text round-trip test is registered',
    cmake.includes('MasterTermNativePathTextTest')
        && fs.existsSync(path.join(root, 'tests', 'NativePathTextTest.cpp')));

if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('SPECIAL_PATH_REGRESSION_OK');
