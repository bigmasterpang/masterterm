// Full-screen program interaction regression for the vendored xterm.js
// engine used by MasterTerm.  Runs headless under Node (no DOM required):
// feeds the VT streams produced by vim/top/less-style programs and verifies
// alternate-screen switching, live refresh, resize, scrolling, chunked
// streaming, malformed bytes and exit integrity.
//
// Run: node tests/fullscreen-terminal-regression.js
'use strict';

const path = require('path');
const { Terminal } = require(path.join(
    __dirname, '..', 'resources', 'web', 'vendor', 'xterm', 'xterm.js'));

let failures = 0;

function check(name, condition, detail) {
    if (condition) {
        console.log('PASS: ' + name);
    } else {
        ++failures;
        console.log('FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
    }
}

function makeTerminal(cols, rows) {
    return new Terminal({
        cols: cols || 80,
        rows: rows || 24,
        scrollback: 1000
    });
}

function writeAsync(term, data) {
    return new Promise((resolve) => term.write(data, resolve));
}

function bufferLines(term) {
    const lines = [];
    const buffer = term.buffer.active;
    for (let y = 0; y < buffer.length; ++y) {
        const line = buffer.getLine(y);
        lines.push(line ? line.translateToString(true) : '');
    }
    return lines;
}

function cursorInBounds(term) {
    const buffer = term.buffer.active;
    return buffer.cursorX >= 0 && buffer.cursorX < term.cols
        && buffer.cursorY >= 0 && buffer.cursorY < term.rows;
}

// vim-style session: alternate screen, cursor painting, typing, resize and
// clean exit that must restore the shell buffer exactly.
async function scenarioVimAlternateScreen() {
    const term = makeTerminal();
    const write = (data) => writeAsync(term, data);
    await write('$ cat notes.md\r\nhello world\r\n$ ');
    const normalBefore = bufferLines(term);

    await write('\x1b[?1049h');          // vim opens its full-screen editor
    await write('\x1b[H');               // cursor home
    await write('VIM SESSION\x1b[2;1HLine two content\x1b[3;1HLine three');
    await write('\x1b[24;1H\x1b[7m-- INSERT --\x1b[0m'); // status bar
    await write('\x1b[5;10H');

    check('vim: alternate screen is active',
        term.buffer.active === term.buffer.alternate);
    check('vim: shell content preserved in normal buffer',
        term.buffer.normal.getLine(0).translateToString(true)
            === '$ cat notes.md');
    check('vim: status bar drawn on last row',
        term.buffer.alternate.getLine(23).translateToString(true)
            .includes('-- INSERT --'));
    check('vim: cursor in bounds', cursorInBounds(term));

    const typedKeys = [];
    term.onData((data) => typedKeys.push(data));
    // Keyboard input travels the onData path and is rendered by the remote
    // echo, exactly like the app's session.input round trip.
    term.input('typed');
    check('vim: key input reaches the data path', typedKeys.join('') === 'typed');
    await write('typed');
    check('vim: echoed text lands at cursor',
        term.buffer.alternate.getLine(4).translateToString(true)
            .indexOf('typed') === 9);

    term.resize(120, 40);
    check('vim: resize applied mid-session',
        term.cols === 120 && term.rows === 40);
    check('vim: cursor in bounds after resize', cursorInBounds(term));
    await write('\x1b[5;15H');
    await write('+');
    check('vim: echoed typing still works after resize',
        term.buffer.alternate.getLine(4).translateToString(true)
            .indexOf('typed+') === 9);
    term.resize(80, 24);

    // Mirror the app's leaveAlternateScreen sequence (cursor keys off, show
    // cursor, clear line, exit the alternate screen).
    await write('\x1b[?12l\x1b[?25h\x1b[K\x1b[?1049l');
    check('vim: returns to normal buffer',
        term.buffer.active === term.buffer.normal);
    check('vim: shell buffer restored exactly',
        JSON.stringify(bufferLines(term)) === JSON.stringify(normalBefore));
    check('vim: cursor in bounds after exit', cursorInBounds(term));
}

// top/htop-style live refresh: repeated clear + redraw, with a resize
// interleaved mid-stream.
async function scenarioLiveMonitorRefresh() {
    const term = makeTerminal();
    const write = (data) => writeAsync(term, data);
    await write('\x1b[?1049h');
    for (let cycle = 0; cycle < 5; ++cycle) {
        await write('\x1b[H\x1b[2J');
        await write('top - 12:00:0' + cycle + ' up 3 days\r\n');
        await write('Tasks:  12' + cycle + ' total\r\n');
        await write('%Cpu(s): ' + cycle + '.0 us\r\n');
        await write('\x1b[5;1H');
    }
    check('monitor: latest header value rendered',
        term.buffer.alternate.getLine(0).translateToString(true)
            === 'top - 12:00:04 up 3 days');
    check('monitor: task count updated',
        term.buffer.alternate.getLine(1).translateToString(true)
            === 'Tasks:  124 total');
    check('monitor: cursor in bounds', cursorInBounds(term));

    term.resize(132, 43);
    check('monitor: resize applied',
        term.cols === 132 && term.rows === 43);
    await write('\x1b[H\x1b[2J%CPU after resize: 99.9');
    check('monitor: refresh continues after resize',
        term.buffer.alternate.getLine(0).translateToString(true)
            === '%CPU after resize: 99.9');
    term.resize(80, 24);
    await write('\x1b[?1049l');
    check('monitor: exit restores normal buffer',
        term.buffer.active === term.buffer.normal);
}

// less-style pager: long output in the normal buffer with viewport scrolling.
async function scenarioPagerScrolling() {
    const term = makeTerminal();
    for (let index = 0; index < 60; ++index) {
        await writeAsync(
            term, 'log line ' + String(index).padStart(2, '0') + '\r\n');
    }
    const buffer = term.buffer.active;
    const maxViewportY = buffer.length - term.rows;
    check('pager: buffer grew past the viewport', buffer.length > term.rows);
    check('pager: starts at the bottom',
        buffer.viewportY === maxViewportY);
    term.scrollLines(-10);
    check('pager: scroll up moves the viewport',
        term.buffer.active.viewportY === maxViewportY - 10);
    term.scrollToTop();
    check('pager: scroll to top shows the first line',
        term.buffer.active.viewportY === 0
            && term.buffer.active.getLine(0).translateToString(true)
                === 'log line 00');
    term.scrollToBottom();
    check('pager: scroll to bottom restores the end',
        term.buffer.active.viewportY === maxViewportY);
    check('pager: cursor still in bounds', cursorInBounds(term));
}

// Chunked/streaming input: a full vim paint sequence split at odd byte
// boundaries must produce the same screen as a single write.  Also verifies
// that a multi-byte UTF-8 character split across writes decodes correctly.
async function scenarioChunkedStreaming() {
    const sequence =
        '\x1b[?1049h'
        + '\x1b[2J\x1b[HVim buffer line 1\r\n'
        + 'Vim buffer line 2\r\n'
        + 'Vim buffer line 3\r\n'
        + '\x1b[4;1H\x1b[7m-- NORMAL --\x1b[0m\r\n'
        + '\x1b[5;20Hcursor here\x1b[?1049l';

    const single = makeTerminal();
    await writeAsync(single, sequence);

    const chunked = makeTerminal();
    const sizes = [1, 2, 3, 5, 7, 13, 17];
    let offset = 0;
    for (const size of sizes) {
        await writeAsync(chunked, sequence.slice(offset, offset + size));
        offset += size;
    }
    for (; offset < sequence.length; offset += 17) {
        await writeAsync(chunked, sequence.slice(offset, offset + 17));
    }
    check('streaming: chunked writes match a single write',
        JSON.stringify(bufferLines(chunked)) === JSON.stringify(bufferLines(single)));

    const utf8 = makeTerminal();
    // 中 = E4 B8 AD; split after the first two bytes.
    await writeAsync(utf8, new Uint8Array([0xE4, 0xB8]));
    await writeAsync(utf8, new Uint8Array([0xAD]));
    check('streaming: split UTF-8 sequence decodes to a single character',
        utf8.buffer.active.getLine(0).translateToString(true) === '\u4e2d');
}

// Malformed input resilience: broken UTF-8, lone ESC, incomplete CSI, control
// bytes and 8-bit garbage must not wedge the parser or corrupt state.
async function scenarioMalformedBytes() {
    const term = makeTerminal();
    const garbage = new Uint8Array([
        0xC3, 0x28,          // invalid UTF-8 continuation
        0x1B,                // lone ESC
        0x1B, 0x5B,          // incomplete CSI
        0x00, 0x07, 0x08,    // NUL, BEL, backspace
        0x0B, 0x0C, 0x0E, 0x0F,
        0x80, 0xFF, 0xFE
    ]);
    await writeAsync(term, garbage);
    await writeAsync(term, '\x1b[H\x1b[2Jafter garbage\r\n');
    check('malformed: terminal stays usable after garbage',
        term.buffer.active.getLine(0).translateToString(true)
            === 'after garbage');
    await writeAsync(term, '\x1b[2;1Hmoved');
    check('malformed: CSI state machine recovered',
        term.buffer.active.getLine(1).translateToString(true) === 'moved');
    check('malformed: cursor in bounds', cursorInBounds(term));

    await writeAsync(term, '\x1b[?2004h');
    check('malformed: bracketed paste mode enables',
        term.modes.bracketedPasteMode === true);
    await writeAsync(term, '\x1b[?2004l');
    check('malformed: bracketed paste mode disables',
        term.modes.bracketedPasteMode === false);
}

// Alternate screen variants (47 / 1047 / 1049) used by different full-screen
// programs, including 1047's persistent-content semantics.
async function scenarioAlternateVariants() {
    const term = makeTerminal();
    const write = (data) => writeAsync(term, data);

    await write('\x1b[?47hpersist 47\x1b[?47l');
    check('variant: 47 enters and exits the alternate screen',
        term.buffer.active === term.buffer.normal);

    await write('\x1b[?1047hstale content\x1b[?1047l');
    await write('\x1b[?1047h');
    check('variant: 1047 re-entry starts clean (no stale screen)',
        term.buffer.alternate.getLine(0).translateToString(true)
            === '');
    await write('\x1b[Hfresh');
    check('variant: 1047 writable after re-entry',
        term.buffer.alternate.getLine(0).translateToString(true) === 'fresh');
    await write('\x1b[?1047l');

    await write('\x1b[?1049h');
    await write('\x1b[H\x1b[2Jfresh 1049');
    check('variant: 1049 clears on entry',
        term.buffer.alternate.getLine(0).translateToString(true)
            === 'fresh 1049');
    await write('\x1b[?1049l');
    check('variant: 1049 exits cleanly',
        term.buffer.active === term.buffer.normal);
}

// A transport failure can arrive while a full-screen program owns the
// alternate buffer.  The client must restore the shell buffer locally before
// appending its error marker because there is no remote transport left to
// perform the cleanup.
async function scenarioDisconnectRestoresShellBuffer() {
    const term = makeTerminal();
    const write = (data) => writeAsync(term, data);
    await write('shell history before opencode\r\n$ opencode\r\n');
    await write('\x1b[?1049h\x1b[HOpenCode screen');
    check('disconnect: alternate screen is active before restore',
        term.buffer.active === term.buffer.alternate);
    await write('\x1b[?12l\x1b[?25h\x1b[K\x1b[?1049l');
    await write('\r\n[error: 读取远端终端失败：transport read]\r\n');
    check('disconnect: normal shell buffer is restored',
        term.buffer.active === term.buffer.normal);
    check('disconnect: shell history remains available',
        bufferLines(term).some(line => line.includes('shell history before opencode')));
    check('disconnect: error is appended to the normal buffer',
        bufferLines(term).some(line => line.includes('[error: 读取远端终端失败：transport read]')));
}

async function main() {
    await scenarioVimAlternateScreen();
    await scenarioLiveMonitorRefresh();
    await scenarioPagerScrolling();
    await scenarioChunkedStreaming();
    await scenarioMalformedBytes();
    await scenarioAlternateVariants();
    await scenarioDisconnectRestoresShellBuffer();

    if (failures !== 0) {
        console.log(failures + ' check(s) failed');
        process.exit(1);
    }
    console.log('FULLSCREEN_REGRESSION_OK');
}

main().catch((error) => {
    ++failures;
    console.log('FAIL: scenario threw: ' + (error && error.stack || error));
    process.exit(1);
});
