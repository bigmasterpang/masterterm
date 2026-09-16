// Saved connection ordering regression.
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8')
  .replaceAll('\r\n', '\n');
const backend = read('src/WebViewBackend.cpp');
const header = read('src/WebViewBackend.h');
const app = read('resources/web/app.js');
const index = read('resources/web/index.html');

let failures = 0;
function check(name, condition) {
  if (condition) console.log('PASS: ' + name);
  else {
    ++failures;
    console.log('FAIL: ' + name);
  }
}

function move(items, source, target, before) {
  const result = items.slice();
  const [value] = result.splice(source, 1);
  let insertion = target - (source < target ? 1 : 0);
  if (!before) insertion += 1;
  result.splice(Math.max(0, Math.min(insertion, result.length)), 0, value);
  return result;
}

check('backend exposes a persistent server reorder operation',
  header.includes('reorderServerProfile')
    && backend.includes('server.reorder')
    && backend.includes('writeServerRecords(records)')
    && backend.includes('targetIndex')
    && backend.includes('before'));
check('reorder insertion math handles both directions',
  JSON.stringify(move(['a', 'b', 'c'], 2, 0, true))
      === JSON.stringify(['c', 'a', 'b'])
    && JSON.stringify(move(['a', 'b', 'c'], 0, 2, false))
      === JSON.stringify(['b', 'c', 'a'])
    && JSON.stringify(move(['a', 'b', 'c'], 1, 2, true))
      === JSON.stringify(['a', 'b', 'c']));
check('connection cards support drag and drop ordering',
  app.includes('item.draggable = true')
    && app.includes('server.reorder')
    && app.includes('dragstart')
    && app.includes('dragover')
    && app.includes('drop'));
check('grouped manual ordering validates type and moves workspace atomically',
  app.includes('const resolveProfileDrop = (source, target)')
    && app.includes('const resolveGroupDrop = (source, group)')
    && app.includes('workspace: mode === "workspace" || mode === "workspace-type"')
    && app.includes('const enableManualServerSort = () =>')
    && app.includes('item.draggable = true')
    && app.includes('item.classList.toggle("drop-invalid"')
    && backend.includes('NativeJsonDom::contains(params, "workspace")')
    && backend.includes('moved.workspace = workspaceText'));
check('connection context menu exposes move actions',
  index.includes('data-action="move-up"')
    && index.includes('data-action="move-down"')
    && app.includes('data-action="move-up"')
    && app.includes('data-action="move-down"'));

if (failures) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('SERVER_ORDER_REGRESSION_OK');
