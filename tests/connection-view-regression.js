// Connection card grouping, sorting and type-label regression.
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8')
  .replaceAll('\r\n', '\n');
const app = read('resources/web/app.js');
const index = read('resources/web/index.html');
const backend = read('src/WebViewBackend.cpp');
const window = read('src/WebViewWindow.cpp');
const host = read('src/WebViewHost.cpp');
const sessionCpp = read('src/RdpSession.cpp');
const css = read('resources/web/app.css');

let failures = 0;
function check(name, condition) {
  if (condition) console.log('PASS: ' + name);
  else {
    ++failures;
    console.log('FAIL: ' + name);
  }
}

check('default view groups by type and sorts by name',
  app.includes('serverSortMode: "name-asc"')
    && app.includes('serverGroupMode: "type"')
    && app.includes('serverDisplayGroups')
    && app.includes('compareServerProfiles'));
check('view menu exposes supported sorting and grouping modes',
  index.includes('id="server-view-menu"')
    && index.includes('value="recent"')
    && index.includes('value="workspace-type"')
    && index.includes('value="manual"'));
check('view menu controls are not closed before change events',
  app.includes('if (!serverContextMenu.hidden && !serverContextMenu.contains(event.target)) {\n      closeServerContextMenu();\n    }')
    && !app.includes('if (!serverContextMenu.hidden && !serverContextMenu.contains(event.target))\n      closeServerContextMenu();\n      closeServerViewMenu();'));
check('view menu has a rounded native-RDP overlay surface',
  app.includes('serverViewMenu.hidden = !serverViewMenu.hidden')
    && app.includes('notifyRdpLayout(false, clearOcclusion)')
    && app.includes('server-view-menu'));
check('connection type labels and colors are configurable',
  app.includes('profileDisplayTag')
    && app.includes('connectionTypeColorSsh')
    && app.includes('connectionTypeColorRdp')
    && app.includes('connectionTypeColorSerial')
    && app.includes('data-connection-type')
    && app.includes('--terminal-tab-type-color')
    && css.includes('var(--terminal-tab-type-color')
    && index.includes('setting-connection-color-ssh')
    && index.includes('settings-reset-connection-colors'));
check('custom tags remain compatible with automatic tags',
  index.includes('id="server-tag-mode"')
    && app.includes('tagMode: document.querySelector("#server-tag-mode").value')
    && backend.includes('tagMode')
    && backend.includes('defaultServerTag'));
check('cloud migration preserves tag mode',
  app.includes('tagMode: item.tagMode === "custom" ? "custom" : "auto"'));
check('disabled connection actions remain menu items',
  window.includes('{ServerContextMoveUp, L"上移", false, !canMoveUp}')
    && window.includes('{ServerContextMoveDown, L"下移", false, !canMoveDown}'));
check('metric columns are measured once and stabilized',
  app.includes('const serverMetricLayouts = new Map()')
    && app.includes('stabilizeServerMetricsLayout')
    && app.includes('server-metrics-measuring')
    && css.includes('.server-metrics-measuring'));
check('copy confirmation toast is a visible success notification',
  app.includes('toast.className = "toast toast-success"')
    && app.includes('toast.setAttribute("role", "status")')
    && css.includes('.toast::before')
    && css.includes('left: 50%')
    && css.includes('transform: translateX(-50%)')
    && css.includes('min-width: 174px')
    && css.includes('html[data-theme="dark"] .toast')
    && css.includes('html[data-theme="light"] .toast')
    && css.includes('html[data-theme="blue"] .toast'));
check('sidebar shadow is exposed above the native RDP surface',
  index.includes('id="sidebar-card-shadow"')
    && css.includes('#sidebar-card-shadow')
    && css.includes('width: 34px')
    && css.includes('rgba(71,85,105,.07) 44%')
    && css.includes('sidebar-auto-hide.sidebar-flyout-open > #sidebar-card-shadow')
    && app.includes('const sidebarCardShadow = document.querySelector("#sidebar-card-shadow")')
    && app.includes('sidebarCardShadow.hidden = sidebarAutoHide')
    && app.includes('sidebarCardShadow.hidden = false')
    && app.includes('sidebarCardShadow.hidden = true')
    && app.includes('"#sidebar-flyout-occlusion", "#sidebar-card-shadow", ".context-menu"')
    && host.includes("const isShadow=e.id==='sidebar-card-shadow'?1:0;")
    && host.includes('[l,t,r2,b2,q*d,0,isShadow]')
    && host.includes('isShadowFlag')
    && host.includes('isShadowFlag != 0.0')
    && sessionCpp.includes('nativeGradientShadow')
    && sessionCpp.includes('UpdateLayeredWindow')
    && host.includes('L"#sidebar-card-shadow:not([hidden]),"'));
check('auto-hide flyout uses an atomic native-RDP transition',
  app.includes('const nativeRdp = activeRdpVisible();')
    && app.includes('sidebar-flyout-native-rdp')
    && app.includes('Promise.resolve(notifyRdpLayout(false, true)).finally')
    && app.includes('only then remove the HTML panel')
    && css.includes('sidebar-flyout-native-rdp > .function-panels')
    && css.includes('transition: none; will-change: auto;')
    && !app.includes('setTimeout(finish, 165)'));
check('sidebar hover coalesces panel swaps and cancels an in-flight close',
  app.includes('const resumeSidebarFlyout = () =>')
    && app.includes('if (sidebarFlyoutOcclusion.hidden) {')
    && app.includes('++sidebarFlyoutGeneration;')
    && app.includes('sidebarFlyoutOpenTimer = setTimeout(activateAndShow, 110);')
    && app.indexOf('setTimeout(activateAndShow, 110)')
      < app.indexOf('const scheduleSidebarFlyoutClose'));

if (failures) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('CONNECTION_VIEW_REGRESSION_OK');
