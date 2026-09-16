// Backend shutdown lifecycle regression sentinel.
// beginShutdown() must reject new work without suppressing the later cleanup
// that owns and terminates MasterTermSftpWorker child processes.
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const backendHeader = fs.readFileSync(
    path.join(root, 'src', 'WebViewBackend.h'), 'utf8');
const backendCpp = fs.readFileSync(
    path.join(root, 'src', 'WebViewBackend.cpp'), 'utf8');
const cmake = fs.readFileSync(path.join(root, 'CMakeLists.txt'), 'utf8');

let failures = 0;

function check(name, condition) {
    if (condition) {
        console.log('PASS: ' + name);
    } else {
        ++failures;
        console.log('FAIL: ' + name);
    }
}

function functionBody(source, signature, nextSignature) {
    const start = source.indexOf(signature);
    const end = source.indexOf(nextSignature, start + signature.length);
    return start >= 0 && end > start ? source.slice(start, end) : '';
}

const beginBody = functionBody(
    backendCpp, 'void WebViewBackend::beginShutdown()',
    'void WebViewBackend::shutdown()');
const shutdownBody = functionBody(
    backendCpp, 'void WebViewBackend::shutdown()',
    'WebViewBackend::~WebViewBackend()');

check('beginShutdown closes the new-work gate',
    beginBody.includes('m_shuttingDown.store(true'));
check('cleanup has an independent one-time guard',
    backendHeader.includes('std::atomic<bool> m_shutdownComplete{false};')
        && shutdownBody.includes('m_shutdownComplete.exchange(true'));
check('shutdown does not return merely because beginShutdown ran',
    !shutdownBody.includes('m_shuttingDown.exchange(true'));
check('shutdown snapshots and clears all owned workers',
    shutdownBody.includes('m_sftpProcesses.begin()')
        && shutdownBody.includes('m_sftpProcesses.clear()'));
check('shutdown destroys each worker process owner',
    shutdownBody.includes('for (NativeProcess *process : processes)')
        && shutdownBody.includes('delete process;'));
check('release carries the compiler-matched MSVC synchronization runtime',
    cmake.includes('include(InstallRequiredSystemLibraries)')
        && cmake.includes('CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS'));

if (failures !== 0) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
}
console.log('PROCESS_LIFECYCLE_REGRESSION_OK');
