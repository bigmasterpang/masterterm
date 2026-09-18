(() => {
  // The WebView2 controller stays hidden while the native splash runs.  Once
  // the first content frame is rendered, remove the in-page overlay and tell
  // the host to reveal the fully rendered interface in one step.
  let frontendReady = false;
  const notifyFrontendReady = () => {
    if (frontendReady) return;
    frontendReady = true;
    // Paint the page background only now: until the frontend has real content
    // the WebView2 stays transparent so the native splash is the only screen.
    document.documentElement.style.background =
      themes[activeTheme]?.pageBackground || "#111827";
    document.querySelector("#startup-splash")?.remove();
    post("app.ready").catch(() => {});
    refreshDetectedShells().catch(() => {});
  };
  const pending = new Map();
  const sessions = new Map();
  const hasRdpSession = () => Array.from(sessions.values())
    .some(session => session.connectionType === "rdp");
  const profilesByIndex = new Map();
  const sftpPathsByProfile = new Map();
  const sftpStatesBySession = new Map();
  let activeSession = null;
  let rdpFullscreen = false;
  let rdpFullscreenSessionId = "";
  let rdpFullscreenNativeBar = false;
  const rdpQualityBySession = new Map();
  // Fullscreen controls are transient by default; pinning is opt-in.
  let rdpFullscreenBarPinned = false;
  let rdpFullscreenBarVisible = false;
  let rdpFullscreenBarTimer = 0;
  let rdpFullscreenMenu = "";
  // In a split view each pane has its own active tab.  Keep the session that
  // the user is actually interacting with separately, so the SFTP pane and
  // header metrics always describe the focused terminal rather than always
  // describing the left pane.
  let focusedSessionId = "";
  let sessionActivationGeneration = 0;
  let splitMode = null;
  let splitRightIds = [];
  let splitRightActive = -1;
  let splitViewLeft = null;
  let splitViewRight = null;
  let splitDivider = null;
  let splitRatio = .5;
  let contextMenuSessionId = null;
  let terminalOutputContextSessionId = null;
  let terminalTabsContextGroup = null;
  let pendingNewTerminalGroup = null;
  let broadcastInputActive = false;
  let handleTerminalTabsAction = null;
  let renderSnippetsPanel = null;
  let refreshSidebarTunnelSessions = null;
  let refreshTunnelsPanel = null;
  let updateSidebarCloudUi = null;
  const rdpAutoReconnectAttemptsByProfile = new Map();
  let nativeQuickOpenGroup = null;
  let activeSftpSessionId = null;
  let sequence = 0;
  // The header status is shared by all sessions. Keep ownership of the RDP
  // connecting message explicit so a late native event or a closed tab cannot
  // leave "正在连接远程桌面" behind after the desktop is already usable.
  let rdpConnectingStatusSessionId = "";
  let rdpConnectedStatusSessionId = "";
  let rdpConnectedStatusTimer = null;
  let connectionSuccessStatusTimer = null;
  let connectionSuccessStatusMessage = "";
  let connectionSuccessStatusSessionId = "";

  const status = document.querySelector("#status");
  let transientStatusTimer = 0;
  let transientStatusMessage = "";
  let transientStatusToken = 0;
  const clearTransientStatusTimer = () => {
    if (transientStatusTimer) {
      clearTimeout(transientStatusTimer);
      transientStatusTimer = 0;
    }
    transientStatusMessage = "";
    transientStatusToken++;
  };
  const setHeaderStatus = message => {
    clearTransientStatusTimer();
    status.textContent = String(message || "");
  };
  const showTransientStatus = (message, duration = 5000) => {
    clearTransientStatusTimer();
    const value = String(message || "");
    status.textContent = value;
    if (!value) return;
    transientStatusMessage = value;
    const token = transientStatusToken;
    transientStatusTimer = setTimeout(() => {
      if (token === transientStatusToken && status.textContent === value)
        status.textContent = "";
      if (token === transientStatusToken) {
        transientStatusMessage = "";
        transientStatusTimer = 0;
      }
    }, duration);
  };
  const serverMetrics = document.querySelector("#server-metrics");
  const terminalStatusBar = document.querySelector("#terminal-status-bar");
  const terminalStatusIdentity = document.querySelector("#terminal-status-identity");
  const terminalStatusDot = document.querySelector("#terminal-status-dot");
  const terminalStatusTitle = document.querySelector("#terminal-status-title");
  const rdpTabHoverCard = document.querySelector("#rdp-tab-hover-card");
  // Measure each session's metric columns once. Values such as network rates
  // change frequently; re-measuring every sample would move the header.
  const serverMetricLayouts = new Map();
  const terminalTabs = document.querySelector("#terminal-tabs");
  const terminalPanels = document.querySelector("#terminal-panels");
  const terminalEmpty = document.querySelector("#terminal-empty");
  const terminalContextMenu = document.querySelector("#terminal-context-menu");
  const rdpFullscreenBar = document.querySelector("#rdp-fullscreen-bar");
  const rdpFullscreenHotZone = document.querySelector("#rdp-fullscreen-hot-zone");
  const rdpFullscreenPin = document.querySelector("#rdp-fullscreen-pin");
  const rdpFullscreenDisplay = document.querySelector("#rdp-fullscreen-display");
  const rdpFullscreenConnection = document.querySelector("#rdp-fullscreen-connection");
  const rdpFullscreenKeyboard = document.querySelector("#rdp-fullscreen-keyboard");
  const rdpFullscreenQuality = document.querySelector("#rdp-fullscreen-quality");
  const rdpFullscreenClose = document.querySelector("#rdp-fullscreen-close");
  const rdpFullscreenMenuElement = document.querySelector("#rdp-fullscreen-menu");
  const rdpQualityPanel = document.querySelector("#rdp-quality-panel");
  const rdpQualityContent = document.querySelector("#rdp-quality-content");
  const rdpQualityClose = document.querySelector("#rdp-quality-close");
  const rdpKeyToolboxBtn = document.querySelector("#rdp-key-toolbox-btn");
  const rdpKeyMenu = document.querySelector("#rdp-key-menu");
  const closeRdpKeyMenu = () => {
    if (!rdpKeyMenu) return;
    const wasVisible = !rdpKeyMenu.hidden;
    rdpKeyMenu.hidden = true;
    if (wasVisible && typeof clearRdpOverlay === "function") clearRdpOverlay();
  };
  const terminalCreateGroup = document.querySelector(".terminal-create-group");
  const terminalOutputContextMenu = document.querySelector("#terminal-output-context-menu");

  const isRdpConnectingStatus = value =>
    /^(?:正在打开远程桌面|正在连接远程桌面)/.test(String(value || ""));
  const clearRdpConnectingStatus = (sessionId, replacement = "") => {
    if (rdpConnectingStatusSessionId !== sessionId) return false;
    rdpConnectingStatusSessionId = "";
    if (isRdpConnectingStatus(status.textContent))
      setHeaderStatus(replacement);
    return true;
  };
  const clearRdpConnectedStatus = sessionId => {
    if (rdpConnectedStatusSessionId !== sessionId) return false;
    if (rdpConnectedStatusTimer) {
      clearTimeout(rdpConnectedStatusTimer);
      rdpConnectedStatusTimer = null;
    }
    rdpConnectedStatusSessionId = "";
    if (connectionSuccessStatusSessionId === sessionId) {
      if (connectionSuccessStatusTimer) {
        clearTimeout(connectionSuccessStatusTimer);
        connectionSuccessStatusTimer = null;
      }
      connectionSuccessStatusSessionId = "";
      connectionSuccessStatusMessage = "";
    }
    if (status.textContent === "远程桌面已连接")
      setHeaderStatus("");
    return true;
  };
  const showTransientConnectionStatus = (message, sessionId) => {
    clearTransientStatusTimer();
    if (connectionSuccessStatusTimer) {
      clearTimeout(connectionSuccessStatusTimer);
      connectionSuccessStatusTimer = null;
    }
    connectionSuccessStatusMessage = String(message || "");
    connectionSuccessStatusSessionId = String(sessionId || "");
    status.textContent = connectionSuccessStatusMessage;
    connectionSuccessStatusTimer = setTimeout(() => {
      if (status.textContent === connectionSuccessStatusMessage) {
        status.textContent = "";
      }
      connectionSuccessStatusMessage = "";
      connectionSuccessStatusSessionId = "";
      connectionSuccessStatusTimer = null;
    }, 4000);
  };
  const showRdpConnectedStatus = sessionId => {
    if (rdpConnectedStatusTimer) {
      clearTimeout(rdpConnectedStatusTimer);
      rdpConnectedStatusTimer = null;
    }
    rdpConnectedStatusSessionId = sessionId;
    showTransientConnectionStatus("远程桌面已连接", sessionId);
    // Connection success is a transient event, not a permanent application
    // status. Keep the RDP owner explicit so a later tab close or switch can
    // clear the message even before the timer expires.
    rdpConnectedStatusTimer = setTimeout(() => {
      if (rdpConnectedStatusSessionId === sessionId) {
        clearRdpConnectedStatus(sessionId);
      }
      rdpConnectedStatusTimer = null;
    }, 4000);
  };
  const syncRdpStatus = session => {
    if (!session || session.connectionType !== "rdp"
        || activeSession !== session) return;
    if (session.state === "connecting") {
      rdpConnectingStatusSessionId = session.sessionId;
      setHeaderStatus("正在连接远程桌面…");
    } else if (session.state === "connected") {
      clearRdpConnectingStatus(session.sessionId);
      showRdpConnectedStatus(session.sessionId);
    } else {
      clearRdpConnectingStatus(session.sessionId);
      clearRdpConnectedStatus(session.sessionId);
    }
  };
  const terminalDiagnosticsDialog = document.querySelector("#terminal-diagnostics-dialog");
  const terminalDiagnosticsContent = document.querySelector("#terminal-diagnostics-content");
  const commandFavoritesDialog = document.querySelector("#command-favorites-dialog");
  const batchCommandDialog = document.querySelector("#batch-command-dialog");
  const batchCommandInput = document.querySelector("#batch-command-input");
  const batchCommandSessions = document.querySelector("#batch-command-sessions");
  const batchCommandStatus = document.querySelector("#batch-command-status");
  const batchCommandRun = document.querySelector("#batch-command-run");
  const commandFavoritesList = document.querySelector("#command-favorites-list");
  const commandFavoritesEmpty = document.querySelector("#command-favorites-empty");
  const commandFavoriteEditor = document.querySelector("#command-favorite-editor");
  const commandFavoriteEditName = document.querySelector("#command-favorite-edit-name");
  const commandFavoriteEditCommand = document.querySelector("#command-favorite-edit-command");
  const themeButton = document.querySelector("#theme-button");
  const themeMenu = document.querySelector("#theme-menu");
  const settingsButton = document.querySelector("#settings-button");
  const settingsDialog = document.querySelector("#settings-dialog");
  const settingsForm = document.querySelector("#settings-form");
  const settingsStatus = document.querySelector("#settings-status");
  const settingsTerminalPreview = document.querySelector("#terminal-settings-preview");
  const workspace = document.querySelector("#workspace");
  const sftpFiles = document.querySelector("#sftp-files");
  const sftpTableWrap = document.querySelector(".sftp-table-wrap");
  const sftpEmpty = document.querySelector("#sftp-empty");
  const sftpStatus = document.querySelector("#sftp-status");
  const sftpPathInput = document.querySelector("#sftp-path");
  const sftpBackButton = document.querySelector("#sftp-back");
  const sftpForwardButton = document.querySelector("#sftp-forward");
  const sftpUploadButton = document.querySelector("#sftp-upload");
  const sftpDownloadButton = document.querySelector("#sftp-download");
  const sftpCancelButton = document.querySelector("#sftp-cancel");
  const sftpStatusText = document.querySelector("#sftp-status-text");
  const sftpTransferRow = document.querySelector("#sftp-transfer-row");
  const sftpProgressBar = document.querySelector("#sftp-progress-bar");
  const sftpProgressPercent = document.querySelector("#sftp-progress-percent");
  const sftpProgressSpeed = document.querySelector("#sftp-progress-speed");
  const sftpProgressEta = document.querySelector("#sftp-progress-eta");
  const sftpVerifyChecksum = document.querySelector("#sftp-verify-checksum");
  const sftpTransferResults = document.querySelector("#sftp-transfer-results");
  const sftpTransferResultsList = document.querySelector("#sftp-transfer-results-list");
  const sftpCurrentTransferName = document.querySelector("#sftp-current-transfer-name");
  const sftpTransferStatusText = sftpCurrentTransferName;
  const sftpTransferResultsHeader = document.querySelector(".sftp-transfer-results-header");
  const sftpTransferTabs = document.querySelectorAll("[data-transfer-tab]");
  const sftpTransferFilter = document.querySelector("#sftp-transfer-filter");
  const sftpTransferRetryFailedButton = document.querySelector("#sftp-transfer-retry-failed");
  const sftpTransferContextMenu = document.querySelector("#sftp-transfer-context-menu");
  const sftpStatusResizer = document.querySelector("#sftp-status-resizer");
  const sftpContextMenu = document.querySelector("#sftp-context-menu");
  const remotePreviewDialog = document.querySelector("#remote-preview-dialog");
  const remotePreviewName = document.querySelector("#remote-preview-name");
  const remotePreviewContent = document.querySelector("#remote-preview-content");
  const remotePreviewStatus = document.querySelector("#remote-preview-status");
  const sftpFavoriteButton = document.querySelector("#sftp-favorite-toggle");
  const sftpFavoritesMenu = document.querySelector("#sftp-favorites-menu");
  const sftpFavoritesList = document.querySelector("#sftp-favorites-list");
  const sftpSplit = document.querySelector(".sftp-split");
  const sftpLocalToggle = document.querySelector("#sftp-local-toggle");
  const sftpLocalPane = document.querySelector(".local-pane");
  const sftpPaneDivider = document.querySelector(".sftp-pane-divider");
  const functionTabs = document.querySelector("#function-tabs");
  const functionPanels = document.querySelector("#function-panels");
  const sidebarResizer = document.querySelector("#sidebar-resizer");
  const sidebarPin = document.querySelector("#sidebar-pin");
  const sidebarPanelTitle = document.querySelector("#sidebar-panel-title");
  const sidebarSftpSession = document.querySelector("#sidebar-sftp-session");
  const sidebarFlyoutOcclusion = document.querySelector("#sidebar-flyout-occlusion");
  const sidebarCardShadow = document.querySelector("#sidebar-card-shadow");
  const sftpFunctionBadge = document.querySelector("#sftp-function-badge");
  const localFiles = document.querySelector("#local-files");
  const localTableWrap = document.querySelector(".local-table-wrap");
  const localContextMenu = document.querySelector("#local-context-menu");
  const localEmpty = document.querySelector("#local-empty");
  const localPathInput = document.querySelector("#local-path");
  const localOpenFolderButton = document.querySelector("#local-open-folder");
  const localBackButton = document.querySelector("#local-back");
  const localUploadButton = document.querySelector("#local-upload");
  const localFavoriteButton = document.querySelector("#local-favorite-toggle");
  const localFavoritesMenu = document.querySelector("#local-favorites-menu");
  const localFavoritesList = document.querySelector("#local-favorites-list");
  const localStatus = document.querySelector("#local-status");
  const serverDialog = document.querySelector("#server-dialog");
  const serverForm = document.querySelector("#server-form");
  const serverType = document.querySelector("#server-type");
  const rdpAdvancedSettings = document.querySelector("#rdp-advanced-settings");
  const rdpResetDefaultsButton = document.querySelector(
    "#server-rdp-reset-defaults");
  const rdpQualityPresetSelect = document.querySelector(
    "#server-rdp-quality-preset");
  const rdpOptionFields = {
    colorDepth: document.querySelector("#server-rdp-color-depth"),
    smartSizing: document.querySelector("#server-rdp-smart-sizing"),
    performanceFlags: document.querySelector("#server-rdp-performance"),
    networkConnectionType: document.querySelector("#server-rdp-network-type"),
    desktopBackground: document.querySelector("#server-rdp-wallpaper"),
    fontSmoothing: document.querySelector("#server-rdp-font-smoothing"),
    desktopComposition: document.querySelector("#server-rdp-composition"),
    fullWindowDrag: document.querySelector("#server-rdp-full-window-drag"),
    menuAnimations: document.querySelector("#server-rdp-menu-animations"),
    visualStyles: document.querySelector("#server-rdp-visual-styles"),
    cursorShadow: document.querySelector("#server-rdp-cursor-shadow"),
    cursorSettings: document.querySelector("#server-rdp-cursor-settings"),
    useMultimon: document.querySelector("#server-rdp-use-multimon"),
    keyboardHookMode: document.querySelector("#server-rdp-keyboard-hook"),
    redirectClipboard: document.querySelector("#server-rdp-clipboard"),
    audioRedirectionMode: document.querySelector("#server-rdp-audio"),
    redirectDrives: document.querySelector("#server-rdp-drives"),
    redirectPrinters: document.querySelector("#server-rdp-printers"),
    autoReconnect: document.querySelector("#server-rdp-auto-reconnect"),
    maxReconnectAttempts: document.querySelector("#server-rdp-reconnect-attempts")
  };
  const rdpCheckboxKeys = new Set([
    "desktopBackground", "fontSmoothing", "desktopComposition",
    "fullWindowDrag", "menuAnimations", "visualStyles", "cursorShadow",
    "cursorSettings"
  ]);
  const defaultRdpOptionValue = key => key === "maxReconnectAttempts"
    ? 3 : key === "useMultimon" ? 1 : key === "autoReconnect" ? 0 : -1;
  const serialPortSelect = document.querySelector("#server-serial-port");
  const serialPortRefreshButton = document.querySelector("#server-serial-refresh");
  const serialPortStatus = document.querySelector("#server-serial-status");
  const serverFormStatus = document.querySelector("#server-form-status");
  const serverContextMenu = document.querySelector("#server-context-menu");
  const serverViewOptions = document.querySelector("#server-view-options");
  const serverViewMenu = document.querySelector("#server-view-menu");
  const serverSortMode = document.querySelector("#server-sort-mode");
  const serverGroupMode = document.querySelector("#server-group-mode");
  const workspaceFilterSelect = document.querySelector("#workspace-filter-select");
  const workspaceManageButton = document.querySelector("#workspace-manage");
  const workspaceContextMenu = document.querySelector("#workspace-context-menu");
  const actionDialog = document.querySelector("#action-dialog");
  const actionDialogTitle = document.querySelector("#action-dialog-title");
  const actionDialogMessage = document.querySelector("#action-dialog-message");
  const actionDialogInput = document.querySelector("#action-dialog-input");
  const actionDialogSelect = document.querySelector("#action-dialog-select");
  const actionDialogActions = document.querySelector("#action-dialog-actions");
  let contextMenuProfile = null;
  const selectedProfileIndexes = new Set();
  let profilesCache = [];
  let draggingProfileIndex = null;
  let suppressServerClick = false;
  let workspaceNames = ["未分配"];
  let activeWorkspace = "全部连接";
  let serverSearchQuery = "";
  let displayedProfileOrder = [];
  const collapsedServerGroups = new Set();
  const serverSearchInput = document.querySelector("#server-search-input");
  let contextMenuWorkspace = "";
  const recentConnectionsStorageKey = "masterterm.recentConnections";
  const recentConnectionKey = profile => [
    profile?.connectionType || "ssh", profile?.name || "",
    profile?.address || "", profile?.port || ""
  ].join("\u0000");
  const readRecentConnectionTimes = () => {
    try {
      const value = JSON.parse(
        localStorage.getItem(recentConnectionsStorageKey) || "{}");
      return value && typeof value === "object" && !Array.isArray(value)
        ? value : {};
    } catch {
      return {};
    }
  };
  const rememberRecentConnection = profile => {
    const times = readRecentConnectionTimes();
    times[recentConnectionKey(profile)] = Date.now();
    const entries = Object.entries(times)
      .sort((first, second) => second[1] - first[1])
      .slice(0, 100);
    localStorage.setItem(recentConnectionsStorageKey,
      JSON.stringify(Object.fromEntries(entries)));
  };
  const sortRecentConnections = profiles => {
    const times = readRecentConnectionTimes();
    return profiles.map((profile, index) => ({ profile, index }))
      .sort((first, second) => {
        const firstTime = Number(times[recentConnectionKey(first.profile)]) || 0;
        const secondTime = Number(times[recentConnectionKey(second.profile)]) || 0;
        return secondTime - firstTime || first.index - second.index;
      })
      .map(item => item.profile);
  };
  let sftpProfile = null;
  let sftpPath = "/";
  let sftpGeneration = 0;
  let sftpHistory = [];
  let sftpHistoryIndex = -1;
  let sftpSelectedEntry = null;
  let sftpEntriesByPath = new Map();
  let sftpSelectedPaths = new Set();
  let sftpDirectoryEntries = [];
  let sftpSort = { key: "name", direction: 1 };
  let localPath = "";
  let localParentPath = "";
  let localHistory = [];
  let localHistoryIndex = -1;
  let localSelectedEntry = null;
  let localEntriesByPath = new Map();
  let localSelectedPaths = new Set();
  let localDirectoryEntries = [];
  let localSort = { key: "name", direction: 1 };
  let activeFilePane = "remote";
  let localLoaded = false;
  let localPanelVisible = false;
  let sidebarWidthBeforeLocal = 0;
  let activeFunctionPanel = "connections";
  let sidebarAutoHide = localStorage.getItem("masterterm.sidebarMode") === "auto";
  let sidebarFlyoutOpenTimer = 0;
  let sidebarFlyoutCloseTimer = 0;
  let sidebarFlyoutGeneration = 0;
  let activeTransferId = null;
  let activeTransferOperation = "";
  let activeTransferBatch = null;
  let activeTransferTask = null;
  let activeTransferPaused = false;
  let transferRetryCount = 0;
  const maximumTransferRetries = 2;
  let transferStarting = false;
  const transferQueue = [];
  const transferResults = [];
  const remoteEditTransfers = new Map();
  // ---- 传输队列持久化（T3-1）----
  // 未完成任务（等待中、传输中、失败）保存在 localStorage，重启后提示恢复。
  // 流式拖放上传与远程编辑同步无法跨重启恢复，不参与持久化。
  const transferQueueStorageKey = "masterterm.transferQueue.v1";
  let transferQueueHydrated = false;
  let transferPersistTimer = 0;
  const serializeTransferCheckpoint = task => {
    const files = task?.checkpoint?.files;
    if (!files || typeof files !== "object") return undefined;
    const snapshot = {};
    for (const [name, value] of Object.entries(files)) {
      if (!name || !value || typeof value !== "object") continue;
      const done = Math.max(0, Number(value.done || 0));
      const total = Math.max(0, Number(value.total || 0));
      if (total > 0 || done > 0)
        snapshot[name] = {
          done: Math.min(done, total || done), total,
          state: value.state === "complete" ? "complete" : "partial"
        };
    }
    return Object.keys(snapshot).length
      ? { version: 1, files: snapshot, updatedAt: Date.now() }
      : undefined;
  };
  const recoverableTransferSnapshot = () => {
    const tasks = [];
    const pushTask = task => {
      if (!task) return;
      if (task.type === "upload")
        tasks.push({
          type: "upload", index: task.index ?? sftpProfile?.index ?? -1,
          path: task.path, temporary: !!task.temporary,
          conflict: task.conflict || "overwrite", directory: !!task.directory,
          remoteDirectory: task.remoteDirectory || "",
          remoteName: task.remoteName || "",
          syncMode: !!task.syncMode, verify: !!task.verify,
          resumeDirectory: !!task.resumeDirectory,
          checkpoint: serializeTransferCheckpoint(task)
        });
      else if (task.type === "download" && task.entry)
        tasks.push({
          type: "download", index: task.index ?? sftpProfile?.index ?? -1,
          entry: { name: task.entry.name, path: task.entry.path,
            directory: !!task.entry.directory },
          destination: task.destination || "", directory: !!task.directory,
          conflict: task.conflict || "overwrite", verify: !!task.verify,
          resumeDirectory: !!task.resumeDirectory,
          checkpoint: serializeTransferCheckpoint(task)
        });
    };
    if (activeTransferTask) pushTask(activeTransferTask);
    for (const task of transferQueue) pushTask(task);
    for (const result of transferResults)
      if (result.state === "error" || result.state === "cancelled")
        pushTask(result);
    return tasks;
  };
  const persistTransferQueue = () => {
    if (!transferQueueHydrated) return;
    try {
      localStorage.setItem(transferQueueStorageKey, JSON.stringify({
        version: 1, tasks: recoverableTransferSnapshot()
      }));
    } catch { /* 存储不可用时持久化按尽力而为处理 */ }
  };
  const scheduleTransferPersist = () => {
    if (!transferQueueHydrated || transferPersistTimer) return;
    transferPersistTimer = setTimeout(() => {
      transferPersistTimer = 0;
      persistTransferQueue();
    }, 1500);
  };
  // 启动恢复：上次运行未完成的传输任务提示继续或清理（T3-1）。
  const recoverPersistedTransfers = async () => {
    let stored = null;
    try {
      stored = JSON.parse(
        localStorage.getItem(transferQueueStorageKey) || "null");
    } catch { localStorage.removeItem(transferQueueStorageKey); }
    const tasks = stored && stored.version === 1 && Array.isArray(stored.tasks)
      ? stored.tasks.filter(task => task && (task.type === "upload"
        || task.type === "download")) : [];
    transferQueueHydrated = true;
    if (!tasks.length) return;
    const names = tasks.slice(0, 6).map(task => task.type === "upload"
      ? (task.remoteName || String(task.path).split(/[\\/]/).at(-1) || "文件")
      : (task.entry?.name || "文件"));
    const choice = await showActionDialog({
      title: "发现未完成的传输任务",
      message: `上次运行时 ${tasks.length} 个传输任务未完成`
        + `（${names.join("、")}${tasks.length > 6 ? " 等" : ""}）。\n\n`
        + "继续会重新开始这些任务；清理会丢弃记录（不影响已存在的文件）。",
      value: null,
      buttons: [
        { action: "discard", label: "清理" },
        { action: "resume", label: "继续全部", primary: true }
      ]
    });
    if (!choice || choice.action !== "resume") {
      localStorage.removeItem(transferQueueStorageKey);
      persistTransferQueue();
      return;
    }
    for (const task of tasks) {
      if (task.type === "upload")
        startSftpUpload(task.path, !!task.temporary, true,
          task.conflict || "overwrite", !!task.directory,
          task.remoteDirectory || "", task.remoteName || "",
          !!task.syncMode, 0, !!task.verify, task.index, true,
          task.checkpoint || null);
      else
        startSftpDownload({
          name: task.entry?.name || "文件",
          path: task.entry?.path || "",
          directory: !!task.entry?.directory
        }, true, task.destination || "", task.conflict || "overwrite",
          0, !!task.verify, task.index, true, task.checkpoint || null);
    }
    persistTransferQueue();
    sftpTransferStatusText.textContent =
      `已恢复 ${tasks.length} 个传输任务，正在继续传输…`;
  };
  const defaultTransferColumnWidths = [24, 170, 52, 78, 118, 78, 180];
  let transferColumnWidths = [...defaultTransferColumnWidths];
  let activeTransferTab = "queue";
  let transferFilterText = "";
  let transferContextTarget = null;
  let selectedTransferTarget = null;
  let transferQueuePaused = false;
  let transferConflictPolicy = null;
  let transferLastRenderAt = 0;
  let transferResizeStart = null;
  let transferColumnResizeStart = null;
  const localDragType = "application/x-masterterm-local-paths";
  const remoteDragType = "application/x-masterterm-remote-paths";
  let refreshRemoteAfterTransfers = false;
  let refreshLocalAfterTransfers = false;
  let suppressSelectionClick = false;
  const transferStartedAt = new Map();
  let terminalLibrariesPromise = null;
  let terminalFontFamilies = [];
  let activeTheme = localStorage.getItem("masterterm.theme") || "dark";
  let detectedShells = null;
  const refreshDetectedShells = async () => {
    try {
      const list = await post("local.detectShells");
      if (Array.isArray(list)) {
        detectedShells = new Map(list.map(s => [s.id, Boolean(s.available || s.exists)]));
        return detectedShells;
      }
    } catch (_) {}
    return null;
  };
  const sessionRestoreStorageKey = "masterterm.sessionRestore";
  let restoringSessions = false;
  const defaultSettings = {
    localPanelOnOpen: false,
    confirmDelete: true,
    restoreSessions: false,
    keepalive: 30,
    autoReconnect: false,
    reconnectAttempts: 3,
    remoteEditConflict: "overwrite",
    fontSize: 14,
    fontFamily: "Cascadia Mono, Consolas, Courier New, monospace",
    scrollback: 5000,
    terminalBackground: "",
    terminalForeground: "",
    historyLimit: 500,
    completionLimit: 5,
    historyDays: 0,
    readBashHistory: true,
    readZshHistory: true,
    deduplicateHistory: true,
    copyOnSelect: true,
    cursorStyle: "block",
    cursorBlink: true,
    alertsEnabled: false,
    alertCpuThreshold: 90,
    alertMemoryThreshold: 90,
    alertDiskThreshold: 90,
    sessionLogging: false,
    editorAssociations: {},
    serverSortMode: "name-asc",
    serverGroupMode: "type",
    connectionTypeColorSsh: "#ef4444",
    connectionTypeColorRdp: "#3b82f6",
    connectionTypeColorSerial: "#f59e0b",
    defaultLocalShell: "auto",
    localWorkingDir: "",
    terminalThemePreset: "custom",
    updateServerUrl: "https://vm.dapang.wang/updates/masterterm.json"
  };
  let appSettings = { ...defaultSettings };
  try {
    const stored = JSON.parse(localStorage.getItem("masterterm.settings") || "{}");
    if (stored && typeof stored === "object")
      appSettings = { ...defaultSettings, ...stored };
  } catch { /* Use defaults when an older setting value is malformed. */ }
  const saveSettings = () => {
    localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
  };
  const normalizeAppSettings = settings => {
    const value = { ...defaultSettings, ...(settings || {}) };
    const sortModes = new Set(["manual", "name-asc", "name-desc", "address-asc", "recent"]);
    const groupModes = new Set(["none", "type", "workspace", "workspace-type"]);
    if (!sortModes.has(value.serverSortMode)) value.serverSortMode = defaultSettings.serverSortMode;
    if (!groupModes.has(value.serverGroupMode)) value.serverGroupMode = defaultSettings.serverGroupMode;
    if (!value.defaultLocalShell) value.defaultLocalShell = defaultSettings.defaultLocalShell;
    if (typeof value.localWorkingDir !== "string") value.localWorkingDir = defaultSettings.localWorkingDir;
    if (!value.terminalThemePreset) value.terminalThemePreset = defaultSettings.terminalThemePreset;
    for (const key of ["connectionTypeColorSsh", "connectionTypeColorRdp", "connectionTypeColorSerial"])
      if (!/^#[0-9a-f]{6}$/i.test(String(value[key] || ""))) value[key] = defaultSettings[key];
    return value;
  };
  appSettings = normalizeAppSettings(appSettings);
  const connectionTypeColor = connectionType => {
    const colors = {
      ssh: appSettings.connectionTypeColorSsh,
      rdp: appSettings.connectionTypeColorRdp,
      serial: appSettings.connectionTypeColorSerial
    };
    return colors[connectionType] || defaultSettings.connectionTypeColorSsh;
  };
  const updateTerminalTabTypeColors = () => {
    document.querySelectorAll(".terminal-tab[data-connection-type]")
      .forEach(tab => tab.style.setProperty(
        "--terminal-tab-type-color",
        connectionTypeColor(tab.dataset.connectionType)));
  };
  const reportError = error => {
    showTransientStatus(error instanceof Error ? error.message : String(error));
  };
  const hideVisibleElement = element => {
    if (!element || element.hidden) return false;
    element.hidden = true;
    return true;
  };
  const showToast = (message, duration = 2000) => {
    const container = document.querySelector("#toast-container");
    if (!container) {
      showTransientStatus(message, duration);
      return;
    }
    const toast = document.createElement("div");
    toast.className = "toast toast-success";
    toast.setAttribute("role", "status");
    toast.setAttribute("aria-label", message);
    toast.textContent = message;
    container.appendChild(toast);
    requestAnimationFrame(() => requestAnimationFrame(() => toast.classList.add("show")));
    setTimeout(() => {
      toast.classList.remove("show");
      toast.classList.add("hide");
      setTimeout(() => toast.remove(), 220);
    }, duration);
  };
  let localCommandHistory = [];
  let localCommandHistoryPromise = null;

  // The snapshot deliberately contains only profile indexes and the selected
  // tab. Passwords, terminal buffers and entered commands are never persisted.
  const saveSessionRestore = () => {
    if (!appSettings.restoreSessions) {
      localStorage.removeItem(sessionRestoreStorageKey);
      return;
    }
    const sessionIds = Array.from(terminalTabs.querySelectorAll(".terminal-tab"))
      .map(tab => tab.dataset.sessionId);
    const profileIndexes = sessionIds.map(sessionId => sessions.get(sessionId))
      .filter(session => session && session.profileIndex >= 0
        && session.state !== "closed" && session.state !== "error")
      .map(session => session.profileIndex);
    if (!profileIndexes.length) {
      localStorage.removeItem(sessionRestoreStorageKey);
      return;
    }
    localStorage.setItem(sessionRestoreStorageKey, JSON.stringify({
      version: 1,
      profileIndexes,
      activeProfileIndex: activeSession?.profileIndex ?? -1
    }));
  };

  const restoreSavedSessions = async profiles => {
    if (!appSettings.restoreSessions || restoringSessions) return;
    let snapshot;
    try {
      snapshot = JSON.parse(localStorage.getItem(sessionRestoreStorageKey) || "null");
    } catch {
      localStorage.removeItem(sessionRestoreStorageKey);
      return;
    }
    const profileIndexes = Array.isArray(snapshot?.profileIndexes)
      ? snapshot.profileIndexes.filter(Number.isInteger) : [];
    if (!profileIndexes.length) return;
    const byIndex = new Map(profiles.map(profile => [profile.index, profile]));
    const restorable = profileIndexes.map(index => byIndex.get(index)).filter(Boolean);
    if (!restorable.length) {
      localStorage.removeItem(sessionRestoreStorageKey);
      return;
    }
    restoringSessions = true;
    showTransientStatus(`正在恢复 ${restorable.length} 个终端标签…`, 8000);
    try {
      for (const profile of restorable)
        await connectProfile(profile);
    } finally {
      restoringSessions = false;
    }
  };

  const formatUptime = seconds => {
    const value = Number(seconds);
    if (!Number.isFinite(value) || value < 0) return "—";
    const days = Math.floor(value / 86400);
    const hours = Math.floor((value % 86400) / 3600);
    const minutes = Math.floor((value % 3600) / 60);
    return `${days}天 ${hours}时 ${minutes}分`;
  };

  const formatNetworkRates = (upBytes, downBytes) => {
    const up = Math.max(0, Number(upBytes) || 0);
    const down = Math.max(0, Number(downBytes) || 0);
    const units = ["KB/s", "MB/s", "GB/s"];
    let divisor = 1024;
    let unit = 0;
    while (Math.max(up, down) / divisor >= 1024 && unit < units.length - 1) {
      divisor *= 1024;
      unit += 1;
    }
    const format = value => {
      const scaled = value / divisor;
      return scaled >= 10 ? scaled.toFixed(1) : scaled.toFixed(2);
    };
    return `↑ ${format(up)} ${units[unit]} · ↓ ${format(down)} ${units[unit]}`;
  };

  const renderServerMetricItems = (values, cpuHistory = []) => values.map(([label, value, detail]) => {
    const item = document.createElement("span");
    item.className = `server-metric${label === "CPU" ? " server-cpu-metric" : ""}${label === "网络" ? " server-network-metric" : ""}`;
    item.title = detail || `${label} ${value}`;
    const title = document.createElement("strong");
    title.textContent = label;
    item.append(title);
    if (label === "CPU" && cpuHistory?.length) {
      const current = document.createElement("span");
      current.className = "server-cpu-value";
      current.textContent = value;
      item.append(current);
      const chart = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      chart.classList.add("server-cpu-chart");
      chart.setAttribute("viewBox", "0 0 60 20");
      chart.setAttribute("preserveAspectRatio", "none");
      if (cpuHistory.length >= 2) {
        const line = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
        const last = Math.max(1, cpuHistory.length - 1);
        const offset = 30 - cpuHistory.length;
        line.setAttribute("points", cpuHistory.map((sample, index) =>
          `${(index + offset) * 60 / 29},${20 - Math.max(0, Math.min(100, sample)) * .18}`).join(" "));
        chart.appendChild(line);
      }
      item.append(chart);
    } else {
      item.append(document.createTextNode(value));
    }
    return item;
  });

  const stabilizeServerMetricsLayout = (session, values) => {
    if (!serverMetrics || !session || !values?.length) return;
    const layoutKey = `${session.sessionId}|${values.map(([label]) => label).join("|")}`;
    let columns = serverMetricLayouts.get(layoutKey);
    if (!columns) {
      // Measure outside the header's constrained grid, then assign fixed
      // tracks with a cushion. Normal value changes therefore do not resize
      // the header or make adjacent metrics jump.
      const measure = serverMetrics.cloneNode(true);
      measure.className = "server-metrics server-metrics-measuring";
      measure.hidden = false;
      measure.style.cssText = [
        "position:absolute", "left:-100000px", "top:-100000px",
        "display:flex", "width:max-content", "max-width:none",
        "min-width:0", "visibility:hidden", "justify-self:start",
        "overflow:visible"
      ].join(";");
      measure.querySelectorAll(".server-metric").forEach(item => {
        item.style.flex = "0 0 auto";
        item.style.width = "max-content";
        item.style.minWidth = "max-content";
        item.style.overflow = "visible";
      });
      document.body.appendChild(measure);
      columns = [...measure.children].map(item => {
        const reserve = item.classList.contains("server-cpu-metric") ? 24
          : item.classList.contains("server-network-metric") ? 48 : 32;
        return Math.max(64, Math.ceil(item.getBoundingClientRect().width) + reserve);
      });
      measure.remove();
      serverMetricLayouts.set(layoutKey, columns);
    }
    const total = columns.reduce((sum, width) => sum + width, 0)
      + Math.max(0, columns.length - 1) * 7;
    const available = Math.max(0, window.innerWidth - 320);
    serverMetrics.style.gridTemplateColumns = columns.map(width => `${width}px`).join(" ");
    serverMetrics.style.width = `${Math.min(860, total, available)}px`;
  };

  const renderRdpServerMetrics = session => {
    const quality = rdpQualityBySession.get(session.sessionId) || {};
    const metrics = session.metrics || {};
    const networkTypes = {
      1: "调制解调器", 2: "低速宽带", 3: "卫星",
      4: "高速宽带", 5: "WAN", 6: "LAN"
    };
    const networkType = networkTypes[Number(quality.networkConnectionType)] || "自动";
    const performanceFlags = Number(quality.performanceFlags);
    const performance = Number.isFinite(performanceFlags)
      ? (performanceFlags === 0 || performanceFlags === 384
        ? "高画质" : `自定义 (${performanceFlags})`)
      : "等待回读";
    const resolution = Number(quality.displayWidth) > 0
        && Number(quality.displayHeight) > 0
      ? `${quality.displayWidth} × ${quality.displayHeight}` : "等待协商";
    const latency = Number(metrics.latency);
    const latencyText = Number.isFinite(latency) && latency >= 0
      ? `${Math.round(latency)} ms` : "—";
    const qualityState = quality.lastHresultSucceeded === false ? "需检查" : "正常";
    const values = [
      ["延迟", latencyText, "RDP 网络往返延迟；当前控件未提供数据时显示 —"],
      ["质量", performance, `性能标志 ${Number.isFinite(performanceFlags) ? performanceFlags : "—"}`],
      ["分辨率", resolution, `当前会话分辨率 ${resolution}`],
      ["连接", networkType, `连接类型 ${networkType}；带宽检测 ${quality.bandwidthDetection ? "开启" : "关闭"}`],
      ["色深", Number(quality.colorDepth) > 0 ? `${quality.colorDepth} 位` : "等待回读"],
      ["状态", qualityState, `完整画面刷新 ${quality.fullFrameRefreshCount ?? 0} 次；${quality.lastOperation || "等待最近操作"}`]
    ];
    serverMetrics.replaceChildren(...renderServerMetricItems(values));
    stabilizeServerMetricsLayout(session, values);
    serverMetrics.hidden = false;
  };

  let rdpTabHoverTimer = null;
  let rdpTabHoverHideTimer = null;
  const hideRdpTabHover = (immediate = false) => {
    if (rdpTabHoverTimer) {
      clearTimeout(rdpTabHoverTimer);
      rdpTabHoverTimer = null;
    }
    if (rdpTabHoverHideTimer) {
      clearTimeout(rdpTabHoverHideTimer);
      rdpTabHoverHideTimer = null;
    }
    const doHide = () => {
      if (rdpTabHoverCard && !rdpTabHoverCard.hidden) {
        rdpTabHoverCard.hidden = true;
        notifyRdpLayout(false, true);
      }
    };
    if (immediate) {
      doHide();
    } else {
      rdpTabHoverHideTimer = setTimeout(doHide, 120);
    }
  };

  const showRdpTabHover = (tab, sessionId) => {
    if (rdpTabHoverHideTimer) {
      clearTimeout(rdpTabHoverHideTimer);
      rdpTabHoverHideTimer = null;
    }
    if (rdpTabHoverTimer) clearTimeout(rdpTabHoverTimer);
    if (tab) {
      tab.removeAttribute("title");
      tab.title = "";
    }
    rdpTabHoverTimer = setTimeout(() => {
      const session = sessions.get(sessionId);
      if (!session || session.connectionType !== "rdp" || !tab.isConnected || !rdpTabHoverCard) return;
      const profile = profilesByIndex.get(session.profileIndex) || {};
      const quality = rdpQualityBySession.get(sessionId) || {};
      const metrics = session.metrics || {};
      const networkTypes = {
        1: "调制解调器", 2: "低速宽带", 3: "卫星",
        4: "高速宽带", 5: "WAN", 6: "LAN"
      };
      const networkType = networkTypes[Number(quality.networkConnectionType)] || "自动";
      const performanceFlags = Number(quality.performanceFlags);
      const performance = Number.isFinite(performanceFlags)
        ? (performanceFlags === 0 || performanceFlags === 384
          ? "高画质" : `自定义 (${performanceFlags})`)
        : "等待回读";
      const resolution = Number(quality.displayWidth) > 0
          && Number(quality.displayHeight) > 0
        ? `${quality.displayWidth} × ${quality.displayHeight}` : "等待协商";
      const latency = Number(metrics.latency);
      const latencyText = Number.isFinite(latency) && latency >= 0
        ? `${Math.round(latency)} ms` : "—";
      const colorDepth = Number(quality.colorDepth) > 0 ? `${quality.colorDepth} 位` : "32 位";
      const remoteTarget = profile.host ? `${profile.host}${profile.port ? `:${profile.port}` : ""}` : (session.name || "远程桌面");

      rdpTabHoverCard.replaceChildren();

      const header = document.createElement("div");
      header.className = "rdp-hover-header";
      const icon = document.createElement("span");
      icon.className = "rdp-hover-icon";
      icon.textContent = "🖥️";
      const title = document.createElement("strong");
      title.className = "rdp-hover-title";
      title.textContent = session.displayName || session.name || "远程桌面";
      const target = document.createElement("span");
      target.className = "rdp-hover-target";
      target.textContent = remoteTarget;
      header.append(icon, title, target);

      const grid = document.createElement("div");
      grid.className = "rdp-hover-grid";
      const items = [
        ["⚡ 延迟", latencyText],
        ["🎨 画质", performance],
        ["📐 分辨率", resolution],
        ["🌐 连接", networkType],
        ["🔒 色深", colorDepth],
        ["🔄 刷新", `${quality.fullFrameRefreshCount ?? 0} 次`]
      ];
      items.forEach(([label, val]) => {
        const item = document.createElement("div");
        item.className = "rdp-hover-item";
        const lSpan = document.createElement("span");
        lSpan.className = "rdp-hover-label";
        lSpan.textContent = label;
        const vSpan = document.createElement("span");
        vSpan.className = "rdp-hover-value";
        vSpan.textContent = val;
        item.append(lSpan, vSpan);
        grid.append(item);
      });
      rdpTabHoverCard.append(header, grid);

      rdpTabHoverCard.hidden = false;
      const tabRect = tab.getBoundingClientRect();
      let left = tabRect.left;
      let top = tabRect.bottom + 6;
      const cardWidth = 300;
      if (left + cardWidth > window.innerWidth - 12) {
        left = Math.max(12, window.innerWidth - cardWidth - 12);
      }
      rdpTabHoverCard.style.left = `${Math.round(left)}px`;
      rdpTabHoverCard.style.top = `${Math.round(top)}px`;
      notifyRdpLayout(false, false);
    }, 120);
  };

  if (rdpTabHoverCard) {
    rdpTabHoverCard.addEventListener("mouseenter", () => {
      if (rdpTabHoverHideTimer) {
        clearTimeout(rdpTabHoverHideTimer);
        rdpTabHoverHideTimer = null;
      }
    });
    rdpTabHoverCard.addEventListener("mouseleave", () => {
      hideRdpTabHover(false);
    });
  }

  const renderServerMetrics = () => {
    const session = sessions.get(focusedSessionId) || activeSession;
    if (!session || session.state !== "connected") {
      if (terminalStatusBar) terminalStatusBar.hidden = true;
      serverMetrics.hidden = true;
      serverMetrics.replaceChildren();
      return;
    }
    if (session.connectionType === "rdp") {
      if (terminalStatusBar) terminalStatusBar.hidden = true;
      renderRdpServerMetrics(session);
      return;
    }
    const prevStatusBarHidden = terminalStatusBar ? terminalStatusBar.hidden : true;
    if (terminalStatusBar) {
      terminalStatusBar.hidden = false;
      if (terminalStatusDot) {
        terminalStatusDot.style.setProperty("--terminal-tab-type-color",
          connectionTypeColor(session.connectionType));
      }
      if (terminalStatusTitle) {
        terminalStatusTitle.textContent = session.displayName || session.name || "终端";
      }
    }
    if (prevStatusBarHidden && session.scheduleRefit) {
      session.scheduleRefit();
    }
    const metrics = session.metrics;
    if (session.connectionType !== "ssh" || !metrics || metrics.state === "waiting") {
      const typeLabel = session.connectionType === "serial" ? "串口"
        : (session.shellType ? `本地 (${session.shellType})` : "本地终端");
      const cols = session.terminal?.cols || 80;
      const rows = session.terminal?.rows || 24;
      const values = [
        ["会话", typeLabel],
        ["尺寸", `${cols}×${rows}`],
        ["编码", "UTF-8"]
      ];
      if (session.connectionType === "ssh") {
        values.unshift(["状态", "正在获取监控…"]);
      }
      serverMetrics.replaceChildren(...renderServerMetricItems(values));
      stabilizeServerMetricsLayout(session, values);
      serverMetrics.hidden = false;
      return;
    }
    if (metrics.state === "unavailable") {
      const cols = session.terminal?.cols || 80;
      const rows = session.terminal?.rows || 24;
      const values = [
        ["监控", "不可用"],
        ["尺寸", `${cols}×${rows}`],
        ["编码", "UTF-8"]
      ];
      serverMetrics.replaceChildren(...renderServerMetricItems(values));
      stabilizeServerMetricsLayout(session, values);
      serverMetrics.hidden = false;
      return;
    }
    if (appSettings.alertsEnabled) {
      if (!session.alertedMetrics) session.alertedMetrics = new Set();
      const check = (key, percent, threshold, label) => {
        const over = Number.isFinite(percent) && percent >= threshold;
        if (over && !session.alertedMetrics.has(key)) {
          session.alertedMetrics.add(key);
          notify("服务器告警", `${session.name}：${label}已达 ${Math.round(percent)}%`);
        } else if (!over) {
          session.alertedMetrics.delete(key);
        }
      };
      check("cpu", metrics.cpuPercent, appSettings.alertCpuThreshold, "CPU 使用率");
      let memoryPercent = NaN;
      if (Number.isFinite(metrics.memoryTotal) && metrics.memoryTotal > 0) {
        const used = Math.max(0, metrics.memoryTotal
          - Math.min(metrics.memoryAvailable, metrics.memoryTotal));
        memoryPercent = used * 100 / metrics.memoryTotal;
      }
      check("memory", memoryPercent, appSettings.alertMemoryThreshold, "内存使用率");
      let diskPercent = NaN;
      if (Number.isFinite(metrics.diskTotal) && metrics.diskTotal > 0)
        diskPercent = Math.max(0, metrics.diskUsed) * 100 / metrics.diskTotal;
      check("disk", diskPercent, appSettings.alertDiskThreshold, "磁盘使用率");
    }
    const values = [];
    if (Number.isFinite(metrics.cpuPercent)) values.push(["CPU", `${Math.round(metrics.cpuPercent)}%`]);
    if (Number.isFinite(metrics.memoryTotal) && metrics.memoryTotal > 0) {
      const used = Math.max(0, metrics.memoryTotal - Math.min(metrics.memoryAvailable, metrics.memoryTotal));
      const toGb = bytes => `${(bytes / (1024 ** 3)).toFixed(2)} GB`;
      values.push(["内存", `${Math.round(used * 100 / metrics.memoryTotal)}% · ${toGb(used)}/${toGb(metrics.memoryTotal)}`]);
    }
    if (metrics.network) values.push(["网络", metrics.network]);
    if (Number.isFinite(metrics.diskTotal) && metrics.diskTotal > 0)
      values.push(["磁盘", `${Math.round(Math.max(0, metrics.diskUsed) * 100 / metrics.diskTotal)}%`,
        (metrics.diskPartitions || []).map(partition => {
          const used = Number(partition.used);
          const total = Number(partition.total);
          const percent = total > 0 ? Math.round(Math.max(0, used) * 100 / total) : 0;
          return `${partition.mount || "?"} ${percent}% · ${formatSize(used)}/${formatSize(total)}`;
        }).join("\n")]);
    if (Number.isFinite(metrics.uptime) && metrics.uptime >= 0)
      values.push(["运行", formatUptime(metrics.uptime)]);
    if (Number.isFinite(metrics.latency)) {
      if (metrics.latency >= 0) {
        if (metrics.isProxyJump) {
          const jumpLabel = metrics.proxyHost ? `跳板机网关 (${metrics.proxyHost})` : "跳板机网关";
          values.push(["延迟", `${Math.round(metrics.latency)} ms (跳板)`, `网络延迟：通过 ${jumpLabel} 测得往返延迟 ${Math.round(metrics.latency)} ms`]);
        } else {
          values.push(["延迟", `${Math.round(metrics.latency)} ms`, `目标服务器网络往返延迟 ${Math.round(metrics.latency)} ms`]);
        }
      } else {
        values.push(["延迟", "—", metrics.isProxyJump ? "跳板机网络延迟探测超时或不可达" : "网络延迟探测超时或不可达"]);
      }
    }
    serverMetrics.replaceChildren(...renderServerMetricItems(values, metrics.cpuHistory));
    stabilizeServerMetricsLayout(session, values);
    serverMetrics.hidden = values.length === 0;
  };

  let actionDialogResolve = null;
  const finishActionDialog = action => {
    if (!actionDialogResolve) return;
    const resolve = actionDialogResolve;
    actionDialogResolve = null;
    const value = actionDialogSelect.hidden ? actionDialogInput.value : actionDialogSelect.value;
    actionDialog.close();
    resolve({ action, value });
  };

  const showActionDialog = ({ title, message = "", value = null, buttons, monospace = false }) => {
    if (actionDialogResolve)
      finishActionDialog("cancel");
    actionDialogTitle.textContent = title;
    actionDialogMessage.textContent = message;
    actionDialogMessage.style.fontFamily = monospace
      ? '"Cascadia Mono", Consolas, monospace' : "";
    actionDialogInput.hidden = value === null;
    actionDialogSelect.hidden = true;
    actionDialogInput.value = value === null ? "" : value;
    actionDialogActions.replaceChildren();
    let primaryElement = null;
    buttons.forEach(button => {
      const element = document.createElement("button");
      element.type = "button";
      element.textContent = button.label;
      element.className = button.primary ? "primary" : "";
      if (button.primary)
        primaryElement = element;
      element.addEventListener("click", () => finishActionDialog(button.action));
      actionDialogActions.appendChild(element);
    });
    actionDialog.showModal();
    requestAnimationFrame(() => {
      if (value !== null) {
        actionDialogInput.focus();
        actionDialogInput.select();
      } else {
        primaryElement?.focus();
      }
    });
    return new Promise(resolve => { actionDialogResolve = resolve; });
  };

  const requestChoice = async (title, message, options, value = "") => {
    actionDialogTitle.textContent = title;
    actionDialogMessage.textContent = message;
    actionDialogInput.hidden = true;
    actionDialogSelect.hidden = false;
    actionDialogSelect.replaceChildren();
    options.forEach(option => {
      const element = document.createElement("option");
      element.value = option.value;
      element.textContent = option.label;
      actionDialogSelect.appendChild(element);
    });
    if (options.some(option => option.value === value))
      actionDialogSelect.value = value;
    actionDialogActions.replaceChildren();
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "取消";
    cancel.addEventListener("click", () => finishActionDialog("cancel"));
    const confirm = document.createElement("button");
    confirm.type = "button";
    confirm.textContent = "确定";
    confirm.className = "primary";
    confirm.addEventListener("click", () => finishActionDialog("confirm"));
    actionDialogActions.append(cancel, confirm);
    actionDialog.showModal();
    requestAnimationFrame(() => actionDialogSelect.focus());
    const result = await new Promise(resolve => { actionDialogResolve = resolve; });
    return result.action === "confirm" ? result.value : null;
  };

  const requestText = async (title, message, value = "") => {
    const result = await showActionDialog({
      title, message, value,
      buttons: [
        { action: "cancel", label: "取消" },
        { action: "confirm", label: "确定", primary: true }
      ]
    });
    return result.action === "confirm" ? result.value : null;
  };

  const requestConfirmation = async (title, message, confirmLabel = "确定") => {
    const result = await showActionDialog({
      title, message, value: null,
      buttons: [
        { action: "cancel", label: "取消" },
        { action: "confirm", label: confirmLabel, primary: true }
      ]
    });
    return result.action === "confirm";
  };

  const manageKnownHosts = async () => {
    let entries;
    try {
      entries = await post("knownHosts.list");
    } catch (error) {
      reportError(error);
      return;
    }
    while (entries.length) {
      const options = entries.map(entry => ({
        value: String(entry.index),
        label: `${entry.host || "未知主机"} · ${entry.keyType || "未知类型"} · ${(entry.key || "").slice(0, 18)}…`
      }));
      const selected = await requestChoice(
        "已知主机指纹", "选择要管理的主机指纹：", options);
      if (selected === null) return;
      const entry = entries.find(item => String(item.index) === selected);
      if (!entry) continue;
      const action = await showActionDialog({
        title: "主机指纹操作",
        message: `“${entry.host || entry.line}”\n\n重新确认会移除当前记录；下次连接将校验并保存服务器当前指纹。`,
        buttons: [
          { action: "cancel", label: "取消" },
          { action: "remove", label: "删除" },
          { action: "reconfirm", label: "重新确认", primary: true }
        ]
      });
      if (action.action === "cancel") continue;
      try {
        await post("knownHosts.remove", { index: Number(selected) });
        entries = await post("knownHosts.list");
        if (action.action === "reconfirm")
          await showActionDialog({
            title: "等待重新确认",
            message: "旧指纹已移除。请重新连接该服务器，MasterTerm 会保存服务器当前的主机指纹。",
            buttons: [{ action: "close", label: "关闭", primary: true }]
          });
      } catch (error) {
        reportError(error);
        return;
      }
    }
    await showActionDialog({
      title: "已知主机指纹", message: "当前没有已保存的主机指纹。",
      buttons: [{ action: "close", label: "关闭", primary: true }]
    });
  };

  const loadScript = source => new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = source;
    script.onload = resolve;
    script.onerror = () => reject(new Error(`无法加载 ${source}`));
    document.head.appendChild(script);
  });

  const ensureTerminalLibraries = () => {
    if (typeof Terminal !== "undefined" && typeof FitAddon !== "undefined")
      return Promise.resolve();
    if (!terminalLibrariesPromise) {
      terminalLibrariesPromise = loadScript("vendor/xterm/xterm.js")
        .then(() => loadScript("vendor/xterm-fit/addon-fit.js"))
        .then(() => {
          if (typeof Terminal === "undefined" || typeof FitAddon === "undefined")
            throw new Error("终端组件加载失败");
        })
        .catch(error => {
          terminalLibrariesPromise = null;
          throw error;
        });
    }
    return terminalLibrariesPromise;
  };

  const updateServerFormType = () => {
    const serial = serverType.value === "serial";
    const rdp = serverType.value === "rdp";
    const passwordLabel = document.querySelector("#server-password").closest("label");
    const keyPathLabel = document.querySelector("#server-key-path").closest("label");
    const keyPassphraseLabel = document.querySelector("#server-key-passphrase").closest("label");
    const jumpSection = document.querySelector("#jump-server-fields");
    document.querySelector("#ssh-fields").hidden = serial;
    if (rdp) {
      // Show only the password among the SSH fields, positioned right after
      // the port so the form reads: 用户名 / 地址 / 端口 / 密码 / …
      passwordLabel.hidden = false;
      keyPathLabel.hidden = true;
      keyPassphraseLabel.hidden = true;
      if (jumpSection) jumpSection.hidden = true;
      document.querySelector("#server-port").closest("label").after(passwordLabel);
    } else {
      passwordLabel.hidden = false;
      keyPathLabel.hidden = false;
      keyPassphraseLabel.hidden = false;
      if (jumpSection) jumpSection.hidden = false;
      document.querySelector("#ssh-fields").prepend(passwordLabel);
    }
    document.querySelector("#serial-fields").hidden = !serial;
    rdpAdvancedSettings.hidden = !rdp;
    rdpResetDefaultsButton.hidden = !rdp;
    document.querySelector("#server-user-field").hidden = serial;
    document.querySelector("#server-address-field").hidden = serial;
    document.querySelector("#server-serial-port-field").hidden = !serial;
    document.querySelector("#server-user").required = !serial;
    document.querySelector("#server-address").required = !serial;
    serialPortSelect.required = serial;
    document.querySelector("#server-port-label").textContent = serial ? "波特率" : (rdp ? "远程桌面端口" : "端口");
    const port = document.querySelector("#server-port");
    port.max = serial ? "4000000" : "65535";
    // Update the port when switching types while the field still holds the
    // previous type's default (22 / 115200).
    const current = port.value;
    if (current === "" || current === "22" || current === "115200")
      port.value = serial ? "115200" : (rdp ? "3389" : "22");
    // RDP advanced options are part of the normal editor. Keep the dialog in
    // its fixed-height, scrollable layout as soon as the connection type is
    // RDP; there is no second click-driven geometry transition.
    serverDialog.classList.toggle("rdp-advanced-open", rdp);
  };

  const populateWorkspaceSelect = selected => {
    const select = document.querySelector("#server-workspace");
    select.replaceChildren();
    workspaceNames.forEach(name => {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      select.appendChild(option);
    });
    select.value = workspaceNames.includes(selected) ? selected : "未分配";
  };

  const renderWorkspaceFilter = () => {
    workspaceFilterSelect.replaceChildren();
    const counts = new Map();
    let totalCount = 0;
    (profilesCache || []).forEach(p => {
      const ws = p.workspace || "未分配";
      counts.set(ws, (counts.get(ws) || 0) + 1);
      totalCount++;
    });

    ["全部连接", ...workspaceNames].forEach(name => {
      const option = document.createElement("option");
      option.value = name;
      const count = name === "全部连接" ? totalCount : (counts.get(name) || 0);
      option.textContent = `${name} (${count})`;
      workspaceFilterSelect.appendChild(option);
    });
    workspaceFilterSelect.value = activeWorkspace;
  };

  const refreshWorkspaces = async () => {
    workspaceNames = await post("workspace.list");
    if (!workspaceNames.includes("未分配"))
      workspaceNames.unshift("未分配");
    if (activeWorkspace !== "全部连接" && !workspaceNames.includes(activeWorkspace))
      activeWorkspace = "全部连接";
    renderWorkspaceFilter();
  };

  const splitSshEndpoint = endpoint => {
    const value = String(endpoint ?? "").trim();
    const separator = value.lastIndexOf("@");
    return separator > 0
      ? { user: value.slice(0, separator), address: value.slice(separator + 1) }
      : { user: "", address: value };
  };
  const joinSshEndpoint = (user, address) => {
    const cleanUser = String(user ?? "").trim();
    const cleanAddress = String(address ?? "").trim();
    return cleanUser ? `${cleanUser}@${cleanAddress}` : cleanAddress;
  };
  const splitProxyJump = endpoint => {
    const value = String(endpoint ?? "").trim();
    if (!value) return { user: "", host: "", port: 22 };
    let user = "";
    let remainder = value;
    const at = value.lastIndexOf("@");
    if (at > 0) {
      user = value.slice(0, at).trim();
      remainder = value.slice(at + 1).trim();
    }
    let host = remainder;
    let port = 22;
    if (remainder.startsWith("[") && remainder.includes("]")) {
      const closeBracket = remainder.indexOf("]");
      host = remainder.slice(1, closeBracket).trim();
      const afterBracket = remainder.slice(closeBracket + 1);
      if (afterBracket.startsWith(":")) {
        const parsed = parseInt(afterBracket.slice(1), 10);
        if (parsed > 0 && parsed <= 65535) port = parsed;
      }
    } else {
      const colon = remainder.lastIndexOf(":");
      if (colon > 0) {
        const parsed = parseInt(remainder.slice(colon + 1), 10);
        if (parsed > 0 && parsed <= 65535) {
          port = parsed;
          host = remainder.slice(0, colon).trim();
        }
      }
    }
    return { user, host, port };
  };
  const formatProxyJump = (user, host, port) => {
    let cleanHost = String(host ?? "").trim();
    if (!cleanHost) return "";
    const cleanUser = String(user ?? "").trim();
    const portNum = Number(port);
    const validPort = Number.isInteger(portNum) && portNum > 0 && portNum <= 65535 ? portNum : 22;
    if (cleanHost.includes(":") && !cleanHost.startsWith("[")) {
      cleanHost = `[${cleanHost}]`;
    }
    const hostWithPort = validPort !== 22 ? `${cleanHost}:${validPort}` : cleanHost;
    return cleanUser ? `${cleanUser}@${hostWithPort}` : hostWithPort;
  };

  const updateServerTagMode = () => {
    const mode = document.querySelector("#server-tag-mode").value;
    const input = document.querySelector("#server-tag");
    const field = document.querySelector("#server-custom-tag-field");
    const type = serverType.value;
    const defaultTag = type === "serial" ? "串口" : type === "rdp" ? "RDP" : "SSH";
    input.disabled = mode !== "custom";
    if (mode !== "custom") input.value = defaultTag;
    field?.classList.toggle("tag-auto-mode", mode !== "custom");
  };
  const openServerEditor = (profile = null, requestedType = "") => {
    const serial = profile?.connectionType === "serial" || requestedType === "serial";
    const rdp = profile?.connectionType === "rdp" || requestedType === "rdp";
    const endpoint = splitSshEndpoint(profile?.address);
    const proxyEndpoint = splitProxyJump(profile?.proxyJump);
    document.querySelector("#server-dialog-title").textContent =
      profile ? "编辑连接" : "新建连接";
    document.querySelector("#server-index").value = profile?.index ?? -1;
    serverType.value = serial ? "serial" : rdp ? "rdp" : "ssh";
    document.querySelector("#server-name").value = profile?.name ?? "";
    document.querySelector("#server-user").value = serial ? "" : endpoint.user;
    document.querySelector("#server-address").value = serial ? "" : endpoint.address;
    document.querySelector("#server-port").value = profile?.port
      ?? (serial ? 115200 : rdp ? 3389 : 22);
    document.querySelector("#server-tag").value = profile?.tag || "";
    document.querySelector("#server-tag-mode").value = profile?.tagMode === "custom"
      ? "custom" : "auto";
    populateWorkspaceSelect(profile?.workspace || "未分配");
    document.querySelector("#server-color").value = profile?.color || "#8ab4f8";
    document.querySelector("#server-password").value = "";
    document.querySelector("#server-key-path").value = profile?.keyPath ?? "";
    document.querySelector("#server-key-passphrase").value = "";
    document.querySelector("#server-proxy-user").value = proxyEndpoint.user;
    document.querySelector("#server-proxy-host").value = proxyEndpoint.host;
    document.querySelector("#server-proxy-port").value = proxyEndpoint.port;
    document.querySelector("#server-proxy-password").value = "";
    document.querySelector("#server-proxy-key-path").value = profile?.proxyKeyPath ?? "";
    const proxyEnabledCheckbox = document.querySelector("#server-proxy-enabled");
    const hasProxy = Boolean(profile?.proxyJump);
    const isProxyEnabled = profile ? (profile.proxyJumpEnabled !== false && hasProxy) : false;
    if (proxyEnabledCheckbox) {
      proxyEnabledCheckbox.checked = isProxyEnabled;
    }
    document.querySelector("#jump-server-body")?.classList.toggle("disabled", !isProxyEnabled);
    document.querySelector("#server-data-bits").value = String(profile?.serialDataBits ?? 8);
    document.querySelector("#server-parity").value = profile?.serialParity ?? "none";
    document.querySelector("#server-stop-bits").value = profile?.serialStopBits ?? "1";
    document.querySelector("#server-flow-control").value = profile?.serialFlowControl ?? "none";
    const rdpOptions = profile?.rdpOptions || {};
    if (rdpQualityPresetSelect) {
      rdpQualityPresetSelect.value = profile?.rdpQualityPreset || "balanced";
    }
    Object.entries(rdpOptionFields).forEach(([key, field]) => {
      if (!field) return;
      const value = rdpOptions[key];
      if (rdpCheckboxKeys.has(key)) {
        field.checked = value == null || Number(value) !== 0;
      } else {
        field.value = String(value ?? defaultRdpOptionValue(key));
      }
    });
    serverFormStatus.textContent = "";
    updateServerFormType();
    updateServerTagMode();
    // Never put saved secrets into the input value.  Show a neutral bullet
    // placeholder for every credential that is already stored, including the
    // key passphrase and ProxyJump password.
    const passwordInput = document.querySelector("#server-password");
    const keyPassphraseInput = document.querySelector("#server-key-passphrase");
    const proxyPasswordInput = document.querySelector("#server-proxy-password");
    const credentialFields = [
      [passwordInput, "留空则保持现有密码"],
      [keyPassphraseInput, "私钥加密时填写，留空则保持现有口令"],
      [proxyPasswordInput, "留空则保持现有密码"]
    ];
    credentialFields.forEach(([input, fallback]) => {
      input.placeholder = fallback;
    });
    const savedPlaceholder = (length, fallback = "•••") => {
      const count = Number(length);
      return Number.isFinite(count) && count > 0
        ? "•".repeat(Math.floor(count)) : fallback;
    };
    const applyStoredCredentialPlaceholders = result => {
      if (!result)
        return;
      const passwordLength = Number(result.passwordLength);
      const keyPassphraseLength = Number(result.keyPassphraseLength);
      const proxyPasswordLength = Number(result.proxyPasswordLength);
      if (result.hasPassword || passwordLength > 0)
        passwordInput.placeholder = savedPlaceholder(passwordLength);
      if (result.hasKeyPassphrase || keyPassphraseLength > 0)
        keyPassphraseInput.placeholder =
          savedPlaceholder(keyPassphraseLength);
      if (result.hasProxyPassword || proxyPasswordLength > 0)
        proxyPasswordInput.placeholder =
          savedPlaceholder(proxyPasswordLength);
    };
    // server.list already carries secret lengths.  Apply them synchronously
    // first, then refresh from the credential store for legacy/profile changes.
    applyStoredCredentialPlaceholders(profile);
    if (profile && profile.index >= 0) {
      post("server.hasPassword", { index: profile.index })
        .then(applyStoredCredentialPlaceholders)
        .catch(() => {});
    }
    if (serial)
      refreshSerialPorts(profile?.address ?? "");
    serverDialog.showModal();
    document.querySelector("#server-name").focus();
  };

  const refreshProfiles = () => post("server.list").then(render);

  const profileFormPayload = () => ({
    index: Number(document.querySelector("#server-index").value),
    connectionType: serverType.value,
    name: document.querySelector("#server-name").value,
    address: serverType.value === "serial"
      ? serialPortSelect.value
      : joinSshEndpoint(document.querySelector("#server-user").value,
          document.querySelector("#server-address").value),
    port: Number(document.querySelector("#server-port").value),
    tag: document.querySelector("#server-tag").value,
    tagMode: document.querySelector("#server-tag-mode").value,
    workspace: document.querySelector("#server-workspace").value,
    color: document.querySelector("#server-color").value,
    password: document.querySelector("#server-password").value,
    keyPath: document.querySelector("#server-key-path").value,
    keyPassphrase: document.querySelector("#server-key-passphrase").value,
    proxyJumpEnabled: document.querySelector("#server-proxy-enabled")?.checked ?? false,
    proxyJump: formatProxyJump(
      document.querySelector("#server-proxy-user").value,
      document.querySelector("#server-proxy-host").value,
      document.querySelector("#server-proxy-port").value),
    proxyPassword: document.querySelector("#server-proxy-password").value,
    proxyKeyPath: document.querySelector("#server-proxy-key-path").value,
    serialDataBits: Number(document.querySelector("#server-data-bits").value),
    serialParity: document.querySelector("#server-parity").value,
    serialStopBits: document.querySelector("#server-stop-bits").value,
    serialFlowControl: document.querySelector("#server-flow-control").value,
    ...(serverType.value === "rdp" ? {
      rdpQualityPreset: rdpQualityPresetSelect ? rdpQualityPresetSelect.value : "balanced",
      rdpOptions: Object.fromEntries(Object.entries(rdpOptionFields).map(
        ([key, field]) => [key, rdpCheckboxKeys.has(key)
          ? (field.checked ? 1 : 0) : Number(field.value)]))
    } : {})
  });

  const connectProfile = (profile, options = {}) => {
    rememberRecentConnection(profile);
    if (profile.connectionType === "rdp") {
      return ensureTerminalLibraries().then(() => {
        const sessionId = `rdp-${profile.index}-${Date.now()}`;
        const session = openTerminal(
          sessionId, profile.name || profile.address, "rdp", profile.index, options);
        if (session?.state === "connecting" && activeSession === session) {
          rdpConnectingStatusSessionId = sessionId;
          setHeaderStatus("正在连接远程桌面…");
        }
        return post("session.rdpOpen", {
          index: profile.index, sessionId
        });
      }).catch(reportError);
    }
    setHeaderStatus(`正在连接 ${profile.name || profile.address}…`);
    return ensureTerminalLibraries()
      .then(() => post("session.connect", {
        index: profile.index,
        keepalive: appSettings.keepalive
      }))
      .catch(reportError);
  };

  const deleteProfile = async profile => {
    if (!await requestDeleteConfirmation(
        "删除连接", `确定删除“${profile.name || profile.address}”吗？`, "删除"))
      return;
    post("server.delete", { index: profile.index })
      .then(refreshProfiles)
      .catch(reportError);
  };

  const selectedProfiles = () => profilesCache.filter(profile =>
    selectedProfileIndexes.has(profile.index));

  const batchUpdateProfiles = async (changes) => {
    const profiles = selectedProfiles();
    if (!profiles.length) return;
    for (const profile of profiles)
      await post("server.update", { index: profile.index, ...changes });
    await refreshProfiles();
  };

  const batchDeleteProfiles = async () => {
    const profiles = selectedProfiles();
    if (!profiles.length) return;
    if (!await requestDeleteConfirmation(
        "批量删除连接", `确定删除选中的 ${profiles.length} 个连接吗？`, "删除"))
      return;
    for (const profile of [...profiles].sort((a, b) => b.index - a.index))
      await post("server.delete", { index: profile.index });
    selectedProfileIndexes.clear();
    await refreshProfiles();
  };

  const batchWorkspace = async () => {
    const workspace = await requestChoice(
      "批量设置工作区", "请选择已有工作区：",
      workspaceNames.map(name => ({ value: name, label: name })),
      "未分配");
    if (workspace !== null)
      await batchUpdateProfiles({ workspace });
  };

  const batchColor = async () => {
    const color = await requestChoice(
      "批量设置颜色", "请选择连接卡片颜色：",
      [{ value: "#8ab4f8", label: "蓝色" }, { value: "#34d399", label: "绿色" },
       { value: "#fbbf24", label: "黄色" }, { value: "#f472b6", label: "粉色" },
       { value: "#a78bfa", label: "紫色" }], "#8ab4f8");
    if (color !== null)
      await batchUpdateProfiles({ color });
  };

  const batchIcon = async () => {
    const icon = await requestChoice(
      "批量设置图标", "请选择连接卡片图标：",
      [{ value: "server", label: "服务器" }, { value: "terminal", label: "终端" },
       { value: "cloud", label: "云端" }, { value: "database", label: "数据库" },
       { value: "serial", label: "串口" }], "server");
    if (icon !== null)
      await batchUpdateProfiles({ icon });
  };

  const closeServerContextMenu = () => {
    const wasVisible = hideVisibleElement(serverContextMenu);
    contextMenuProfile = null;
    if (wasVisible) clearRdpOverlay();
  };

  const openServerContextMenu = (event, profile) => {
    event.preventDefault();
    event.stopPropagation();
    contextMenuProfile = profile;
    if (!selectedProfileIndexes.has(profile.index)) {
      selectedProfileIndexes.clear();
      selectedProfileIndexes.add(profile.index);
    }
    document.querySelectorAll(".server").forEach(element => {
      const index = Number(element.dataset.profileIndex);
      element.classList.toggle("selected", selectedProfileIndexes.has(index));
    });
    const display = serverDisplayGroups(profilesCache);
    const group = display.groups.find(candidate =>
      candidate.profiles.some(item => item.index === profile.index));
    const visible = group?.profiles || displayedProfileOrder;
    const position = visible.findIndex(item => item.index === profile.index);
    const moveUp = serverContextMenu.querySelector('[data-action="move-up"]');
    const moveDown = serverContextMenu.querySelector('[data-action="move-down"]');
    const canMove = position >= 0;
    if (moveUp) moveUp.disabled = !canMove || position <= 0;
    if (moveDown) moveDown.disabled = !canMove || position < 0 || position >= visible.length - 1;
    if (activeRdpVisible()) {
      // CSS z-index cannot place this WebView2 menu above the native RDP
      // child. Reuse the same Win32 popup used by the RDP tab menu so its
      // border and system shadow are painted in the top-level menu layer.
      serverContextMenu.hidden = true;
      post("app.serverContextMenu", {
        index: profile.index,
        x: Math.round(event.clientX),
        y: Math.round(event.clientY),
        canMoveUp: Boolean(canMove && position > 0),
        canMoveDown: Boolean(
          canMove && position >= 0 && position < visible.length - 1)
      }).catch(reportError);
      return;
    }
    serverContextMenu.hidden = false;
    serverContextMenu.style.left = `${event.clientX}px`;
    serverContextMenu.style.top = `${event.clientY}px`;
    const bounds = serverContextMenu.getBoundingClientRect();
    const left = Math.max(6, Math.min(event.clientX, window.innerWidth - bounds.width - 6));
    const top = Math.max(6, Math.min(event.clientY, window.innerHeight - bounds.height - 6));
    serverContextMenu.style.left = `${left}px`;
    serverContextMenu.style.top = `${top}px`;
  };

  window.addEventListener("error", event => {
    showTransientStatus(`前端错误：${event.message}`);
  });

  const post = (method, params = {}) => new Promise((resolve, reject) => {
    const id = String(++sequence);
    pending.set(id, { resolve, reject });
    if (window.chrome?.webview) {
      window.chrome.webview.postMessage({ id, method, params });
    } else {
      pending.delete(id);
      reject(new Error("WebView2 bridge unavailable"));
    }
  });

  const postWithTimeout = (method, params = {}, timeoutMs = 15000) => {
    return Promise.race([
      post(method, params),
      new Promise((_, reject) => setTimeout(() => reject(new Error("请求超时（超过 15 秒），请检查网络连接")), timeoutMs))
    ]);
  };
  const syncRdpFullscreenMenu = () => {
    if (!rdpFullscreenMenuElement) return;
    const barHidden = !rdpFullscreenBar || rdpFullscreenBar.hidden;
    const open = Boolean(rdpFullscreenMenu) && rdpFullscreen && !barHidden;
    rdpFullscreenMenuElement.hidden = !open;
    rdpFullscreenMenuElement.dataset.menu = rdpFullscreenMenu;
    rdpFullscreenMenuElement.querySelectorAll("[data-rdp-menu]")
      .forEach(button => {
        button.hidden = button.dataset.rdpMenu !== rdpFullscreenMenu;
      });
    if (!open) return;
    const trigger = rdpFullscreenMenu === "display"
      ? rdpFullscreenDisplay
      : rdpFullscreenMenu === "connection"
        ? rdpFullscreenConnection : rdpFullscreenKeyboard;
    const barBounds = rdpFullscreenBar.getBoundingClientRect();
    const triggerBounds = trigger?.getBoundingClientRect();
    if (!triggerBounds) return;
      const menuWidth = 150;
    const left = Math.max(
      8, Math.min(barBounds.width - menuWidth - 8,
        triggerBounds.left - barBounds.left
          + triggerBounds.width / 2 - menuWidth / 2));
    rdpFullscreenMenuElement.style.left = `${left}px`;
  };
  const syncRdpFullscreenUi = () => {
    document.documentElement.classList.toggle("rdp-fullscreen", rdpFullscreen);
    if (!rdpFullscreen)
      hideRdpQualityPanel();
    if (rdpFullscreenHotZone)
      rdpFullscreenHotZone.hidden = !rdpFullscreen || rdpFullscreenNativeBar;
    if (rdpFullscreenBar)
      rdpFullscreenBar.hidden = rdpFullscreenNativeBar || !(rdpFullscreen
        && (rdpFullscreenBarPinned || rdpFullscreenBarVisible));
    if (rdpFullscreenPin) {
      rdpFullscreenPin.textContent = rdpFullscreenBarPinned ? "📌" : "○";
      rdpFullscreenPin.title = rdpFullscreenBarPinned
        ? "取消固定后，控制栏将在离开顶部后自动隐藏"
        : "固定显示控制栏";
      rdpFullscreenPin.setAttribute(
        "aria-pressed", String(rdpFullscreenBarPinned));
    }
    syncRdpFullscreenMenu();
  };
  const activeRdpSessionId = () =>
    activeSession?.connectionType === "rdp"
      && activeSession.state !== "closed" && activeSession.state !== "error"
      ? activeSession.sessionId : "";
  const activeRdpVisible = () => Boolean(activeRdpSessionId());
  const formatRdpHresult = value => {
    const number = Number(value);
    return Number.isFinite(number)
      ? `0x${(number >>> 0).toString(16).padStart(8, "0")}` : "未知";
  };
  const renderRdpQualityPanel = sessionId => {
    const quality = rdpQualityBySession.get(String(sessionId));
    if (!rdpQualityContent) return;
    if (!quality) {
      rdpQualityContent.textContent = "等待连接协商…";
      return;
    }
    const boolText = value => value ? "开启" : "关闭";
    const resolution = quality.displayWidth && quality.displayHeight
      ? `${quality.displayWidth} × ${quality.displayHeight}` : "等待连接";
    const lastResult = `${formatRdpHresult(quality.lastHresult)}${
      quality.lastHresultSucceeded ? "（成功）" : "（失败）"}`;
    rdpQualityContent.textContent = [
      `色深             ${quality.colorDepth || "等待回读"} 位`,
      `分辨率           ${resolution}`,
      `SmartSizing      ${boolText(Boolean(quality.smartSizing))}`,
      `性能标志         ${quality.performanceFlags ?? "等待回读"}`,
      `连接类型         ${quality.networkConnectionType || "自动"}`,
      `带宽检测         ${boolText(Boolean(quality.bandwidthDetection))}`,
      `协议模式         ${quality.clientProtocolSpec ?? "等待回读"}`,
      `完整画面刷新     ${quality.fullFrameRefreshCount ?? 0} 次`,
      `编码/AVC         ${quality.avcStatus || "未检测"}`,
      `最近操作         ${quality.lastOperation || "未知"}`,
      `最近 HRESULT     ${lastResult}`
    ].join("\n");
  };
  const showRdpQualityPanel = () => {
    if (!rdpFullscreen || !rdpQualityPanel) return;
    renderRdpQualityPanel(rdpFullscreenSessionId || activeRdpSessionId());
    rdpQualityPanel.hidden = false;
  };
  const hideRdpQualityPanel = () => {
    if (rdpQualityPanel) rdpQualityPanel.hidden = true;
  };
  const requestRdpFullscreen = (enabled, sessionId = enabled
    ? activeRdpSessionId()
    : (rdpFullscreenSessionId || activeRdpSessionId())) => post(
    "session.rdpFullscreen", {
      sessionId, enabled: Boolean(enabled)
    });
  const hideRdpFullscreenBar = () => {
    if (rdpFullscreenBarTimer) {
      clearTimeout(rdpFullscreenBarTimer);
      rdpFullscreenBarTimer = 0;
    }
    if (rdpFullscreenBarPinned || !rdpFullscreenBarVisible) return;
    rdpFullscreenBarVisible = false;
    rdpFullscreenMenu = "";
    syncRdpFullscreenUi();
    notifyRdpLayout(false, true);
  };
  const scheduleRdpFullscreenBarHide = () => {
    if (!rdpFullscreen || rdpFullscreenBarPinned) return;
    if (rdpFullscreenBarTimer) clearTimeout(rdpFullscreenBarTimer);
    rdpFullscreenBarTimer = setTimeout(() => {
      rdpFullscreenBarTimer = 0;
      hideRdpFullscreenBar();
    }, 2800);
  };
  const showRdpFullscreenBar = () => {
    if (!rdpFullscreen) return;
    rdpFullscreenBarVisible = true;
    syncRdpFullscreenUi();
    if (!rdpFullscreenBarPinned)
      scheduleRdpFullscreenBarHide();
  };
  let rdpLayoutFrame = 0;
  let rdpLayoutRefresh = false;
  let rdpLayoutReflowAll = false;
  const notifyRdpLayout = (
    refresh = true, clearOcclusion = false, reflowAll = false) => {
    if (!Array.from(sessions.values())
      .some(session => session.connectionType === "rdp"))
      return Promise.resolve();
    rdpLayoutRefresh = rdpLayoutRefresh || refresh;
    rdpLayoutReflowAll = rdpLayoutReflowAll || reflowAll;
    if (clearOcclusion) {
      if (rdpLayoutFrame) {
        cancelAnimationFrame(rdpLayoutFrame);
        rdpLayoutFrame = 0;
      }
      const shouldRefresh = rdpLayoutRefresh;
      const shouldReflowAll = rdpLayoutReflowAll;
      rdpLayoutRefresh = false;
      rdpLayoutReflowAll = false;
      return post("session.rdpLayout", {
        sessionId: activeRdpSessionId(),
        visible: activeRdpVisible(),
        refresh: shouldRefresh,
        clearOcclusion: true,
        reflowAll: shouldReflowAll
      }).catch(() => {});
    }
    if (rdpLayoutFrame) return;
    rdpLayoutFrame = requestAnimationFrame(() => {
      rdpLayoutFrame = 0;
      const shouldRefresh = rdpLayoutRefresh;
      const shouldReflowAll = rdpLayoutReflowAll;
      rdpLayoutRefresh = false;
      rdpLayoutReflowAll = false;
      post("session.rdpLayout", {
        sessionId: activeRdpSessionId(),
        visible: activeRdpVisible(),
        refresh: shouldRefresh,
        clearOcclusion: false,
        reflowAll: shouldReflowAll
      }).catch(() => {});
    });
  };
  const clearRdpOverlay = () => notifyRdpLayout(false, true);
  // The embedded RDP control is a native child HWND above WebView2. Ask the
  // host to refresh its clipped region whenever an HTML dialog/menu changes;
  // the desktop remains visible everywhere outside that overlay rectangle.
  const rdpOverlayMutationSelector = [
    "dialog:not(.rdp-editor-fallback-freeze)",
    "#sidebar-flyout-occlusion", "#sidebar-card-shadow", ".context-menu",
    ".terminal-create-dropdown", "#rdp-fullscreen-bar",
    "#rdp-fullscreen-menu", "#rdp-quality-panel", "#rdp-tab-hover-card",
    "#rdp-fullscreen-hot-zone", ".rdp-connecting-notice"
  ].join(",");
  const isRdpOverlayMutation = record => {
    const target = record.target instanceof Element ? record.target : null;
    if (record.type === "attributes") {
      if (!target?.matches(rdpOverlayMutationSelector)) return false;
      if (record.attributeName === "hidden")
        return (record.oldValue !== null) !== target.hidden;
      if (record.attributeName === "open")
        return (record.oldValue !== null) !== target.open;
      return true;
    }
    return [...record.addedNodes, ...record.removedNodes].some(node =>
      node instanceof Element && (node.matches(rdpOverlayMutationSelector)
        || node.querySelector(rdpOverlayMutationSelector)));
  };
  const rdpOverlayObserver = new MutationObserver(records => {
    const overlayRecords = records.filter(isRdpOverlayMutation);
    if (!overlayRecords.length) return;
    const clearOcclusion = overlayRecords.some(record => {
      if (record.type === "attributes") {
        return (record.attributeName === "hidden" && record.target.hidden)
          || (record.attributeName === "open" && !record.target.open);
      }
      return record.type === "childList" && record.removedNodes.length > 0;
    });
    notifyRdpLayout(false, clearOcclusion);
  });
  rdpOverlayObserver.observe(document.body, {
    subtree: true,
    childList: true,
    attributes: true,
    attributeOldValue: true,
    attributeFilter: ["hidden", "open"]
  });

  const mergeCommandHistory = (...sources) => {
    const merged = [];
    for (const source of sources) {
      if (!Array.isArray(source)) continue;
      for (const command of source) {
        const value = String(command || "").replace(/[\r\n]+/g, " ").trim();
        if (!value) continue;
        const previous = merged.indexOf(value);
        if (previous >= 0) merged.splice(previous, 1);
        merged.push(value);
      }
    }
    const limit = Math.max(1, Number(appSettings.historyLimit) || 500);
    return merged.slice(-limit);
  };

  const closeTerminalCreateDropdown = group => {
    const dropdown = group?.querySelector(".terminal-create-dropdown");
    const menu = group?.querySelector(".terminal-create-menu");
    if (!dropdown || !menu) return;
    dropdown.hidden = true;
    menu.setAttribute("aria-expanded", "false");
  };

  const openExistingConnection = profile => {
    if (!profile) return;
    connectProfile(profile);
  };

  const renderExistingConnectionOptions = dropdown => {
    const options = dropdown.querySelector(".terminal-create-options");
    if (!options) return;
    options.replaceChildren();

    const shellTitle = document.createElement("div");
    shellTitle.className = "terminal-create-dropdown-title";
    shellTitle.textContent = "新建本地终端";
    options.appendChild(shellTitle);

    const availableShells = [
      { id: "default", name: "本地终端 (默认)", icon: "💻", shellType: null },
      { id: "pwsh", name: "PowerShell 7", icon: "⚡", shellType: "pwsh" },
      { id: "powershell", name: "Windows PowerShell", icon: "🔷", shellType: "powershell" },
      { id: "cmd", name: "命令提示符 (CMD)", icon: "⬛", shellType: "cmd" },
      { id: "wsl", name: "WSL (Linux 子系统)", icon: "🐧", shellType: "wsl" },
      { id: "gitbash", name: "Git Bash", icon: "🔶", shellType: "gitbash" }
    ];
    availableShells.forEach(sh => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.dataset.shell = sh.shellType || "default";
      const isInstalled = (sh.id === "default" || sh.id === "cmd") ? true : (detectedShells ? detectedShells.get(sh.id) !== false : true);
      const uninstalledTag = (!isInstalled && detectedShells) ? ' <span style="opacity: 0.55; font-size: 11px; margin-left: auto;">(未安装)</span>' : '';
      btn.innerHTML = `<span class="terminal-create-opt-icon">${sh.icon}</span> <span>${sh.name}</span>${uninstalledTag}`;
      if (!isInstalled && detectedShells) {
        btn.dataset.uninstalled = "true";
        btn.title = `${sh.name} 本地未安装`;
      }
      options.appendChild(btn);
    });

    const profiles = sortRecentConnections(profilesCache.filter(profile =>
      ["ssh", "rdp", "serial"].includes(profile.connectionType)));
    if (profiles.length) {
      const connTitle = document.createElement("div");
      connTitle.className = "terminal-create-dropdown-title";
      connTitle.style.marginTop = "6px";
      connTitle.textContent = "已有连接";
      options.appendChild(connTitle);

      profiles.forEach(profile => {
        const button = document.createElement("button");
        button.type = "button";
        button.dataset.profileIndex = String(profile.index);
        const typeLabel = profile.connectionType === "rdp" ? "RDP"
          : profile.connectionType === "serial" ? "串口" : "SSH";
        const typeIcon = profile.connectionType === "rdp" ? "🖥️"
          : profile.connectionType === "serial" ? "🔌" : "🌐";
        button.innerHTML = `<span class="terminal-create-opt-icon">${typeIcon}</span> <span>${profile.name || profile.address}（${typeLabel}）</span>`;
        options.appendChild(button);
      });
    }
  };

  const resolveTabGroupHost = group => {
    if (!splitMode) return terminalTabs;
    if (!group) return null;
    if (group.closest?.(".tab-group.left")) return splitViewLeft?.querySelector(".tab-group.left") || null;
    if (group.closest?.(".tab-group.right")) return splitViewRight?.querySelector(".tab-group.right") || null;
    if (group.closest?.("#terminal-tabs") || group === terminalTabs) return terminalTabs;
    const tg = group.closest?.(".tab-group");
    return tg?.isConnected ? tg : null;
  };

  const connectLocalTerminal = (shellType = null, group = null) => {
    if (shellType && detectedShells && detectedShells.get(shellType) === false) {
      showTransientStatus(`本地未检测到该终端 (${shellType})，请先安装或选择其它终端。`);
      return Promise.resolve();
    }
    const targetGroup = resolveTabGroupHost(group);
    pendingNewTerminalGroup = targetGroup?.isConnected && splitMode ? targetGroup : null;
    const payload = {};
    if (shellType) {
      payload.shellType = shellType;
    } else if (appSettings.defaultLocalShell && appSettings.defaultLocalShell !== "auto") {
      payload.shellType = appSettings.defaultLocalShell;
    }
    if (appSettings.localWorkingDir) {
      payload.workingDir = appSettings.localWorkingDir;
    }
    return post("local.connect", payload).catch(error => {
      pendingNewTerminalGroup = null;
      reportError(error);
    });
  };

  const installTerminalCreateGroup = group => {
    const menu = group.querySelector(".terminal-create-menu");
    const dropdown = group.querySelector(".terminal-create-dropdown");
    if (!menu || !dropdown) return;
    menu.addEventListener("click", event => {
      event.stopPropagation();
      const opening = dropdown.hidden;
      if (!opening) {
        closeTerminalCreateDropdown(group);
        return;
      }
      if (!detectedShells) {
        refreshDetectedShells().catch(() => {});
      }
      if (activeRdpVisible()) {
        closeTerminalCreateDropdown(group);
        nativeQuickOpenGroup = resolveTabGroupHost(group);
        const anchor = menu.getBoundingClientRect();
        const items = [{ index: -1, label: "本地终端" }];
        sortRecentConnections(profilesCache.filter(profile =>
          ["ssh", "rdp", "serial"].includes(profile.connectionType)))
          .forEach(profile => {
            const typeLabel = profile.connectionType === "rdp" ? "RDP"
              : profile.connectionType === "serial" ? "串口" : "SSH";
            items.push({
              index: profile.index,
              label: `${profile.name || profile.address}（${typeLabel}）`
            });
          });
        post("app.existingConnectionsMenu", {
          x: Math.round(anchor.left),
          y: Math.round(anchor.bottom + 4),
          items
        }).catch(error => {
          nativeQuickOpenGroup = null;
          reportError(error);
        });
        return;
      }
      renderExistingConnectionOptions(dropdown);
      dropdown.hidden = false;
      menu.setAttribute("aria-expanded", "true");
      const anchor = menu.getBoundingClientRect();
      const bounds = dropdown.getBoundingClientRect();
      const left = Math.max(8, Math.min(anchor.left,
        window.innerWidth - bounds.width - 8));
      const top = anchor.bottom + bounds.height + 8 <= window.innerHeight
        ? anchor.bottom + 4 : Math.max(8, anchor.top - bounds.height - 4);
      dropdown.style.left = `${left}px`;
      dropdown.style.top = `${top}px`;
    });
    dropdown.addEventListener("click", event => {
      const shellButton = event.target.closest("button[data-shell]");
      if (shellButton) {
        closeTerminalCreateDropdown(group);
        const shell = shellButton.dataset.shell === "default" ? null : shellButton.dataset.shell;
        if (shellButton.dataset.uninstalled === "true") {
          showTransientStatus(`本地未检测到该终端 (${shellButton.querySelector("span:nth-child(2)")?.textContent || shell})，请先安装或选择其它终端。`);
          return;
        }
        connectLocalTerminal(shell, group);
        return;
      }
      const localButton = event.target.closest("button[data-local-terminal]");
      if (localButton) {
        closeTerminalCreateDropdown(group);
        connectLocalTerminal(null, group);
        return;
      }
      const button = event.target.closest("button[data-profile-index]");
      if (!button) return;
      closeTerminalCreateDropdown(group);
      const profile = profilesCache.find(item =>
        String(item.index) === button.dataset.profileIndex);
      if (!profile) return;
      const targetGroup = resolveTabGroupHost(group);
      pendingNewTerminalGroup = targetGroup?.isConnected && splitMode
        ? targetGroup : null;
      openExistingConnection(profile);
    });
  };

  const cloneTerminalCreateGroup = () => {
    const clone = terminalCreateGroup.cloneNode(true);
    clone.querySelectorAll("[id]").forEach(element => element.removeAttribute("id"));
    clone.querySelector(".terminal-create-dropdown").hidden = true;
    installTerminalCreateGroup(clone);
    return clone;
  };
  // Keep the quick-open control after all tabs.  Every insertion path uses
  // this helper so sorting and new sessions cannot move it into the tab list.
  const insertTerminalTab = (host, tab, reference = null) => {
    if (!host || !tab) return;
    const createGroup = host.querySelector(".terminal-create-group");
    if (reference) {
      host.insertBefore(tab, reference);
    } else if (createGroup) {
      host.insertBefore(tab, createGroup);
    } else {
      host.appendChild(tab);
    }
    ensureTerminalQuickOpenLast(host);
  };
  const ensureTerminalQuickOpenLast = host => {
    const createGroup = host?.querySelector(".terminal-create-group");
    if (createGroup && createGroup !== host.lastElementChild)
      host.appendChild(createGroup);
  };
  const appendTerminalTabs = (host, tabs) => {
    tabs.forEach(tab => insertTerminalTab(host, tab));
  };
  const loadLocalCommandHistory = () => {
    if (localCommandHistoryPromise) return localCommandHistoryPromise;
    localCommandHistoryPromise = post("local.history.load")
      .then(result => {
        const commands = Array.isArray(result) ? result : [];
        // Keep commands entered while the first load was in flight.
        localCommandHistory = mergeCommandHistory(commands, localCommandHistory);
        sessions.forEach(session => {
          if (session.connectionType !== "local") return;
          session.commandHistory = mergeCommandHistory(
            localCommandHistory, session.commandHistory);
          session.completion.loaded = true;
          if (session.completion.visible) renderCompletion(session);
        });
        return localCommandHistory;
      })
      .catch(error => {
        localCommandHistoryPromise = null;
        reportError(error);
        return localCommandHistory;
      });
    return localCommandHistoryPromise;
  };
  const rememberLocalCommand = command => {
    localCommandHistory = mergeCommandHistory(localCommandHistory, [command]);
    post("local.history.append", { command }).catch(reportError);
  };

  const notify = (title, message) => {
    post("app.notify", { title, message }).catch(() => {});
  };

  const themes = {
    dark: { pageBackground: "#111827",
      terminal: { background: "#0b1220", foreground: "#d1fae5", cursor: "#d1fae5",
      red: "#f87171", green: "#4ade80", yellow: "#facc15", blue: "#60a5fa", magenta: "#e879f9", cyan: "#67e8f9",
      // Search/selection highlights must stay visible on the dark canvas;
      // xterm's default (30% white) becomes invisible on light themes.
      selectionBackground: "rgba(59, 130, 246, 0.45)",
      selectionInactiveBackground: "rgba(59, 130, 246, 0.22)" } },
    light: { pageBackground: "#e9eef5",
      terminal: { background: "#f8fafc", foreground: "#1e293b", cursor: "#1e293b",
      red: "#dc2626", green: "#15803d", yellow: "#a16207", blue: "#2563eb", magenta: "#a21caf", cyan: "#0e7490",
      selectionBackground: "rgba(37, 99, 235, 0.30)",
      selectionInactiveBackground: "rgba(37, 99, 235, 0.18)" } },
    blue: { pageBackground: "#061b30",
      terminal: { background: "#061b30", foreground: "#d8ecff", cursor: "#d8ecff",
      red: "#fb7185", green: "#4ade80", yellow: "#facc15", blue: "#60a5fa", magenta: "#e879f9", cyan: "#67e8f9",
      selectionBackground: "rgba(96, 165, 250, 0.42)",
      selectionInactiveBackground: "rgba(96, 165, 250, 0.20)" } }
  };
  const terminalThemePresets = {
    "one-dark": {
      name: "One Dark",
      background: "#282c34",
      foreground: "#abb2bf",
      cursor: "#528bff",
      red: "#e06c75", green: "#98c379", yellow: "#e5c07b", blue: "#61afef", magenta: "#c678dd", cyan: "#56b6c2",
      selectionBackground: "rgba(97, 175, 239, 0.35)",
      selectionInactiveBackground: "rgba(97, 175, 239, 0.18)"
    },
    "dracula": {
      name: "Dracula",
      background: "#282a36",
      foreground: "#f8f8f2",
      cursor: "#f8f8f2",
      red: "#ff5555", green: "#50fa7b", yellow: "#f1fa8c", blue: "#bd93f9", magenta: "#ff79c6", cyan: "#8be9fd",
      selectionBackground: "rgba(189, 147, 249, 0.35)",
      selectionInactiveBackground: "rgba(189, 147, 249, 0.18)"
    },
    "nord": {
      name: "Nord",
      background: "#2e3440",
      foreground: "#d8dee9",
      cursor: "#d8dee9",
      red: "#bf616a", green: "#a3be8c", yellow: "#ebcb8b", blue: "#81a1c1", magenta: "#b48ead", cyan: "#88c0d0",
      selectionBackground: "rgba(136, 192, 208, 0.35)",
      selectionInactiveBackground: "rgba(136, 192, 208, 0.18)"
    },
    "monokai": {
      name: "Monokai Pro",
      background: "#2d2a2e",
      foreground: "#fcfcfa",
      cursor: "#fcfcfa",
      red: "#ff6188", green: "#a9dc76", yellow: "#ffd866", blue: "#fc9867", magenta: "#ab9df2", cyan: "#78dce8",
      selectionBackground: "rgba(255, 216, 102, 0.35)",
      selectionInactiveBackground: "rgba(255, 216, 102, 0.18)"
    },
    "solarized-dark": {
      name: "Solarized Dark",
      background: "#002b36",
      foreground: "#839496",
      cursor: "#839496",
      red: "#dc322f", green: "#859900", yellow: "#b58900", blue: "#268bd2", magenta: "#d33682", cyan: "#2aa198",
      selectionBackground: "rgba(38, 139, 210, 0.35)",
      selectionInactiveBackground: "rgba(38, 139, 210, 0.18)"
    },
    "catppuccin-mocha": {
      name: "Catppuccin Mocha",
      background: "#1e1e2e",
      foreground: "#cdd6f4",
      cursor: "#f5e0dc",
      red: "#f38ba8", green: "#a6e3a1", yellow: "#f9e2af", blue: "#89b4fa", magenta: "#f5c2e7", cyan: "#94e2d5",
      selectionBackground: "rgba(137, 180, 250, 0.35)",
      selectionInactiveBackground: "rgba(137, 180, 250, 0.18)"
    },
    "github-dark": {
      name: "GitHub Dark",
      background: "#0d1117",
      foreground: "#c9d1d9",
      cursor: "#58a6ff",
      red: "#ff7b72", green: "#3fb950", yellow: "#d29922", blue: "#58a6ff", magenta: "#bc8cff", cyan: "#39c5cf",
      selectionBackground: "rgba(88, 166, 255, 0.35)",
      selectionInactiveBackground: "rgba(88, 166, 255, 0.18)"
    },
    "github-light": {
      name: "GitHub Light (浅色)",
      background: "#ffffff",
      foreground: "#24292f",
      cursor: "#0969da",
      red: "#cf222e", green: "#116329", yellow: "#4d2d00", blue: "#0969da", magenta: "#8250df", cyan: "#1b7c83",
      selectionBackground: "rgba(9, 105, 218, 0.22)",
      selectionInactiveBackground: "rgba(9, 105, 218, 0.12)"
    },
    "one-light": {
      name: "One Light (浅色)",
      background: "#fafafa",
      foreground: "#383a42",
      cursor: "#526fff",
      red: "#e45649", green: "#50a14f", yellow: "#c18401", blue: "#4078f2", magenta: "#a626a4", cyan: "#0184bc",
      selectionBackground: "rgba(64, 120, 242, 0.22)",
      selectionInactiveBackground: "rgba(64, 120, 242, 0.12)"
    },
    "solarized-light": {
      name: "Solarized Light (浅色)",
      background: "#fdf6e3",
      foreground: "#657b83",
      cursor: "#657b83",
      red: "#dc322f", green: "#859900", yellow: "#b58900", blue: "#268bd2", magenta: "#d33682", cyan: "#2aa198",
      selectionBackground: "rgba(38, 139, 210, 0.22)",
      selectionInactiveBackground: "rgba(38, 139, 210, 0.12)"
    }
  };
  const terminalTheme = () => {
    const baseTheme = themes[activeTheme].terminal;
    let preset = null;
    if (appSettings.terminalThemePreset && appSettings.terminalThemePreset !== "custom" && appSettings.terminalThemePreset !== "auto") {
      preset = terminalThemePresets[appSettings.terminalThemePreset];
    }
    const themeToUse = preset ? { ...baseTheme, ...preset } : baseTheme;
    const background = (preset ? preset.background : appSettings.terminalBackground) || baseTheme.background;
    const foreground = (preset ? preset.foreground : appSettings.terminalForeground) || baseTheme.foreground;
    return {
      ...themeToUse,
      background,
      foreground,
      cursor: foreground,
      // xterm.js uses cursorAccent for the glyph drawn inside a block cursor.
      // Keep it opposite to the cursor background so the character remains
      // readable in the light theme (and when custom terminal colors are used).
      cursorAccent: background
    };
  };
  const terminalFontName = value => String(value || "").split(",")[0]
    .trim().replace(/^['"]|['"]$/g, "") || "Cascadia Mono";
  const terminalFontStack = value => {
    // Monospace fonts must come first so xterm metrics use a monospaced
    // face even when the user's chosen font is missing on this machine.
    // CJK fonts at the end only serve full-width characters, never metrics.
    const primary = String(value || "Cascadia Mono, Consolas, Courier New, monospace");
    return `${primary}, Consolas, "Courier New", monospace, `
      + `"Microsoft YaHei UI", "微软雅黑", "SimHei", "Noto Sans CJK SC"`;
  };
  const populateTerminalFontOptions = selectedValue => {
    const selected = terminalFontName(selectedValue);
    const fallback = ["Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console", "Courier New"];
    const names = [...new Set([...terminalFontFamilies, ...fallback, selected])]
      .sort((left, right) => left.localeCompare(right, undefined, { sensitivity: "base" }));
    const select = document.querySelector("#setting-font-family");
    select.replaceChildren(...names.map(name => new Option(name, name, false, name === selected)));
  };
  const loadTerminalFontFamilies = async () => {
    try {
      const fonts = await post("font.list");
      if (!Array.isArray(fonts)) return;
      terminalFontFamilies = fonts.filter(font => typeof font === "string" && font.trim());
      populateTerminalFontOptions(appSettings.fontFamily);
      updateTerminalSettingsPreview();
    } catch {
      // The compact fallback list remains usable if font enumeration is unavailable.
    }
  };
  const updateTerminalSettingsPreview = () => {
    const theme = themes[activeTheme].terminal;
    const fontSize = Math.min(32, Math.max(8,
      Number(document.querySelector("#setting-font-size").value) || defaultSettings.fontSize));
    const fontFamily = document.querySelector("#setting-font-family").value
      || terminalFontName(defaultSettings.fontFamily);
    const background = document.querySelector("#setting-terminal-background").value
      || theme.background;
    const foreground = document.querySelector("#setting-terminal-foreground").value
      || theme.foreground;
    settingsTerminalPreview.style.fontFamily = fontFamily;
    settingsTerminalPreview.style.fontSize = `${fontSize}px`;
    settingsTerminalPreview.style.setProperty("--preview-background", background);
    settingsTerminalPreview.style.setProperty("--preview-foreground", foreground);
    for (const color of ["red", "green", "yellow", "magenta", "cyan"])
      settingsTerminalPreview.style.setProperty(`--preview-${color}`, theme[color] || themes[activeTheme].terminal[color]);
  };
  const applyTerminalSettings = () => {
    const theme = terminalTheme();
    document.documentElement.style.setProperty("--terminal-background", theme.background);
    document.documentElement.style.setProperty("--terminal-foreground", theme.foreground);
    document.documentElement.style.setProperty("--terminal-tab-active-bg", theme.background);
    sessions.forEach(session => {
      session.terminal.options.fontSize = appSettings.fontSize;
      session.terminal.options.fontFamily = terminalFontStack(appSettings.fontFamily);
      session.terminal.options.scrollback = appSettings.scrollback;
      session.terminal.options.cursorStyle = appSettings.cursorStyle;
      session.terminal.options.cursorBlink = appSettings.cursorBlink;
      session.terminal.options.theme = theme;
      session.output.style.setProperty("--terminal-background", theme.background);
      session.panel?.style.setProperty("--terminal-background", theme.background);
      session.scheduleRefit?.();
      session.terminal.refresh(0, Math.max(0, session.terminal.rows - 1));
    });
    updateThemeMenuChecks();
  };
  const updateThemeMenuChecks = () => {
    if (!themeMenu) return;
    const currentPreset = appSettings.terminalThemePreset || "custom";
    themeMenu.querySelectorAll(".theme-preset-btn").forEach(btn => {
      const btnTheme = btn.dataset.theme;
      const btnPreset = btn.dataset.preset;
      const isSelected = (btnTheme === activeTheme && btnPreset === currentPreset);
      btn.setAttribute("aria-checked", String(isSelected));
      let check = btn.querySelector(".theme-preset-check");
      if (isSelected) {
        if (!check) {
          check = document.createElement("span");
          check.className = "theme-preset-check";
          check.textContent = "✓";
          btn.appendChild(check);
        }
      } else {
        if (check) check.remove();
      }
    });
    themeMenu.querySelectorAll(".theme-menu-cat-btn").forEach(catBtn => {
      const isCurrentCat = (catBtn.dataset.theme === activeTheme);
      catBtn.setAttribute("aria-selected", String(isCurrentCat));
      catBtn.classList.toggle("active", isCurrentCat);
    });
  };
  const applyTheme = theme => {
    if (!themes[theme]) return;
    activeTheme = theme;
    post("app.theme", { theme }).catch(() => {});
    document.documentElement.dataset.theme = theme;
    document.documentElement.style.colorScheme = theme === "light" ? "light" : "dark";
    localStorage.setItem("masterterm.theme", theme);
    themeMenu.querySelectorAll("[data-theme]").forEach(button =>
      button.setAttribute("aria-checked", String(button.dataset.theme === theme)));
    if (frontendReady)
      document.documentElement.style.background =
        themes[theme].pageBackground || "#111827";
    applyTerminalSettings();
    updateThemeMenuChecks();
  };

  const populateSettingsForm = () => {
    const theme = themes[activeTheme].terminal;
    document.querySelector("#setting-local-panel").checked = appSettings.localPanelOnOpen;
    document.querySelector("#setting-confirm-delete").checked = appSettings.confirmDelete;
    document.querySelector("#setting-restore-sessions").checked = appSettings.restoreSessions;
    document.querySelector("#setting-session-logging").checked = appSettings.sessionLogging;
    document.querySelector("#setting-keepalive").value = appSettings.keepalive;
    document.querySelector("#setting-auto-reconnect").checked = appSettings.autoReconnect;
    document.querySelector("#setting-reconnect-attempts").value = appSettings.reconnectAttempts;
    document.querySelector("#setting-remote-edit-conflict").value = appSettings.remoteEditConflict;
    document.querySelector("#setting-font-size").value = appSettings.fontSize;
    populateTerminalFontOptions(appSettings.fontFamily);
    document.querySelector("#setting-scrollback").value = appSettings.scrollback;
    document.querySelector("#setting-terminal-background").value =
      appSettings.terminalBackground || theme.background;
    document.querySelector("#setting-terminal-foreground").value =
      appSettings.terminalForeground || theme.foreground;
    document.querySelector("#setting-history-limit").value = appSettings.historyLimit;
    document.querySelector("#setting-completion-limit").value = appSettings.completionLimit;
    document.querySelector("#setting-history-days").value = appSettings.historyDays;
    document.querySelector("#setting-read-bash").checked = appSettings.readBashHistory;
    document.querySelector("#setting-read-zsh").checked = appSettings.readZshHistory;
    document.querySelector("#setting-history-deduplicate").checked = appSettings.deduplicateHistory;
    document.querySelector("#setting-copy-on-select").checked = appSettings.copyOnSelect;
    document.querySelector("#setting-cursor-style").value = appSettings.cursorStyle;
    document.querySelector("#setting-cursor-blink").checked = appSettings.cursorBlink;
    document.querySelector("#setting-alert-enabled").checked = appSettings.alertsEnabled;
    document.querySelector("#setting-alert-cpu").value = appSettings.alertCpuThreshold;
    document.querySelector("#setting-alert-memory").value = appSettings.alertMemoryThreshold;
    document.querySelector("#setting-alert-disk").value = appSettings.alertDiskThreshold;
    document.querySelector("#setting-connection-color-ssh").value = appSettings.connectionTypeColorSsh;
    document.querySelector("#setting-connection-color-rdp").value = appSettings.connectionTypeColorRdp;
    document.querySelector("#setting-connection-color-serial").value = appSettings.connectionTypeColorSerial;
    const defaultShellSelect = document.querySelector("#setting-default-local-shell");
    if (defaultShellSelect) {
      defaultShellSelect.value = appSettings.defaultLocalShell || "powershell";
      post("local.detectShells").then(detected => {
        if (!Array.isArray(detected)) return;
        detected.forEach(shell => {
          const opt = defaultShellSelect.querySelector(`option[value="${shell.id}"]`);
          const available = Boolean(shell.available || shell.exists);
          if (opt) {
            const cleanText = opt.textContent.replace(/\s*[✓✔]\s*/g, "").replace(/\s*\(未安装\)\s*/g, "");
            opt.textContent = available ? `${cleanText} ✓` : `${cleanText} (未安装)`;
          }
        });
      }).catch(() => {});
    }
    const localWorkingDirInput = document.querySelector("#setting-local-working-dir");
    if (localWorkingDirInput) localWorkingDirInput.value = appSettings.localWorkingDir || "";
    const themePresetSelect = document.querySelector("#setting-terminal-theme-preset");
    if (themePresetSelect) themePresetSelect.value = appSettings.terminalThemePreset || "custom";
    document.querySelector("#update-check-status").textContent = "";
    settingsStatus.textContent = "";
    updateTerminalSettingsPreview();
    if (serverSortMode) serverSortMode.value = appSettings.serverSortMode;
    if (serverGroupMode) serverGroupMode.value = appSettings.serverGroupMode;
  };

  const cloudSyncStorageKey = "masterterm.cloud";
  const cloudSyncDefaultUrl = "https://vm.dapang.wang/cloud-sync";
  const updateServerDefaultUrl = "https://vm.dapang.wang/updates/masterterm.json";
  let cloudState = { url: cloudSyncDefaultUrl, username: "", token: "", updatedAt: "" };
  try {
    const storedCloud = JSON.parse(localStorage.getItem(cloudSyncStorageKey) || "{}");
    if (storedCloud && typeof storedCloud === "object")
      cloudState = { ...cloudState, ...storedCloud };
  } catch { /* malformed stored state falls back to defaults */ }
  const cloudSyncUsername = document.querySelector("#cloud-sync-username");
  const cloudSyncPassword = document.querySelector("#cloud-sync-password");
  const cloudRegisterButton = document.querySelector("#cloud-register");
  const cloudLoginButton = document.querySelector("#cloud-login");
  const cloudLogoutButton = document.querySelector("#cloud-logout");
  const cloudPushButton = document.querySelector("#cloud-push");
  const cloudPullButton = document.querySelector("#cloud-pull");
  const cloudSyncStatus = document.querySelector("#cloud-sync-status");
  const cloudSyncTime = document.querySelector("#cloud-sync-time");
  const cloudDiffDialog = document.querySelector("#cloud-diff-dialog");
  const cloudHistoryDialog = document.querySelector("#cloud-history-dialog");
  const cloudHistoryList = document.querySelector("#cloud-history-list");
  const cloudHistoryEmpty = document.querySelector("#cloud-history-empty");
  const cloudHistoryStatus = document.querySelector("#cloud-history-status");
  const updateCloudSyncUi = () => {
    const loggedIn = Boolean(cloudState.token);
    cloudSyncUsername.value = cloudState.username || "";
    cloudSyncUsername.disabled = loggedIn;
    cloudSyncPassword.disabled = loggedIn;
    const passwordLength = Number(cloudState.passwordLength);
    cloudSyncPassword.placeholder = loggedIn
      ? "•".repeat(Number.isFinite(passwordLength) && passwordLength > 0
          ? Math.floor(passwordLength) : 3)
      : "";
    cloudRegisterButton.disabled = loggedIn;
    cloudLoginButton.disabled = loggedIn;
    cloudLogoutButton.hidden = !loggedIn;
    cloudPushButton.disabled = !loggedIn;
    cloudPullButton.disabled = !loggedIn;
    document.querySelector("#cloud-history").disabled = !loggedIn;
    cloudSyncStatus.textContent = loggedIn
      ? `已登录：${cloudState.username}` : "未登录";
    cloudSyncTime.textContent = cloudState.updatedAt
      ? `上次同步：${new Date(cloudState.updatedAt).toLocaleString()}` : "";
    if (!loggedIn) cloudSyncPassword.value = "";
    updateSidebarCloudUi?.();
  };
  const saveCloudState = () => {
    localStorage.setItem(cloudSyncStorageKey, JSON.stringify(cloudState));
    updateCloudSyncUi();
  };
  const handleCloudError = error => {
    const msg = error instanceof Error ? error.message : String(error || "操作失败");
    if (msg.includes("无效或过期的令牌") || msg.includes("401") || msg.includes("未授权")) {
      cloudState.token = "";
      saveCloudState();
      cloudSyncStatus.textContent = "登录凭证已失效（令牌已过期或服务器重置），请重新输入密码登录或注册新用户";
      return;
    }
    cloudSyncStatus.textContent = msg;
  };

  const cloudAuth = register => {
    const url = cloudState.url || cloudSyncDefaultUrl;
    const username = cloudSyncUsername.value.trim();
    const password = cloudSyncPassword.value;
    if (!username || !password) {
      cloudSyncStatus.textContent = "请输入用户名和密码";
      return;
    }
    cloudSyncStatus.textContent = register ? "正在注册…" : "正在登录…";
    post(register ? "cloud.register" : "cloud.login", { url, username, password })
      .then(result => {
        cloudState = {
          url, username, token: result.token, updatedAt: "",
          passwordLength: password.length
        };
        saveCloudState();
        cloudSyncStatus.textContent = register ? "注册成功，已自动登录" : "登录成功";
      })
      .catch(error => {
        const msg = error instanceof Error ? error.message : String(error);
        if (!register && (msg.includes("用户名或密码错误") || msg.includes("用户不存在"))) {
          cloudSyncStatus.textContent = "用户名或密码错误。如果是新服务器首次使用，请点击【注册】创建账号";
        } else {
          cloudSyncStatus.textContent = msg;
        }
      });
  };
  // ---- 云同步 schema 版本与迁移（T2-1）----
  // 云端 payload 结构：{ version, savedAt, servers, workspaces, history,
  // settings }。下载时按版本逐步迁移到当前版本；版本高于当前客户端时
  // 拒绝应用，避免旧客户端破坏新数据；每节独立清洗，坏数据只降级不报错。
  // begin cloud-schema
  const createCloudSchema = defaults => {
    const CLOUD_SCHEMA_VERSION = 2;
    const migrateWorkspaces = workspaces => {
      const result = [];
      const seen = new Set();
      for (const value of Array.isArray(workspaces) ? workspaces : []) {
        const name = typeof value === "string" ? value.trim() : "";
        if (!name || seen.has(name)) continue;
        seen.add(name);
        result.push(name);
      }
      const unassigned = "未分配";
      if (!seen.has(unassigned)) {
        result.unshift(unassigned);
      } else if (result[0] !== unassigned) {
        result.splice(result.indexOf(unassigned), 1);
        result.unshift(unassigned);
      }
      return result;
    };
    const migrateHistory = history => Array.isArray(history)
      ? history.filter(entry => typeof entry === "string" && entry.trim())
          .slice(0, 2000)
      : [];
    const migrateSettings = settings => {
      const merged = { ...defaults };
      if (!settings || typeof settings !== "object") return merged;
      for (const key of Object.keys(defaults)) {
        if (key in settings && settings[key] !== undefined
            && settings[key] !== null
            && typeof settings[key] === typeof defaults[key]) {
          merged[key] = settings[key];
        }
      }
      return merged;
    };
    const migrateCloudServers = servers => (Array.isArray(servers) ? servers : [])
      .filter(item => item && typeof item === "object"
          && typeof item.address === "string" && item.address.trim())
      .map(item => ({
        connectionType: ["ssh", "serial", "rdp"].includes(item.connectionType)
          ? item.connectionType : "ssh",
        name: typeof item.name === "string" && item.name
          ? item.name : item.address,
        address: item.address,
        port: typeof item.port === "string" || typeof item.port === "number"
          ? String(item.port) : "22",
        tag: typeof item.tag === "string" ? item.tag : "SSH",
        tagMode: item.tagMode === "custom" ? "custom" : "auto",
        workspace: typeof item.workspace === "string" && item.workspace
          ? item.workspace : "未分配",
        color: typeof item.color === "string" ? item.color : "#8ab4f8",
        iconKey: typeof item.iconKey === "string" ? item.iconKey : "server",
        keyPath: typeof item.keyPath === "string" ? item.keyPath : "",
        keyPassphrase: typeof item.keyPassphrase === "string" ? item.keyPassphrase : "",
        proxyJump: typeof item.proxyJump === "string" ? item.proxyJump : "",
        proxyJumpEnabled: item.proxyJumpEnabled !== false,
        proxyKeyPath: typeof item.proxyKeyPath === "string" ? item.proxyKeyPath : "",
        serialDataBits: Number.isFinite(Number(item.serialDataBits))
          ? Math.min(8, Math.max(5, Math.trunc(Number(item.serialDataBits)))) : 8,
        serialParity: typeof item.serialParity === "string" ? item.serialParity : "none",
        serialStopBits: typeof item.serialStopBits === "string" ? item.serialStopBits : "1",
        serialFlowControl: typeof item.serialFlowControl === "string" ? item.serialFlowControl : "none",
        rdpOptions: item.rdpOptions && typeof item.rdpOptions === "object"
          ? item.rdpOptions : {},
        tunnels: Array.isArray(item.tunnels) ? item.tunnels : [],
        password: typeof item.password === "string" ? item.password : "",
        keyData: typeof item.keyData === "string" ? item.keyData : "",
        proxyKeyData: typeof item.proxyKeyData === "string" ? item.proxyKeyData : ""
      }));
    const migrateCloudPayload = data => {
      if (!data || typeof data !== "object" || Array.isArray(data)) return null;
      let version = Number.isFinite(Number(data.version))
        ? Math.trunc(Number(data.version)) : 0;
      if (version <= 0) version = 1;
      if (version > CLOUD_SCHEMA_VERSION)
        return { tooNew: true, version };
      return {
        version: CLOUD_SCHEMA_VERSION,
        savedAt: typeof data.savedAt === "string" ? data.savedAt : "",
        servers: migrateCloudServers(data.servers),
        workspaces: migrateWorkspaces(data.workspaces),
        history: migrateHistory(data.history),
        settings: migrateSettings(data.settings)
      };
    };
    return {
      CLOUD_SCHEMA_VERSION, migrateCloudPayload, migrateWorkspaces,
      migrateHistory, migrateSettings, migrateCloudServers
    };
  };
  const cloudSchema = createCloudSchema(defaultSettings);
  // end cloud-schema
  // ---- 上传前差异预览（T2-3）----
  // 对比云端与本地数据，输出新增/修改/删除的连接与工作区。凭据字段
  // （password/keyPassphrase/proxyPassword/keyData/proxyKeyData）不参与
  // 比较：DPAPI 密文在不同设备上必然不同。
  // begin cloud-diff
  const computeCloudDiff = (cloudServers, cloudWorkspaces, localServers, localWorkspaces) => {
    const secretKeys = new Set([
      "password", "keyPassphrase", "proxyPassword", "keyData", "proxyKeyData"
    ]);
    const profileKey = profile =>
      `${profile.connectionType || "ssh"}|${profile.address || ""}`;
    const comparable = profile => {
      const copy = { ...profile };
      for (const key of secretKeys) delete copy[key];
      return JSON.stringify(copy);
    };
    const clouds = cloudSchema.migrateCloudServers(cloudServers);
    const locals = cloudSchema.migrateCloudServers(localServers);
    const cloudByKey = new Map(clouds.map(profile => [profileKey(profile), profile]));
    const localByKey = new Map(locals.map(profile => [profileKey(profile), profile]));
    const added = [];
    const modified = [];
    for (const [key, local] of localByKey) {
      const cloud = cloudByKey.get(key);
      if (!cloud) {
        added.push(local);
      } else if (comparable(cloud) !== comparable(local)) {
        modified.push({ local, cloud });
      }
    }
    const deleted = [];
    for (const [key, cloud] of cloudByKey) {
      if (!localByKey.has(key)) deleted.push(cloud);
    }
    const cloudWorkspaceList = cloudSchema.migrateWorkspaces(cloudWorkspaces);
    const localWorkspaceList = cloudSchema.migrateWorkspaces(localWorkspaces);
    const unassigned = "未分配";
    const addedWorkspaces = localWorkspaceList.filter(
      name => name !== unassigned && !cloudWorkspaceList.includes(name));
    const deletedWorkspaces = cloudWorkspaceList.filter(
      name => name !== unassigned && !localWorkspaceList.includes(name));
    return {
      added, modified, deleted,
      addedWorkspaces, deletedWorkspaces,
      changed: added.length + modified.length + deleted.length
        + addedWorkspaces.length + deletedWorkspaces.length
    };
  };
  // end cloud-diff
  const buildCloudPayload = async () => {
    cloudSyncStatus.textContent = "正在导出连接…";
    const servers = await post("cloud.exportServers");
    cloudSyncStatus.textContent = "正在读取工作区…";
    const workspaces = await post("workspace.list");
    cloudSyncStatus.textContent = "正在读取本地历史…";
    const history = await post("local.history.load");
    return {
      version: cloudSchema.CLOUD_SCHEMA_VERSION,
      savedAt: new Date().toISOString(),
      servers, workspaces, history, settings: appSettings
    };
  };
  const applyCloudData = async rawData => {
    const data = cloudSchema.migrateCloudPayload(rawData);
    if (!data) {
      cloudSyncStatus.textContent = "云端数据格式无法识别，未做任何更改";
      return;
    }
    if (data.tooNew) {
      cloudSyncStatus.textContent =
        `云端数据版本（${data.version}）高于当前客户端支持的版本（`
        + `${cloudSchema.CLOUD_SCHEMA_VERSION}），请升级 MasterTerm 后再下载`;
      return;
    }
    if (Array.isArray(data.servers)) {
      const imported = await post("cloud.importServers", {
        servers: data.servers,
        workspaces: Array.isArray(data.workspaces) ? data.workspaces : []
      });
      const profiles = Array.isArray(imported)
        ? imported : (imported && Array.isArray(imported.profiles)
          ? imported.profiles : []);
      await refreshWorkspaces();
      render(profiles);
      const undecryptable = imported && !Array.isArray(imported)
        ? Number(imported.undecryptableSecrets || 0) : 0;
      if (undecryptable > 0)
        cloudSyncStatus.textContent =
          `下载成功；${undecryptable} 项凭据来自其他设备无法解密，请重新填写密码或私钥`;
    }
    if (Array.isArray(data.history))
      await post("local.history.replace", { commands: data.history });
    if (data.settings && typeof data.settings === "object") {
      appSettings = normalizeAppSettings(data.settings);
      localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
      applyTerminalSettings();
      saveSessionRestore();
    }
  };
  const showCloudDiffDialog = diff => new Promise(resolve => {
    const list = document.querySelector("#cloud-diff-list");
    list.replaceChildren();
    const addSection = (title, items, formatter) => {
      if (!items.length) return;
      const section = document.createElement("div");
      section.className = "cloud-diff-section";
      const heading = document.createElement("strong");
      heading.textContent = title;
      section.appendChild(heading);
      const entries = document.createElement("ul");
      for (const item of items.slice(0, 20)) {
        const entry = document.createElement("li");
        entry.textContent = formatter(item);
        entries.appendChild(entry);
      }
      if (items.length > 20) {
        const more = document.createElement("li");
        more.textContent = `…等 ${items.length} 项`;
        entries.appendChild(more);
      }
      section.appendChild(entries);
      list.appendChild(section);
    };
    addSection(`新增连接（${diff.added.length}）`, diff.added,
      profile => `${profile.name}（${profile.address}）`);
    addSection(`修改连接（${diff.modified.length}）`, diff.modified,
      entry => `${entry.local.name}（${entry.local.address}）`);
    addSection(`云端独有连接（${diff.deleted.length}）`, diff.deleted,
      profile => `${profile.name}（${profile.address}）`);
    addSection(`新增工作区（${diff.addedWorkspaces.length}）`, diff.addedWorkspaces,
      name => name);
    addSection(`云端独有工作区（${diff.deletedWorkspaces.length}）`, diff.deletedWorkspaces,
      name => name);
    document.querySelector("#cloud-diff-summary").textContent =
      `与云端数据相比：新增 ${diff.added.length} 个连接、修改 ${diff.modified.length} 个、`
      + `云端独有 ${diff.deleted.length} 个连接；新增工作区 ${diff.addedWorkspaces.length} 个、`
      + `云端独有工作区 ${diff.deletedWorkspaces.length} 个。`;
    cloudDiffDialog.showModal();
    let resolved = false;
    const finish = choice => {
      if (resolved) return;
      resolved = true;
      cloudDiffDialog.close();
      resolve(choice);
    };
    document.querySelector("#cloud-diff-cancel").onclick = () => finish("cancel");
    document.querySelector("#cloud-diff-merge").onclick = () => finish("merge");
    document.querySelector("#cloud-diff-overwrite").onclick = () => finish("overwrite");
    document.querySelector("#cloud-diff-close").onclick = () => finish("cancel");
    cloudDiffDialog.addEventListener("cancel", () => finish("cancel"), { once: true });
  });
  const cloudPush = async () => {
    if (!cloudState.token) {
      cloudSyncStatus.textContent = "请先登录再上传";
      return;
    }
    cloudSyncStatus.textContent = "正在准备上传…";
    try {
      // 检查云端快照配额（普通用户最多保留 3 个备份版本）
      try {
        const historyData = await post("cloud.history", {
          url: cloudState.url, token: cloudState.token
        });
        const snapshots = Array.isArray(historyData?.snapshots) ? historyData.snapshots : [];
        if (snapshots.length >= 3) {
          cloudSyncStatus.textContent = "云端备份已达上限（3/3），请在【历史版本】中删除旧版本";
          if (await requestConfirmation(
            "云端备份配额已满",
            "公益备份服务限制每个普通用户最多保留 3 个历史备份版本。\n\n"
              + `当前账号已有 ${snapshots.length} 个历史版本，无法继续创建新备份。\n\n`
              + "是否现在前往【历史版本】管理和删除旧版本？", "前往管理")) {
            await openCloudHistory();
          }
          return;
        }
      } catch {
        // 若查询历史失败，不阻断主流程，由服务端校验最终拦截
      }
      const payload = await buildCloudPayload();
      let remote = null;
      try {
        remote = await post("cloud.pull", {
          url: cloudState.url, token: cloudState.token
        });
      } catch (error) {
        // 服务器不可达时由最终的上传请求报错；预览仅在有云端数据时进行。
      }
      if (remote && remote.data && typeof remote.data === "object") {
        const diff = computeCloudDiff(
          remote.data.servers, remote.data.workspaces,
          payload.servers, payload.workspaces);
        if (diff.changed > 0) {
          const choice = await showCloudDiffDialog(diff);
          if (choice === "cancel") {
            cloudSyncStatus.textContent = "已取消上传";
            return;
          }
          if (choice === "merge") {
            // 保留云端独有的连接和工作区，避免覆盖另一台设备的数据
            payload.servers = payload.servers.concat(diff.deleted);
            payload.workspaces = [
              ...new Set(payload.workspaces.concat(diff.deletedWorkspaces))
            ];
          }
        }
      }
      cloudSyncStatus.textContent = "正在上传到服务器…";
      const result = await post("cloud.push", {
        url: cloudState.url, token: cloudState.token,
        payload: JSON.stringify(payload)
      });
      cloudState.updatedAt = result.updatedAt || "";
      saveCloudState();
      cloudSyncStatus.textContent = "上传成功";
    } catch (error) {
      handleCloudError(error);
    }
  };
  const cloudPull = () => {
    if (!cloudState.token) {
      cloudSyncStatus.textContent = "请先登录再下载";
      return;
    }
    cloudSyncStatus.textContent = "正在下载…";
    post("cloud.pull", { url: cloudState.url, token: cloudState.token })
      .then(result => {
        if (result.data == null) {
          cloudSyncStatus.textContent = "云端暂无数据，请先上传";
          return;
        }
        return applyCloudData(result.data).then(() => {
          cloudState.updatedAt = result.updatedAt || "";
          saveCloudState();
          cloudSyncStatus.textContent = "下载并应用成功";
        });
      })
      .catch(error => {
        handleCloudError(error);
      });
  };
  cloudRegisterButton.addEventListener("click", () => cloudAuth(true));
  cloudLoginButton.addEventListener("click", () => cloudAuth(false));
  cloudLogoutButton.addEventListener("click", () => {
    cloudState = { url: cloudState.url, username: "", token: "", updatedAt: "" };
    saveCloudState();
    cloudSyncStatus.textContent = "已退出登录";
  });
  cloudSyncPassword?.addEventListener("keydown", event => {
    if (event.key === "Enter" && !cloudLoginButton.disabled) {
      cloudAuth(false);
    }
  });
  cloudPushButton.addEventListener("click", cloudPush);
  cloudPullButton.addEventListener("click", cloudPull);
  // ---- 云端历史版本（T2-4）----
  const cloudHistorySeq = snapshot => {
    const seq = Number(snapshot?.seq);
    return Number.isSafeInteger(seq) && seq > 0 ? seq : null;
  };
  const renderCloudHistory = snapshots => {
    const quotaEl = document.querySelector("#cloud-history-quota");
    if (quotaEl) {
      quotaEl.textContent = `备份配额：${snapshots.length} / 3 个（公益服务限制普通用户上限 3 个${snapshots.length >= 3 ? "，已达上限，需删除旧版本后方可继续备份" : ""}）`;
      quotaEl.style.color = snapshots.length >= 3 ? "#f87171" : "#94a3b8";
    }
    cloudHistoryList.replaceChildren();
    cloudHistoryEmpty.hidden = snapshots.length > 0;
    for (const snapshot of snapshots) {
      const row = document.createElement("div");
      row.className = "cloud-history-row";
      const info = document.createElement("div");
      info.className = "cloud-history-info";
      const name = document.createElement("strong");
      name.textContent = String(snapshot.name || `版本 ${snapshot.seq}`);
      const detail = document.createElement("small");
      detail.textContent = `版本 ${snapshot.seq} · ${snapshot.updatedAt || "未知时间"}`;
      info.append(name, detail);
      const actions = document.createElement("div");
      actions.className = "cloud-history-actions";
      const restore = document.createElement("button");
      restore.type = "button";
      restore.className = "settings-inline-button";
      restore.textContent = "恢复此版本";
      restore.addEventListener("click", async () => {
        const seq = cloudHistorySeq(snapshot);
        if (seq === null) {
          cloudHistoryStatus.textContent = "云端版本序号无效，请刷新历史版本列表";
          return;
        }
        if (!await requestConfirmation(
          "恢复云端版本",
          `确定用版本 ${snapshot.seq}（${snapshot.updatedAt || "未知时间"}）替换当前云端数据吗？\n\n`
            + "恢复后请点击“从云端下载”应用到本机。", "恢复")) {
          return;
        }
        cloudHistoryStatus.textContent = "正在恢复…";
        try {
          const result = await post("cloud.restore", {
            url: cloudState.url, token: cloudState.token,
            seq
          });
          cloudState.updatedAt = result.updatedAt || "";
          saveCloudState();
          cloudHistoryStatus.textContent = "恢复成功，请下载应用到本机";
          openCloudHistory().catch(reportError);
        } catch (error) {
          cloudHistoryStatus.textContent =
            error instanceof Error ? error.message : String(error);
        }
      });
      const rename = document.createElement("button");
      rename.type = "button";
      rename.className = "settings-inline-button";
      rename.textContent = "改名";
      rename.addEventListener("click", async () => {
        const seq = cloudHistorySeq(snapshot);
        if (seq === null) {
          cloudHistoryStatus.textContent = "云端版本序号无效，请刷新历史版本列表";
          return;
        }
        const nextName = await requestText(
          "重命名云端版本",
          "请输入版本名称（留空可恢复为默认名称）：",
          String(snapshot.name || ""));
        if (nextName === null) return;
        cloudHistoryStatus.textContent = "正在保存名称…";
        try {
          await post("cloud.history.rename", {
            url: cloudState.url, token: cloudState.token,
            seq, name: nextName.trim()
          });
          cloudHistoryStatus.textContent = "名称已更新";
          await openCloudHistory();
        } catch (error) {
          cloudHistoryStatus.textContent =
            error instanceof Error ? error.message : String(error);
        }
      });
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "settings-inline-button danger";
      remove.textContent = "删除";
      remove.addEventListener("click", async () => {
        const seq = cloudHistorySeq(snapshot);
        if (seq === null) {
          cloudHistoryStatus.textContent = "云端版本序号无效，请刷新历史版本列表";
          return;
        }
        if (!await requestConfirmation(
          "删除云端版本",
          `确定删除“${name.textContent}”吗？删除后无法恢复。`, "删除")) return;
        cloudHistoryStatus.textContent = "正在删除…";
        try {
          await post("cloud.history.delete", {
            url: cloudState.url, token: cloudState.token,
            seq
          });
          cloudHistoryStatus.textContent = "版本已删除";
          await openCloudHistory();
        } catch (error) {
          cloudHistoryStatus.textContent =
            error instanceof Error ? error.message : String(error);
        }
      });
      actions.append(restore, rename, remove);
      row.append(info, actions);
      cloudHistoryList.appendChild(row);
    }
  };
  const openCloudHistory = async () => {
    cloudHistoryStatus.textContent = "";
    cloudHistoryList.replaceChildren();
    cloudHistoryEmpty.hidden = true;
    if (!cloudHistoryDialog.open) cloudHistoryDialog.showModal();
    try {
      const result = await post("cloud.history", {
        url: cloudState.url, token: cloudState.token
      });
      renderCloudHistory(Array.isArray(result.snapshots) ? result.snapshots : []);
    } catch (error) {
      cloudHistoryStatus.textContent =
        error instanceof Error ? error.message : String(error);
    }
  };
  document.querySelector("#cloud-history").addEventListener(
    "click", () => openCloudHistory().catch(reportError));
  document.querySelector("#cloud-history-done").addEventListener(
    "click", () => cloudHistoryDialog.close());
  document.querySelector("#cloud-history-close").addEventListener(
    "click", () => cloudHistoryDialog.close());
  updateCloudSyncUi();

  const updateCheckStatus = document.querySelector("#update-check-status");
  let updateProgressHandler = null;
  const showUpdateDialog = payload => {
    if (!payload || !payload.hasUpdate || !payload.latestVersion) return;
    showTransientStatus(
      `发现新版本 ${payload.latestVersion}，可下载更新`);
    status.title = payload.url || "";
    const summary = String(payload.body || "（发布方未提供变更说明）").trim();
    const downloadable = Boolean(payload.url && payload.sha256);
    const buttons = [
      { action: "later", label: "稍后再说" },
      ...(downloadable
        ? [{ action: "download", label: "下载并更新", primary: true }]
        : payload.url
          ? [{ action: "open", label: "前往发布页面", primary: true }]
          : [])
    ];
    showActionDialog({
      title: `发现新版本 ${payload.latestVersion}`,
      message:
        `当前版本：${payload.currentVersion || "—"}\n`
        + `最新版本：${payload.latestVersion}\n\n`
        + `变更摘要：\n${summary.length > 3000 ? `${summary.slice(0, 3000)}\n…（已截断）` : summary}`
        + (downloadable ? "\n\n更新将自动下载并校验，安装后程序自动重启。" : ""),
      buttons
    }).then(choice => {
      if (choice.action === "open" && payload.url)
        post("shell.openUrl", { url: payload.url }).catch(reportError);
      else if (choice.action === "download")
        downloadAndInstallUpdate(payload.url, payload.sha256);
    });
  };
  const downloadAndInstallUpdate = (url, sha256) => {
    if (!url) return;
    showTransientStatus("正在下载更新…", 8000);
    let downloadPath = "";
    let progressDialog = null;
    let progressClosed = false;
    const dialogMessage = document.querySelector("#action-dialog-message");
    const dialogActions = document.querySelector("#action-dialog-actions");
    const closeProgress = () => {
      if (progressClosed) return;
      progressClosed = true;
      updateProgressHandler = null;
      if (progressDialog && progressDialog.open)
        progressDialog.close();
    };
    updateProgressHandler = payload => {
      if (progressClosed || !progressDialog || !progressDialog.open) return;
      const done = Number(payload.done || 0);
      const total = Number(payload.total || 0);
      const percent = total > 0 ? Math.floor(done * 100 / total) : 0;
      if (dialogMessage) {
        dialogMessage.textContent =
          `正在下载更新包… ${percent}%（${formatSize(done)} / ${total > 0 ? formatSize(total) : "—"}）`;
      }
      if (progressBar) progressBar.style.width = `${percent}%`;
    };
    showActionDialog({
      title: "正在下载更新",
      message: "正在下载更新包… 0%",
      buttons: []
    });
    progressDialog = actionDialog;
    // Replace the empty action row with a progress bar.
    const progressBar = document.createElement("div");
    progressBar.className = "update-progress-bar";
    if (dialogActions) {
      dialogActions.replaceChildren();
      const track = document.createElement("div");
      track.className = "update-progress-track";
      track.appendChild(progressBar);
      dialogActions.appendChild(track);
    }
    post("update.download", { url, sha256 })
      .then(result => {
        closeProgress();
        downloadPath = result.path || "";
        return requestConfirmation(
          "更新已就绪",
          `更新包已下载并校验（${result.size > 0 ? formatSize(result.size) : "—"}）。\n`
          + "安装过程中程序会自动退出并在完成后重新启动。\n确定现在安装吗？",
          "立即安装");
      })
      .then(confirmed => {
        if (!confirmed) {
          showTransientStatus("已取消安装，更新包保留在临时目录");
          return;
        }
        if (!downloadPath) throw new Error("更新包路径无效");
        return post("update.install", { path: downloadPath })
          .then(() => post("app.closeDecision", { decision: "exit" }));
      })
      .catch(error => {
        closeProgress();
        showTransientStatus(error instanceof Error ? error.message : String(error));
      });
  };
  const checkUpdateNow = () => {
    updateCheckStatus.textContent = "正在检查…";
    postWithTimeout("update.check", { url: appSettings.updateServerUrl || updateServerDefaultUrl })
      .then(result => {
        if (result && result.hasUpdate) {
          showUpdateDialog(result);
          updateCheckStatus.textContent = "发现新版本";
        } else if (result && result.status > 0 && result.debugBody) {
          updateCheckStatus.textContent = `更新源响应异常：${String(result.debugBody).slice(0, 80)}`;
        } else if (result && result.status > 0) {
          updateCheckStatus.textContent = `已是最新版本（当前 ${result.currentVersion || "?"}，源版本 ${result.latestVersion || "?"}，HTTP ${result.status}）`;
        } else {
          updateCheckStatus.textContent = "检查失败（无法连接更新源）";
        }
      })
      .catch(() => { updateCheckStatus.textContent = "检查失败"; });
  };
  const checkUpdateButton = document.querySelector("#check-update-now");
  if (checkUpdateButton)
    checkUpdateButton.addEventListener("click", checkUpdateNow);
  const saveSettingsFromForm = () => {
    const numberValue = (selector, minimum, maximum, fallback) => {
      const value = Number(document.querySelector(selector).value);
      return Number.isFinite(value) ? Math.min(maximum, Math.max(minimum, value)) : fallback;
    };
    const activeTerminalTheme = themes[activeTheme].terminal;
    const background = document.querySelector("#setting-terminal-background").value;
    const foreground = document.querySelector("#setting-terminal-foreground").value;
    appSettings = {
      localPanelOnOpen: document.querySelector("#setting-local-panel").checked,
      confirmDelete: document.querySelector("#setting-confirm-delete").checked,
      restoreSessions: document.querySelector("#setting-restore-sessions").checked,
      sessionLogging: document.querySelector("#setting-session-logging").checked,
      keepalive: numberValue("#setting-keepalive", 0, 3600, defaultSettings.keepalive),
      autoReconnect: document.querySelector("#setting-auto-reconnect").checked,
      reconnectAttempts: numberValue("#setting-reconnect-attempts", 0, 20, defaultSettings.reconnectAttempts),
      remoteEditConflict: document.querySelector("#setting-remote-edit-conflict").value,
      fontSize: numberValue("#setting-font-size", 8, 32, defaultSettings.fontSize),
      fontFamily: document.querySelector("#setting-font-family").value || terminalFontName(defaultSettings.fontFamily),
      scrollback: numberValue("#setting-scrollback", 100, 50000, defaultSettings.scrollback),
      terminalBackground: background.toLowerCase() === activeTerminalTheme.background ? "" : background,
      terminalForeground: foreground.toLowerCase() === activeTerminalTheme.foreground ? "" : foreground,
      historyLimit: numberValue("#setting-history-limit", 0, 5000, defaultSettings.historyLimit),
      completionLimit: numberValue("#setting-completion-limit", 1, 50, defaultSettings.completionLimit),
      historyDays: numberValue("#setting-history-days", 0, 3650, defaultSettings.historyDays),
      readBashHistory: document.querySelector("#setting-read-bash").checked,
      readZshHistory: document.querySelector("#setting-read-zsh").checked,
      deduplicateHistory: document.querySelector("#setting-history-deduplicate").checked,
      copyOnSelect: document.querySelector("#setting-copy-on-select").checked,
      cursorStyle: document.querySelector("#setting-cursor-style").value || defaultSettings.cursorStyle,
      cursorBlink: document.querySelector("#setting-cursor-blink").checked,
      alertsEnabled: document.querySelector("#setting-alert-enabled").checked,
      alertCpuThreshold: numberValue("#setting-alert-cpu", 1, 100, defaultSettings.alertCpuThreshold),
      alertMemoryThreshold: numberValue("#setting-alert-memory", 1, 100, defaultSettings.alertMemoryThreshold),
      alertDiskThreshold: numberValue("#setting-alert-disk", 1, 100, defaultSettings.alertDiskThreshold),
      serverSortMode: serverSortMode?.value || defaultSettings.serverSortMode,
      serverGroupMode: serverGroupMode?.value || defaultSettings.serverGroupMode,
      connectionTypeColorSsh: document.querySelector("#setting-connection-color-ssh").value || defaultSettings.connectionTypeColorSsh,
      connectionTypeColorRdp: document.querySelector("#setting-connection-color-rdp").value || defaultSettings.connectionTypeColorRdp,
      connectionTypeColorSerial: document.querySelector("#setting-connection-color-serial").value || defaultSettings.connectionTypeColorSerial,
      defaultLocalShell: document.querySelector("#setting-default-local-shell")?.value || defaultSettings.defaultLocalShell,
      localWorkingDir: document.querySelector("#setting-local-working-dir")?.value || defaultSettings.localWorkingDir,
      terminalThemePreset: document.querySelector("#setting-terminal-theme-preset")?.value || defaultSettings.terminalThemePreset,
      updateServerUrl: appSettings.updateServerUrl || updateServerDefaultUrl,
      editorAssociations: appSettings.editorAssociations || {}
    };
    localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
    updateTerminalTabTypeColors();
    post("logs.configure", { enabled: appSettings.sessionLogging }).catch(() => {});
    saveSessionRestore();
    setLocalPanelVisible(appSettings.localPanelOnOpen);
    applyTerminalSettings();
    render(profilesCache);
    const closeBehavior = document.querySelector("#setting-close-behavior").value;
    post("app.setCloseBehavior", { behavior: closeBehavior }).catch(reportError);
  };
  const requestDeleteConfirmation = (title, message, confirmLabel = "删除") =>
    appSettings.confirmDelete ? requestConfirmation(title, message, confirmLabel) : Promise.resolve(true);

  const refreshSerialPorts = async (selectedPort = "") => {
    serialPortRefreshButton.disabled = true;
    serialPortStatus.textContent = "正在读取可用串口…";
    try {
      const ports = await post("serial.list");
      serialPortSelect.replaceChildren();
      if (!Array.isArray(ports) || !ports.length) {
        const option = new Option("未检测到已连接的串口", "");
        option.disabled = true;
        option.selected = true;
        serialPortSelect.appendChild(option);
        serialPortStatus.textContent = "未检测到可用串口，请连接设备后刷新。";
        return;
      }
      ports.forEach(port => serialPortSelect.appendChild(new Option(port, port)));
      if (selectedPort && !ports.includes(selectedPort)) {
        const missing = new Option(`${selectedPort}（当前未连接）`, selectedPort);
        missing.disabled = true;
        serialPortSelect.appendChild(missing);
      }
      serialPortSelect.value = ports.includes(selectedPort) ? selectedPort : ports[0];
      serialPortStatus.textContent = `已检测到 ${ports.length} 个串口。`;
    } catch (error) {
      serialPortSelect.replaceChildren(new Option("读取串口失败", ""));
      serialPortStatus.textContent = error instanceof Error ? error.message : String(error);
    } finally {
      serialPortRefreshButton.disabled = false;
    }
  };

  window.chrome?.webview?.addEventListener("message", event => {
    const response = typeof event.data === "string" ? JSON.parse(event.data) : event.data;
    if (response.event) {
      handleEvent(response);
      return;
    }
    const request = pending.get(response.id);
    if (!request) return;
    pending.delete(response.id);
    response.ok ? request.resolve(response.result) : request.reject(new Error(response.error));
  });

  const updateSessionState = (session, state) => {
    session.state = state;
    session.tab.dataset.state = state;
    session.disconnectButton.disabled = state === "closed" || state === "error";
    refreshSidebarSftpSessions();
  };

  const showRdpDisconnectNotice = (session, reason) => {
    if (!session?.rdpDisconnectNotice) return;
    session.rdpDisconnectReason.textContent = reason;
    session.rdpDisconnectNotice.hidden = false;
    session.tab.removeAttribute("title");
    session.tab.title = "";

    if (session.rdpAutoReconnectTimer) {
      clearInterval(session.rdpAutoReconnectTimer);
      session.rdpAutoReconnectTimer = 0;
    }

    if (!session.rdpCountdownBadge) {
      session.rdpCountdownBadge = document.createElement("div");
      session.rdpCountdownBadge.className = "rdp-reconnect-countdown";
      const content = session.rdpDisconnectNotice.querySelector(".rdp-disconnect-content");
      const actions = session.rdpDisconnectNotice.querySelector(".rdp-disconnect-actions");
      if (content && actions) {
        content.insertBefore(session.rdpCountdownBadge, actions);
      }
    }

    // 1. 冲突判断：当会话是由另一台设备接入、远端注销、本地主动断开、或特定退出码引起时，绝对禁止自动重连
    const isConflictOrLogoff = (
      session.closing ||
      session.manualDisconnect ||
      reason.includes("另一处设备") ||
      reason.includes("另一登录会话") ||
      reason.includes("另一连接") ||
      reason.includes("接管") ||
      reason.includes("注销") ||
      reason.includes("错误码 3") ||
      reason.includes("错误码 264") ||
      reason.includes("错误码 3334") ||
      reason.includes("错误码 1") ||
      reason.includes("错误码 2")
    );

    if (isConflictOrLogoff) {
      if (session.profileIndex >= 0) {
        rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
      }
      session.rdpCountdownBadge.hidden = false;
      session.rdpCountdownBadge.textContent = "已停止自动重连（检测到会话被其它设备接管或已正常退出）。如需重新连接请点击下方“重新连接”。";
      return;
    }

    // 2. 检查是否开启了自动重连：服务器单独配置优先，为 -1 或未配置时跟随全局设置
    const profile = session.profileIndex >= 0 ? profilesByIndex.get(session.profileIndex) : null;
    const rdpAutoReconnectOpt = profile?.rdpOptions?.autoReconnect;
    const autoReconnectEnabled = (rdpAutoReconnectOpt === 1 || rdpAutoReconnectOpt === "1") ||
      ((rdpAutoReconnectOpt == null || rdpAutoReconnectOpt === -1 || rdpAutoReconnectOpt === "-1") && appSettings.autoReconnect);

    if (!autoReconnectEnabled || session.profileIndex < 0) {
      if (session.profileIndex >= 0) {
        rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
      }
      session.rdpCountdownBadge.hidden = true;
      return;
    }

    // 3. 确为意外掉线且用户启用了重连时，执行带重试上限的重连倒计时
    const maxAttempts = Math.min(20, Math.max(1, Number(profile?.rdpOptions?.maxReconnectAttempts) || appSettings.reconnectAttempts || 3));
    const profileIndex = session.profileIndex;
    const currentAttempts = (rdpAutoReconnectAttemptsByProfile.get(profileIndex) || 0) + 1;
    rdpAutoReconnectAttemptsByProfile.set(profileIndex, currentAttempts);

    if (currentAttempts <= maxAttempts) {
      let remainingSeconds = 5;
      session.rdpCountdownBadge.hidden = false;
      session.rdpCountdownBadge.textContent =
        `将在 ${remainingSeconds} 秒后自动重试连接 (第 ${currentAttempts}/${maxAttempts} 次)...`;

      session.rdpAutoReconnectTimer = setInterval(() => {
        remainingSeconds--;
        if (remainingSeconds > 0) {
          session.rdpCountdownBadge.textContent =
            `将在 ${remainingSeconds} 秒后自动重试连接 (第 ${currentAttempts}/${maxAttempts} 次)...`;
        } else {
          clearInterval(session.rdpAutoReconnectTimer);
          session.rdpAutoReconnectTimer = 0;
          session.rdpCountdownBadge.textContent = `正在自动发起第 ${currentAttempts}/${maxAttempts} 次重连…`;
          const wasActive = (activeSession === session);
          reconnectSession(session.sessionId, { keepBackground: !wasActive }).catch(reportError);
        }
      }, 1000);
    } else {
      session.rdpCountdownBadge.hidden = false;
      session.rdpCountdownBadge.textContent = `已达到最大自动重试次数 (${maxAttempts}次)，请手动重新连接。`;
    }
  };

  const hideRdpDisconnectNotice = (session, resetAttempts = false) => {
    if (!session?.rdpDisconnectNotice) return;
    if (session.rdpAutoReconnectTimer) {
      clearInterval(session.rdpAutoReconnectTimer);
      session.rdpAutoReconnectTimer = 0;
    }
    if (session.rdpCountdownBadge) {
      session.rdpCountdownBadge.hidden = true;
    }
    if (resetAttempts && session.profileIndex >= 0) {
      rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
    }
    session.rdpDisconnectNotice.hidden = true;
    session.tab.removeAttribute("title");
    session.tab.title = "";
  };

  const saveCardLayout = () => {
    const cards = [...workspace.querySelectorAll(".work-card")].map(card => ({
      id: card.dataset.card,
      basis: card.style.flexBasis || ""
    }));
    localStorage.setItem("masterterm.cardLayout", JSON.stringify({
      mode: workspace.dataset.layout || "columns",
      viewportWidth: workspace.clientWidth,
      cards
    }));
  };

  const setCardLayoutMode = mode => {
    const supported = new Set(["columns", "left-stack", "right-stack"]);
    const normalized = supported.has(mode) ? mode : "columns";
    workspace.dataset.layout = normalized;
    workspace.classList.toggle("layout-left-stack", normalized === "left-stack");
    workspace.classList.toggle("layout-right-stack", normalized === "right-stack");
  };

  const sidebarFlyoutWidthStorageKey = panel =>
    `masterterm.sidebarFlyoutWidth.${panel === "sftp" ? "sftp" : "connections"}`;

  const maximumSidebarWidth = floating => Math.max(300,
    workspace.clientWidth - 42 - (floating ? 96 : 326));

  const activeSidebarWidth = floating => {
    const property = floating ? "--sidebar-flyout-width" : "--sidebar-width";
    const value = Number.parseFloat(getComputedStyle(workspace)
      .getPropertyValue(property));
    if (Number.isFinite(value) && value > 0) return value;
    return document.querySelector("#function-panels").getBoundingClientRect().width;
  };

  const applySidebarFlyoutWidth = () => {
    const fallbacks = {
      sftp: 460,
      tunnels: 400,
      snippets: 360,
      cloud: 360,
      connections: 340
    };
    const fallback = fallbacks[activeFunctionPanel] || 340;
    const saved = Number(localStorage.getItem(
      sidebarFlyoutWidthStorageKey(activeFunctionPanel)));
    let width = Number.isFinite(saved) && saved >= 300 ? saved : fallback;
    if (activeFunctionPanel === "sftp" && localPanelVisible) {
      const expanded = Math.min(Math.max(720, workspace.clientWidth * .62),
        maximumSidebarWidth(true));
      width = Math.max(width, expanded);
    }
    workspace.style.setProperty("--sidebar-flyout-width",
      `${Math.round(Math.min(width, maximumSidebarWidth(true)))}px`);
  };

  const connectedSftpSessions = () => Array.from(sessions.values())
    .filter(session => session.connectionType === "ssh"
      && session.state === "connected");

  const refreshSidebarSftpSessions = () => {
    if (!sidebarSftpSession) return;
    const available = connectedSftpSessions();
    sidebarSftpSession.replaceChildren();
    if (!available.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "没有已连接的 SSH";
      sidebarSftpSession.appendChild(option);
      sidebarSftpSession.disabled = true;
    } else {
      available.forEach(session => {
        const profile = profilesByIndex.get(session.profileIndex);
        const option = document.createElement("option");
        option.value = session.sessionId;
        option.textContent = profile?.name || session.name
          || profile?.address || "SSH";
        sidebarSftpSession.appendChild(option);
      });
      sidebarSftpSession.disabled = false;
      const preferred = available.some(session =>
        session.sessionId === activeSftpSessionId)
        ? activeSftpSessionId
        : available.some(session => session.sessionId === focusedSessionId)
          ? focusedSessionId : available[0].sessionId;
      sidebarSftpSession.value = preferred;
    }
    sidebarSftpSession.hidden = activeFunctionPanel !== "sftp";
    refreshSidebarTunnelSessions?.();
  };

  const updateSidebarPanelHeader = () => {
    if (sidebarPanelTitle) {
      const titles = {
        connections: "连接",
        sftp: "SFTP",
        snippets: "常用命令",
        tunnels: "端口转发",
        cloud: "云备份"
      };
      sidebarPanelTitle.textContent = titles[activeFunctionPanel] || "连接";
    }
    refreshSidebarSftpSessions();
  };

  const updateSidebarPinUi = () => {
    workspace.classList.toggle("sidebar-auto-hide", sidebarAutoHide);
    if (sidebarCardShadow)
      sidebarCardShadow.hidden = sidebarAutoHide
        && !workspace.classList.contains("sidebar-flyout-open");
    if (!sidebarPin) return;
    sidebarPin.setAttribute("aria-pressed", String(!sidebarAutoHide));
    sidebarPin.title = sidebarAutoHide ? "固定侧栏" : "启用自动隐藏";
    sidebarPin.setAttribute("aria-label", sidebarPin.title);
    const titles = {
      connections: "连接",
      sftp: "SFTP",
      snippets: "常用命令",
      tunnels: "端口转发",
      cloud: "云备份"
    };
    functionTabs.querySelectorAll(".function-tab").forEach(button => {
      const name = titles[button.dataset.panel] || "连接";
      button.title = `${name}（双击切换自动隐藏）`;
      button.setAttribute("aria-label", button.title);
    });
  };

  const sidebarInteractionLocked = () => {
    const focused = document.activeElement;
    return Boolean(functionPanels?.contains(focused)
      || sidebarResizer?.classList.contains("resizing")
      || functionPanels?.querySelector(".resizing,.drag-over")
      || document.querySelector("dialog[open],.context-menu:not([hidden]),"
        + ".terminal-create-dropdown:not([hidden])"));
  };

  const clearSidebarFlyoutTimers = () => {
    if (sidebarFlyoutOpenTimer) clearTimeout(sidebarFlyoutOpenTimer);
    if (sidebarFlyoutCloseTimer) clearTimeout(sidebarFlyoutCloseTimer);
    sidebarFlyoutOpenTimer = 0;
    sidebarFlyoutCloseTimer = 0;
  };

  const showSidebarFlyout = (immediate = false) => {
    if (!sidebarAutoHide || rdpFullscreen) return;
    clearSidebarFlyoutTimers();
    if (workspace.classList.contains("sidebar-flyout-open")) return;
    const generation = ++sidebarFlyoutGeneration;
    const nativeRdp = activeRdpVisible();
    workspace.classList.toggle("sidebar-flyout-native-rdp", nativeRdp);
    if (!nativeRdp) {
      sidebarFlyoutOcclusion.hidden = false;
      sidebarCardShadow.hidden = false;
      workspace.classList.add("sidebar-flyout-open");
      return;
    }

    // WebView2 and the embedded RDP HWND are composed independently. Keep the
    // native desktop intact while the flyout is painted behind it, then punch
    // the RDP hole on the next painted frame. A CSS slide here would expose
    // the WebView canvas while the native clipping region catches up.
    workspace.classList.add("sidebar-flyout-open");
    requestAnimationFrame(() => requestAnimationFrame(() => {
      if (generation !== sidebarFlyoutGeneration || !sidebarAutoHide
          || !workspace.classList.contains("sidebar-flyout-open")) return;
      sidebarFlyoutOcclusion.hidden = false;
      sidebarCardShadow.hidden = false;
      notifyRdpLayout(false);
    }));
  };

  const resumeSidebarFlyout = () => {
    if (!workspace.classList.contains("sidebar-flyout-open")) return false;
    clearSidebarFlyoutTimers();
    // A pointer can re-enter while the native clear request is in flight.
    // Invalidate its completion token and re-open the RDP hole; otherwise the
    // old Promise closes the panel under the pointer and leaves no new enter
    // event to show it again.
    if (sidebarFlyoutOcclusion.hidden) {
      ++sidebarFlyoutGeneration;
      workspace.classList.toggle(
        "sidebar-flyout-native-rdp", activeRdpVisible());
      sidebarFlyoutOcclusion.hidden = false;
      sidebarCardShadow.hidden = false;
      notifyRdpLayout(false);
    }
    return true;
  };

  const hideSidebarFlyout = (immediate = false) => {
    clearSidebarFlyoutTimers();
    const generation = ++sidebarFlyoutGeneration;
    const nativeRdp = workspace.classList.contains("sidebar-flyout-native-rdp")
      || activeRdpVisible();
    if (nativeRdp) {
      // First remove the native hole while the already-painted sidebar still
      // fills it. The host acknowledges after SetWindowRgn has restored the
      // RDP surface; only then remove the HTML panel. This avoids both the
      // residual black rectangle and the two-stage closing animation.
      workspace.classList.add("sidebar-flyout-native-rdp");
      sidebarFlyoutOcclusion.hidden = true;
      sidebarCardShadow.hidden = true;
      Promise.resolve(notifyRdpLayout(false, true)).finally(() => {
        if (generation !== sidebarFlyoutGeneration) return;
        workspace.classList.remove(
          "sidebar-flyout-open", "sidebar-flyout-native-rdp");
      });
      return;
    }

    workspace.classList.remove("sidebar-flyout-open");
    const finish = () => {
      sidebarFlyoutCloseTimer = 0;
      sidebarFlyoutOcclusion.hidden = true;
      sidebarCardShadow.hidden = true;
      workspace.classList.remove("sidebar-flyout-native-rdp");
    };
    if (immediate) finish();
    else sidebarFlyoutCloseTimer = setTimeout(finish, 145);
  };

  const scheduleSidebarFlyoutOpen = (panel = activeFunctionPanel,
                                      immediate = false) => {
    if (!sidebarAutoHide || rdpFullscreen) return;
    if (sidebarFlyoutOpenTimer) clearTimeout(sidebarFlyoutOpenTimer);
    const activateAndShow = () => {
      sidebarFlyoutOpenTimer = 0;
      if (panel && panel !== activeFunctionPanel)
        activateFunctionPanel(panel);
      if (resumeSidebarFlyout()) return;
      showSidebarFlyout(immediate);
    };
    if (immediate) {
      activateAndShow();
      return;
    }
    // Pointerover fires for every icon crossed. Defer both the expensive SFTP
    // panel swap and the native RDP region update until the pointer settles,
    // instead of rebuilding the flyout on every intermediate icon.
    sidebarFlyoutOpenTimer = setTimeout(activateAndShow, 110);
  };

  const scheduleSidebarFlyoutClose = (immediate = false) => {
    if (sidebarFlyoutOpenTimer) {
      clearTimeout(sidebarFlyoutOpenTimer);
      sidebarFlyoutOpenTimer = 0;
    }
    if (!sidebarAutoHide) return;
    if (!workspace.classList.contains("sidebar-flyout-open")) {
      if (!sidebarFlyoutOcclusion.hidden)
        hideSidebarFlyout(true);
      return;
    }
    if (sidebarFlyoutCloseTimer) clearTimeout(sidebarFlyoutCloseTimer);
    sidebarFlyoutCloseTimer = setTimeout(() => {
      sidebarFlyoutCloseTimer = 0;
      if (!immediate && (functionTabs.matches(":hover")
          || functionPanels.matches(":hover") || sidebarInteractionLocked()))
        return;
      hideSidebarFlyout(immediate);
    }, immediate ? 0 : 360);
  };

  const setSidebarAutoHide = (enabled, persist = true) => {
    const changed = sidebarAutoHide !== Boolean(enabled);
    sidebarAutoHide = Boolean(enabled);
    if (persist)
      localStorage.setItem("masterterm.sidebarMode",
        sidebarAutoHide ? "auto" : "docked");
    if (sidebarAutoHide) {
      applySidebarFlyoutWidth();
      workspace.classList.add("sidebar-auto-hide");
      hideSidebarFlyout(true);
    } else {
      hideSidebarFlyout(true);
      workspace.classList.remove("sidebar-auto-hide");
    }
    updateSidebarPinUi();
    if (!changed) return;
    activeSession?.scheduleRefit();
    requestAnimationFrame(() => setTimeout(() => {
      activeSession?.scheduleRefit();
      notifyRdpLayout(true, true, true);
    }, 180));
  };

  const activateFunctionPanel = panel => {
    const validPanels = ["connections", "sftp", "snippets", "tunnels", "cloud"];
    activeFunctionPanel = validPanels.includes(panel) ? panel : "connections";
    if (activeFunctionPanel === "sftp" && !activeSftpSessionId) {
      const available = connectedSftpSessions();
      const target = available.find(session =>
        session.sessionId === focusedSessionId) || available[0];
      const profile = target && profilesByIndex.get(target.profileIndex);
      if (profile) openSftp(profile, target.sessionId);
    }
    if (activeFunctionPanel === "snippets") {
      renderSnippetsPanel?.();
    } else if (activeFunctionPanel === "tunnels") {
      refreshTunnelsPanel?.();
    } else if (activeFunctionPanel === "cloud") {
      updateSidebarCloudUi?.();
    }
    functionTabs.querySelectorAll(".function-tab").forEach(button =>
      button.classList.toggle("active",
        button.dataset.panel === activeFunctionPanel));
    document.querySelectorAll(".function-panel").forEach(item =>
      item.classList.toggle("active",
        item.dataset.panel === activeFunctionPanel));
    localStorage.setItem("masterterm.activeFunctionPanel", activeFunctionPanel);
    applySidebarFlyoutWidth();
    updateSidebarPanelHeader();
    if (sidebarAutoHide) {
      if (workspace.classList.contains("sidebar-flyout-open"))
        notifyRdpLayout(false);
    } else {
      activeSession?.scheduleRefit();
      notifyRdpLayout(false);
    }
  };

  const setLocalPanelVisible = visible => {
    localPanelVisible = visible;
    sftpSplit.classList.toggle("local-visible", visible);
    sftpLocalPane.hidden = !visible;
    sftpPaneDivider.hidden = !visible;
    sftpLocalToggle.classList.toggle("active", visible);
    sftpLocalToggle.setAttribute("aria-pressed", String(visible));
    sftpLocalToggle.title = visible ? "隐藏本机目录" : "显示本机目录";
    sftpLocalToggle.setAttribute("aria-label", sftpLocalToggle.title);
    const panel = document.querySelector("#function-panels");
    if (visible) {
      sidebarWidthBeforeLocal = activeSidebarWidth(sidebarAutoHide);
      const maximum = sidebarAutoHide
        ? maximumSidebarWidth(true)
        : Math.max(520, workspace.clientWidth - 360);
      const target = Math.min(Math.max(720, workspace.clientWidth * .62), maximum);
      workspace.style.setProperty(
        sidebarAutoHide ? "--sidebar-flyout-width" : "--sidebar-width",
        `${Math.max(sidebarWidthBeforeLocal, target)}px`);
      if (!localLoaded) loadLocalDirectory("");
    } else if (sidebarWidthBeforeLocal > 0) {
      workspace.style.setProperty(
        sidebarAutoHide ? "--sidebar-flyout-width" : "--sidebar-width",
        `${Math.round(sidebarWidthBeforeLocal)}px`);
      sidebarWidthBeforeLocal = 0;
    }
    if (sidebarAutoHide) {
      if (workspace.classList.contains("sidebar-flyout-open"))
        notifyRdpLayout(false);
    } else {
      activeSession?.scheduleRefit();
      notifyRdpLayout(true, false, true);
    }
  };

  const installSftpPaneDivider = () => {
    const saved = Number(localStorage.getItem("masterterm.sftpRemotePanePercent"));
    if (Number.isFinite(saved) && saved >= 15 && saved <= 85)
      sftpSplit.style.setProperty("--remote-pane-width", `${saved}%`);
    sftpPaneDivider.addEventListener("pointerdown", event => {
      if (!localPanelVisible) return;
      event.preventDefault();
      const bounds = sftpSplit.getBoundingClientRect();
      sftpPaneDivider.classList.add("resizing");
      sftpPaneDivider.setPointerCapture(event.pointerId);
      const move = moveEvent => {
        const minimum = Math.min(180, Math.max(100, (bounds.width - 5) * .25));
        const width = Math.max(minimum,
          Math.min(bounds.width - 5 - minimum, moveEvent.clientX - bounds.left));
        const percent = width * 100 / Math.max(1, bounds.width - 5);
        sftpSplit.style.setProperty("--remote-pane-width", `${percent}%`);
      };
      const finish = finishEvent => {
        if (sftpPaneDivider.hasPointerCapture(finishEvent.pointerId))
          sftpPaneDivider.releasePointerCapture(finishEvent.pointerId);
        sftpPaneDivider.classList.remove("resizing");
        sftpPaneDivider.removeEventListener("pointermove", move);
        sftpPaneDivider.removeEventListener("pointerup", finish);
        sftpPaneDivider.removeEventListener("pointercancel", finish);
        const remoteWidth = document.querySelector(".remote-pane").getBoundingClientRect().width;
        const totalWidth = Math.max(1, sftpSplit.getBoundingClientRect().width - 5);
        localStorage.setItem("masterterm.sftpRemotePanePercent",
          String(remoteWidth * 100 / totalWidth));
      };
      sftpPaneDivider.addEventListener("pointermove", move);
      sftpPaneDivider.addEventListener("pointerup", finish);
      sftpPaneDivider.addEventListener("pointercancel", finish);
    });
  };

  const clearFixedCardWidths = () => {
    workspace.querySelectorAll(".work-card").forEach(card => {
      card.style.removeProperty("flex");
      card.style.removeProperty("flex-basis");
      card.style.removeProperty("flex-grow");
      card.style.removeProperty("flex-shrink");
      card.style.removeProperty("width");
    });
  };

  const adaptCardLayoutToViewport = () => {
    const cards = [...workspace.querySelectorAll(".work-card")];
    if (!cards.length) return;
    const available = Math.max(0, workspace.clientWidth - 32);
    const fixedWidth = cards.reduce((total, card) => {
      const value = Number.parseFloat(card.style.flexBasis);
      return total + (Number.isFinite(value) ? value : 0);
    }, 0);
    if (workspace.dataset.layout === "columns"
        && (window.innerWidth <= 1050 || fixedWidth > available
            || workspace.scrollWidth > workspace.clientWidth + 12)) {
      clearFixedCardWidths();
    }
    activeSession?.scheduleRefit();
  };

  const restoreCardLayout = () => {
    try {
      const stored = JSON.parse(localStorage.getItem("masterterm.cardLayout") || "[]");
      // Older builds stored only the card array. Keep it as a horizontal layout.
      const cards = Array.isArray(stored) ? stored : (stored.cards || []);
      setCardLayoutMode(Array.isArray(stored) ? "columns" : stored.mode);
      cards.forEach(saved => {
        const card = workspace.querySelector(`[data-card="${saved.id}"]`);
        if (!card) return;
        workspace.appendChild(card);
        if (saved.basis) {
          card.style.flexBasis = saved.basis;
          card.style.flexGrow = "0";
          card.style.flexShrink = "0";
        }
      });
      const savedWidth = Number(Array.isArray(stored) ? 0 : stored.viewportWidth);
      if (savedWidth > 0 && Math.abs(savedWidth - workspace.clientWidth) > 80)
        clearFixedCardWidths();
      requestAnimationFrame(adaptCardLayoutToViewport);
    } catch {
      localStorage.removeItem("masterterm.cardLayout");
      setCardLayoutMode("columns");
    }
  };

  const installCardLayout = () => {
    if (workspace.classList.contains("sidebar-workspace")) {
      const minimumSidebarWidth = 300;
      const savedWidth = Number(localStorage.getItem("masterterm.sidebarWidth"));
      if (Number.isFinite(savedWidth) && savedWidth >= minimumSidebarWidth)
        workspace.style.setProperty("--sidebar-width",
          `${Math.min(savedWidth, maximumSidebarWidth(false))}px`);
      functionTabs.addEventListener("click", event => {
        const button = event.target.closest(".function-tab");
        if (!button) return;
        activateFunctionPanel(button.dataset.panel);
        scheduleSidebarFlyoutOpen(button.dataset.panel, true);
      });
      functionTabs.addEventListener("dblclick", event => {
        const button = event.target.closest(".function-tab");
        if (!button) return;
        event.preventDefault();
        setSidebarAutoHide(!sidebarAutoHide);
      });
      functionTabs.addEventListener("pointerenter", () =>
        scheduleSidebarFlyoutOpen(activeFunctionPanel), true);
      functionTabs.addEventListener("pointerover", event => {
        const button = event.target.closest(".function-tab");
        if (button) scheduleSidebarFlyoutOpen(button.dataset.panel);
      });
      functionTabs.addEventListener("pointerleave", () =>
        scheduleSidebarFlyoutClose());
      functionPanels.addEventListener("pointerenter", clearSidebarFlyoutTimers);
      functionPanels.addEventListener("pointerleave", () =>
        scheduleSidebarFlyoutClose());
      sidebarResizer.addEventListener("pointerenter", clearSidebarFlyoutTimers);
      sidebarResizer.addEventListener("pointerleave", () =>
        scheduleSidebarFlyoutClose());
      functionPanels.addEventListener("focusout", () =>
        setTimeout(() => scheduleSidebarFlyoutClose(), 0));
      sidebarPin?.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        setSidebarAutoHide(!sidebarAutoHide);
      });
      sidebarSftpSession?.addEventListener("change", () => {
        const session = sessions.get(sidebarSftpSession.value);
        const profile = session && profilesByIndex.get(session.profileIndex);
        if (!session || !profile) return;
        openSftp(profile, session.sessionId);
        activateFunctionPanel("sftp");
      });
      sidebarResizer.addEventListener("pointerdown", event => {
        event.preventDefault();
        const startX = event.clientX;
        const panel = document.querySelector("#function-panels");
        const startWidth = panel.getBoundingClientRect().width;
        sidebarResizer.classList.add("resizing");
        sidebarResizer.setPointerCapture(event.pointerId);
        const move = moveEvent => {
          const width = Math.max(minimumSidebarWidth, Math.min(
            maximumSidebarWidth(sidebarAutoHide),
            startWidth + moveEvent.clientX - startX));
          workspace.style.setProperty(
            sidebarAutoHide ? "--sidebar-flyout-width" : "--sidebar-width",
            `${Math.round(width)}px`);
          if (!sidebarAutoHide) activeSession?.scheduleRefit();
          notifyRdpLayout(false);
        };
        const finish = finishEvent => {
          if (sidebarResizer.hasPointerCapture(finishEvent.pointerId))
            sidebarResizer.releasePointerCapture(finishEvent.pointerId);
          sidebarResizer.classList.remove("resizing");
          sidebarResizer.removeEventListener("pointermove", move);
          sidebarResizer.removeEventListener("pointerup", finish);
          sidebarResizer.removeEventListener("pointercancel", finish);
          const width = document.querySelector("#function-panels")
            .getBoundingClientRect().width;
          if (sidebarAutoHide) {
            if (!localPanelVisible)
              localStorage.setItem(
                sidebarFlyoutWidthStorageKey(activeFunctionPanel), String(width));
            notifyRdpLayout(false);
          } else {
            localStorage.setItem("masterterm.sidebarWidth", String(width));
            notifyRdpLayout(true, false, true);
          }
        };
        sidebarResizer.addEventListener("pointermove", move);
        sidebarResizer.addEventListener("pointerup", finish);
        sidebarResizer.addEventListener("pointercancel", finish);
      });
      const transientUiObserver = new MutationObserver(() => {
        if (sidebarAutoHide && workspace.classList.contains("sidebar-flyout-open")
            && !functionTabs.matches(":hover")
            && !functionPanels.matches(":hover")
            && !sidebarInteractionLocked())
          scheduleSidebarFlyoutClose();
      });
      transientUiObserver.observe(document.body, {
        subtree: true,
        attributes: true,
        attributeFilter: ["hidden", "open"]
      });
      document.addEventListener("dragover", event => {
        if (!sidebarAutoHide || event.clientX > 50) return;
        activateFunctionPanel("sftp");
        scheduleSidebarFlyoutOpen("sftp", true);
      });
      document.addEventListener("keydown", event => {
        if (event.key !== "Escape" || !sidebarAutoHide
            || !workspace.classList.contains("sidebar-flyout-open")
            || sidebarInteractionLocked()) return;
        hideSidebarFlyout(true);
      });
      installSftpPaneDivider();
      // The application always starts from the connection list. The active
      // function panel is session state and must not survive a restart.
      localStorage.removeItem("masterterm.activeFunctionPanel");
      setSidebarAutoHide(sidebarAutoHide, false);
      activateFunctionPanel("connections");
      return;
    }
    restoreCardLayout();
    let draggedCard = null;
    const dropClasses = ["drop-target", "drop-left", "drop-right", "drop-top", "drop-bottom"];
    const clearDropFeedback = () => {
      workspace.querySelectorAll(".work-card").forEach(item => {
        item.classList.remove(...dropClasses);
        delete item.dataset.dropZone;
      });
    };
    const dropZoneFor = (event, card) => {
      const bounds = card.getBoundingClientRect();
      const x = (event.clientX - bounds.left) / Math.max(1, bounds.width);
      const y = (event.clientY - bounds.top) / Math.max(1, bounds.height);
      // Horizontal edge zones take precedence. Since dragging starts from a
      // title bar, checking Y first made almost every drop look like "top".
      if (x < 0.28) return "left";
      if (x > 0.72) return "right";
      return y < 0.5 ? "top" : "bottom";
    };
    workspace.querySelectorAll(".work-card").forEach(card => {
      const dragHandle = card.querySelector(".card-header") || card;
      card.draggable = false;
      dragHandle.draggable = true;
      card.addEventListener("dragstart", event => {
        const interactive = event.target.closest(
          "button,input,select,textarea,a,[contenteditable='true']");
        if ((dragHandle !== card && event.target.closest(".card-header") !== dragHandle)
            || (dragHandle === card && interactive)) {
          event.preventDefault();
          return;
        }
        draggedCard = card;
        card.classList.add("dragging");
        event.dataTransfer.effectAllowed = "move";
        event.dataTransfer.setData("text/plain", card.dataset.card);
      });
      card.addEventListener("dragend", () => {
        card.classList.remove("dragging");
        clearDropFeedback();
        draggedCard = null;
        saveCardLayout();
        activeSession?.scheduleRefit();
      });
      card.addEventListener("dragover", event => {
        if (!draggedCard || draggedCard === card) return;
        event.preventDefault();
        clearDropFeedback();
        const zone = dropZoneFor(event, card);
        card.dataset.dropZone = zone;
        card.classList.add("drop-target", `drop-${zone}`);
        event.dataTransfer.dropEffect = "move";
      });
      card.addEventListener("dragleave", event => {
        const bounds = card.getBoundingClientRect();
        const stillInside = event.clientX >= bounds.left && event.clientX <= bounds.right
          && event.clientY >= bounds.top && event.clientY <= bounds.bottom;
        if (!stillInside)
          clearDropFeedback();
      });
      card.addEventListener("drop", event => {
        if (!draggedCard || draggedCard === card) return;
        event.preventDefault();
        const zone = card.dataset.dropZone || dropZoneFor(event, card);
        clearDropFeedback();
        if (zone === "top" || zone === "bottom") {
          const allCards = [...workspace.querySelectorAll(".work-card")];
          const remaining = allCards.find(item => item !== card && item !== draggedCard);
          const stacked = zone === "top" ? [draggedCard, card] : [card, draggedCard];
          const workspaceBounds = workspace.getBoundingClientRect();
          const targetBounds = card.getBoundingClientRect();
          const stackOnLeft =
            targetBounds.left + targetBounds.width / 2 <=
            workspaceBounds.left + workspaceBounds.width / 2;
          const ordered = stackOnLeft ? [...stacked, remaining] : [remaining, ...stacked];
          ordered.filter(Boolean).forEach(item => workspace.appendChild(item));
          setCardLayoutMode(stackOnLeft ? "left-stack" : "right-stack");
        } else {
          workspace.insertBefore(draggedCard, zone === "left" ? card : card.nextSibling);
          setCardLayoutMode("columns");
        }
        saveCardLayout();
        activeSession?.scheduleRefit();
      });

      const resizer = card.querySelector(".card-resizer");
      resizer.addEventListener("pointerdown", event => {
        event.preventDefault();
        event.stopPropagation();
        const nextCard = card.nextElementSibling;
        if (!nextCard?.classList.contains("work-card")) return;
        const leftBounds = card.getBoundingClientRect();
        const rightBounds = nextCard.getBoundingClientRect();
        // Wrapped cards are on different rows and do not share a horizontal
        // boundary. Automatic layout owns their widths at that breakpoint.
        if (Math.abs(leftBounds.top - rightBounds.top) > 4) return;
        const startX = event.clientX;
        const leftWidth = leftBounds.width;
        const rightWidth = rightBounds.width;
        const minimumWidth = 190;
        resizer.classList.add("resizing");
        resizer.setPointerCapture(event.pointerId);
        const move = moveEvent => {
          const requestedDelta = moveEvent.clientX - startX;
          const delta = Math.max(-(leftWidth - minimumWidth),
            Math.min(rightWidth - minimumWidth, requestedDelta));
          card.style.flex = `0 0 ${Math.round(leftWidth + delta)}px`;
          nextCard.style.flex = `0 0 ${Math.round(rightWidth - delta)}px`;
          activeSession?.scheduleRefit();
          notifyRdpLayout(false);
        };
        const finish = finishEvent => {
          if (resizer.hasPointerCapture(finishEvent.pointerId))
            resizer.releasePointerCapture(finishEvent.pointerId);
          resizer.classList.remove("resizing");
          resizer.removeEventListener("pointermove", move);
          resizer.removeEventListener("pointerup", finish);
          resizer.removeEventListener("pointercancel", finish);
          saveCardLayout();
          notifyRdpLayout(true);
        };
        resizer.addEventListener("pointermove", move);
        resizer.addEventListener("pointerup", finish);
        resizer.addEventListener("pointercancel", finish);
      });
    });
    document.querySelector("#auto-layout")?.addEventListener("click", () => {
      ["servers", "terminal", "sftp"].forEach(id => {
        const card = workspace.querySelector(`[data-card="${id}"]`);
        card.removeAttribute("style");
        workspace.appendChild(card);
      });
      setCardLayoutMode("columns");
      localStorage.removeItem("masterterm.cardLayout");
      adaptCardLayoutToViewport();
      activeSession?.scheduleRefit();
    });
  };

  const normalizeRemotePath = value => {
    const parts = [];
    String(value || "/").replaceAll("\\", "/").split("/").forEach(part => {
      if (!part || part === ".") return;
      if (part === "..") parts.pop();
      else parts.push(part);
    });
    return `/${parts.join("/")}`;
  };

  const remoteParent = path => {
    const normalized = normalizeRemotePath(path);
    if (normalized === "/") return "/";
    const slash = normalized.lastIndexOf("/");
    return slash <= 0 ? "/" : normalized.slice(0, slash);
  };

  const formatSize = bytes => {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value < 0) return "—";
    if (value < 1024) return `${value} B`;
    const units = ["KB", "MB", "GB", "TB"];
    let size = value;
    let unit = -1;
    do { size /= 1024; unit += 1; } while (size >= 1024 && unit < units.length - 1);
    return `${size >= 10 ? size.toFixed(1) : size.toFixed(2)} ${units[unit]}`;
  };

  const updateLocalControls = () => {
    localBackButton.disabled = localHistoryIndex <= 0;
    const selectedEntries = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path))
      .filter(Boolean);
    localUploadButton.disabled =
      !selectedEntries.length || !sftpProfile || Boolean(activeTransferId) || transferStarting;
  };

  const applyRowSelection = (tbody, selectedPaths) => {
    tbody.querySelectorAll("tr[data-path]").forEach(row =>
      row.classList.toggle("selected", selectedPaths.has(row.dataset.path)));
  };

  const sortDirectoryEntries = (entries, sortState) => {
    const filtered = [...entries];
    const valueOf = (entry, key) => {
      if (key === "size") return entry.directory ? -1 : Number(entry.size) || 0;
      if (key === "modified") return Number(entry.modified) || -1;
      return String(entry[key] ?? "").toLocaleLowerCase();
    };
    return filtered.sort((left, right) => {
      if (left.directory !== right.directory)
        return left.directory ? -1 : 1;
      const leftValue = valueOf(left, sortState.key);
      const rightValue = valueOf(right, sortState.key);
      const result = typeof leftValue === "number"
        ? leftValue - rightValue
        : String(leftValue).localeCompare(String(rightValue), "zh-CN", { numeric: true });
      return result * sortState.direction || left.name.localeCompare(right.name, "zh-CN", { numeric: true });
    });
  };

  const updateDirectorySortHeaders = (table, sortState) => {
    table.querySelectorAll("thead th[data-sort]").forEach(header => {
      const active = header.dataset.sort === sortState.key;
      header.dataset.sortDirection = active ? (sortState.direction > 0 ? "asc" : "desc") : "";
      header.title = active
        ? `按${header.textContent}排序（${sortState.direction > 0 ? "升序" : "降序"}）`
        : `按${header.textContent}排序`;
    });
  };

  const beginFileRowDrag = (event, entry, selectedPaths, selectEntry, dragType) => {
    if (!selectedPaths.has(entry.path))
      selectEntry(entry, false);
    const paths = [...selectedPaths];
    event.dataTransfer.effectAllowed = "copy";
    event.dataTransfer.setData(dragType, JSON.stringify(paths));
    event.currentTarget.classList.add("drag-source");
  };

  const finishFileRowDrag = event => {
    event.currentTarget.classList.remove("drag-source");
  };

  const readDraggedPaths = (dataTransfer, dragType) => {
    try {
      const value = JSON.parse(dataTransfer.getData(dragType) || "[]");
      return Array.isArray(value) ? value.filter(path => typeof path === "string") : [];
    } catch {
      return [];
    }
  };

  const updateLocalSelection = () => {
    applyRowSelection(localFiles, localSelectedPaths);
    const selected = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path)).filter(Boolean);
    localSelectedEntry = selected.at(-1) || null;
    if (selected.length === 1)
      localStatus.textContent = selected[0].directory
        ? selected[0].path : `已选择：${selected[0].name}`;
    else if (selected.length > 1)
      localStatus.textContent = `已选择 ${selected.length} 个本机项目`;
    updateLocalControls();
  };

  const selectLocalEntry = (entry, additive = false) => {
    if (!additive)
      localSelectedPaths.clear();
    if (additive && localSelectedPaths.has(entry.path))
      localSelectedPaths.delete(entry.path);
    else
      localSelectedPaths.add(entry.path);
    updateLocalSelection();
  };

  const renderLocalEntries = entries => {
    localFiles.replaceChildren();
    localSelectedEntry = null;
    localSelectedPaths.clear();
    const visibleEntries = sortDirectoryEntries(entries, localSort);
    localEntriesByPath = new Map(visibleEntries.map(entry => [entry.path, entry]));
    visibleEntries.forEach(entry => {
      const row = document.createElement("tr");
      row.dataset.path = entry.path;
      row.draggable = true;
      const name = document.createElement("td");
      name.className = `local-file-name ${entry.directory ? "directory" : "file"}`;
      name.textContent = `${entry.directory ? "📁" : "📄"}  ${entry.name}`;
      name.title = entry.path;
      const size = document.createElement("td");
      size.textContent = entry.directory ? "—" : formatSize(entry.size);
      const modified = document.createElement("td");
      modified.textContent = entry.modified >= 0
        ? new Date(entry.modified * 1000).toLocaleString() : "—";
      row.append(name, size, modified);
      row.addEventListener("click", event => {
        if (suppressSelectionClick) return;
        activeFilePane = "local";
        selectLocalEntry(entry, event.ctrlKey || event.metaKey);
      });
      row.addEventListener("contextmenu", event => showLocalContextMenu(event, entry));
      row.addEventListener("dragstart", event =>
        beginFileRowDrag(
          event, entry, localSelectedPaths, selectLocalEntry, localDragType));
      row.addEventListener("dragend", finishFileRowDrag);
      if (entry.directory)
        row.addEventListener("dblclick", event => {
          if (!event.ctrlKey) loadLocalDirectory(entry.path);
        });
      localFiles.appendChild(row);
    });
    localEmpty.hidden = visibleEntries.length !== 0;
    if (!visibleEntries.length)
      localEmpty.textContent = entries.length ? "没有匹配的项目" : "此目录为空";
    updateDirectorySortHeaders(document.querySelector(".local-table"), localSort);
    updateLocalControls();
  };

  const loadLocalDirectory = (requestedPath, recordHistory = true) => {
    localEmpty.hidden = false;
    localEmpty.textContent = "正在读取本机目录…";
    localStatus.textContent = "正在读取本机目录…";
    post("local.list", { path: requestedPath || "" }).then(result => {
      localLoaded = true;
      localPath = result.path;
      localParentPath = result.parent || "";
      localPathInput.value = localPath;
      if (recordHistory) {
        localHistory = localHistory.slice(0, localHistoryIndex + 1);
        if (localHistory.at(-1) !== localPath)
          localHistory.push(localPath);
        localHistoryIndex = localHistory.length - 1;
      }
      localDirectoryEntries = result.entries || [];
      renderLocalEntries(localDirectoryEntries);
      localStatus.textContent = `${result.entries?.length || 0} 个项目 · ${localPath}`;
      renderLocalFavorites();
    }).catch(error => {
      localEmpty.hidden = false;
      localEmpty.textContent = `读取失败：${error.message}`;
      localStatus.textContent = error.message;
      reportError(error);
    });
  };

  const navigateLocalHistory = offset => {
    const target = localHistoryIndex + offset;
    if (target < 0 || target >= localHistory.length) return;
    localHistoryIndex = target;
    loadLocalDirectory(localHistory[target], false);
  };

  const updateSftpHistoryButtons = () => {
    sftpBackButton.disabled = sftpHistoryIndex <= 0;
    sftpForwardButton.disabled = sftpHistoryIndex < 0 ||
      sftpHistoryIndex >= sftpHistory.length - 1;
  };

  const navigateSftpHistory = offset => {
    const target = sftpHistoryIndex + offset;
    if (target < 0 || target >= sftpHistory.length) return false;
    sftpHistoryIndex = target;
    updateSftpHistoryButtons();
    loadSftpDirectory(sftpHistory[sftpHistoryIndex], false);
    return true;
  };

  const sftpFavoriteStorageKey = () => sftpProfile
    ? `masterterm.sftpFavorites.${encodeURIComponent(
        sftpProfile.address || String(sftpProfile.index))}`
    : "";

  const readSftpFavorites = () => {
    const key = sftpFavoriteStorageKey();
    if (!key) return [];
    try {
      const values = JSON.parse(localStorage.getItem(key) || "[]");
      return Array.isArray(values)
        ? [...new Set(values.map(normalizeRemotePath))].sort((a, b) =>
            a.localeCompare(b, "zh-CN", { numeric: true }))
        : [];
    } catch {
      localStorage.removeItem(key);
      return [];
    }
  };

  const renderSftpFavorites = () => {
    const favorites = readSftpFavorites();
    sftpFavoritesList.replaceChildren();
    favorites.forEach(path => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.path = path;
      button.textContent = path;
      button.title = path;
      sftpFavoritesList.appendChild(button);
    });
    if (!favorites.length) {
      const empty = document.createElement("div");
      empty.className = "empty-favorite";
      empty.textContent = "暂无收藏，请在目录右键菜单中添加";
      sftpFavoritesList.appendChild(empty);
    }
    sftpFavoriteButton.classList.toggle(
      "active", favorites.includes(normalizeRemotePath(sftpPath)));
  };

  const toggleSftpFavorite = path => {
    const key = sftpFavoriteStorageKey();
    if (!key) return;
    const normalized = normalizeRemotePath(path);
    const favorites = readSftpFavorites();
    const index = favorites.indexOf(normalized);
    if (index >= 0) {
      favorites.splice(index, 1);
      sftpStatusText.textContent = `已取消收藏：${normalized}`;
    } else {
      favorites.push(normalized);
      sftpStatusText.textContent = `已收藏：${normalized}`;
    }
    localStorage.setItem(key, JSON.stringify(favorites));
    renderSftpFavorites();
  };

  const remoteChildPath = (directory, name) =>
    normalizeRemotePath(`${directory === "/" ? "" : directory}/${name}`);

  const hideSftpContextMenu = () => {
    hideVisibleElement(sftpContextMenu);
  };

  const hideSftpFavoritesMenu = () => {
    hideVisibleElement(sftpFavoritesMenu);
  };

  const showSftpFavoritesMenu = () => {
    renderSftpFavorites();
    sftpFavoritesMenu.hidden = false;
    const anchor = sftpFavoriteButton.getBoundingClientRect();
    const width = sftpFavoritesMenu.offsetWidth;
    const height = sftpFavoritesMenu.offsetHeight;
    sftpFavoritesMenu.style.left =
      `${Math.max(8, Math.min(anchor.left, window.innerWidth - width - 8))}px`;
    sftpFavoritesMenu.style.top =
      `${Math.max(8, Math.min(anchor.bottom + 5, window.innerHeight - height - 8))}px`;
  };

  const localFavoriteStorageKey = "masterterm.localFavorites";

  const readLocalFavorites = () => {
    try {
      const values = JSON.parse(localStorage.getItem(localFavoriteStorageKey) || "[]");
      if (!Array.isArray(values)) return [];
      const unique = new Map();
      values.forEach(value => {
        const path = String(value || "").trim();
        if (path) unique.set(path.toLocaleLowerCase(), path);
      });
      return [...unique.values()].sort((a, b) =>
        a.localeCompare(b, "zh-CN", { numeric: true }));
    } catch {
      localStorage.removeItem(localFavoriteStorageKey);
      return [];
    }
  };

  const renderLocalFavorites = () => {
    const favorites = readLocalFavorites();
    localFavoritesList.replaceChildren();
    favorites.forEach(path => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.path = path;
      button.textContent = path;
      button.title = path;
      localFavoritesList.appendChild(button);
    });
    if (!favorites.length) {
      const empty = document.createElement("div");
      empty.className = "empty-favorite";
      empty.textContent = "暂无收藏，请在目录右键菜单中添加";
      localFavoritesList.appendChild(empty);
    }
    localFavoriteButton.classList.toggle("active", favorites.some(path =>
      path.localeCompare(localPath, undefined, { sensitivity: "accent" }) === 0));
  };

  const toggleLocalFavorite = path => {
    const value = String(path || "").trim();
    if (!value) return;
    const favorites = readLocalFavorites();
    const index = favorites.findIndex(item =>
      item.localeCompare(value, undefined, { sensitivity: "accent" }) === 0);
    if (index >= 0) {
      favorites.splice(index, 1);
      localStatus.textContent = `已取消收藏：${value}`;
    } else {
      favorites.push(value);
      localStatus.textContent = `已收藏：${value}`;
    }
    localStorage.setItem(localFavoriteStorageKey, JSON.stringify(favorites));
    renderLocalFavorites();
  };

  const hideLocalFavoritesMenu = () => {
    hideVisibleElement(localFavoritesMenu);
  };

  const showLocalFavoritesMenu = () => {
    renderLocalFavorites();
    localFavoritesMenu.hidden = false;
    const anchor = localFavoriteButton.getBoundingClientRect();
    const width = localFavoritesMenu.offsetWidth;
    const height = localFavoritesMenu.offsetHeight;
    localFavoritesMenu.style.left =
      `${Math.max(8, Math.min(anchor.left, window.innerWidth - width - 8))}px`;
    localFavoritesMenu.style.top =
      `${Math.max(8, Math.min(anchor.bottom + 5, window.innerHeight - height - 8))}px`;
  };

  const showSftpContextMenu = (event, entry = null) => {
    event.preventDefault();
    activeFilePane = "remote";
    hideSftpFavoritesMenu();
    if (entry && !sftpSelectedPaths.has(entry.path)) {
      sftpSelectedPaths.clear();
      sftpSelectedPaths.add(entry.path);
    } else if (!entry) {
      sftpSelectedPaths.clear();
    }
    updateSftpSelection();
    const selected = [...sftpSelectedPaths]
      .map(path => sftpEntriesByPath.get(path)).filter(Boolean);
    const singleFile = selected.length === 1 && !selected[0].directory;
    sftpContextMenu.querySelector('[data-action="preview"]').hidden = !singleFile;
    sftpContextMenu.querySelector('[data-action="open"]').hidden = !singleFile;
    sftpContextMenu.querySelector('[data-action="open-with"]').hidden = !singleFile;
    sftpContextMenu.querySelector('[data-action="open-default"]').hidden = !singleFile;
    sftpContextMenu.querySelector('[data-action="rename"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="copy-to"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="move-to"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="chmod"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="properties"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="copy-path"]').hidden = selected.length !== 1;
    sftpContextMenu.querySelector('[data-action="download"]').hidden = !selected.length;
    sftpContextMenu.querySelector('[data-action="delete"]').hidden = !selected.length;
    sftpContextMenu.querySelector('[data-action="download"]').textContent =
      selected.length > 1 ? `下载 ${selected.length} 个项目` : "下载";
    sftpContextMenu.querySelector('[data-action="delete"]').textContent =
      selected.length > 1 ? `删除 ${selected.length} 个项目` : "删除";
    const download = sftpContextMenu.querySelector('[data-action="download"]');
    download.hidden = !selected.length;
    const favoritePath = selected.length === 1 && selected[0].directory
      ? selected[0].path : sftpPath;
    const favoriteButton = sftpContextMenu.querySelector('[data-action="favorite"]');
    favoriteButton.dataset.path = favoritePath;
    favoriteButton.textContent = readSftpFavorites().includes(normalizeRemotePath(favoritePath))
      ? "取消收藏目录" : "收藏目录";
    sftpContextMenu.hidden = false;
    const width = sftpContextMenu.offsetWidth;
    const height = sftpContextMenu.offsetHeight;
    sftpContextMenu.style.left =
      `${Math.max(8, Math.min(event.clientX, window.innerWidth - width - 8))}px`;
    sftpContextMenu.style.top =
      `${Math.max(8, Math.min(event.clientY, window.innerHeight - height - 8))}px`;
  };

  const hideLocalContextMenu = () => {
    hideVisibleElement(localContextMenu);
  };

  const showLocalContextMenu = (event, entry = null) => {
    event.preventDefault();
    activeFilePane = "local";
    hideLocalFavoritesMenu();
    if (entry && !localSelectedPaths.has(entry.path)) {
      localSelectedPaths.clear();
      localSelectedPaths.add(entry.path);
    } else if (!entry) {
      localSelectedPaths.clear();
    }
    updateLocalSelection();
    const selected = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path)).filter(Boolean);
    const open = localContextMenu.querySelector('[data-action="open"]');
    const upload = localContextMenu.querySelector('[data-action="upload"]');
    const sync = localContextMenu.querySelector('[data-action="sync"]');
    const rename = localContextMenu.querySelector('[data-action="rename"]');
    const remove = localContextMenu.querySelector('[data-action="delete"]');
    const favorite = localContextMenu.querySelector('[data-action="favorite"]');
    open.hidden = selected.length !== 1 || !selected[0].directory;
    upload.hidden = !selected.length || !sftpProfile;
    upload.textContent = selected.length > 1
      ? `上传 ${selected.length} 个项目到当前远程目录` : "上传到当前远程目录";
    sync.hidden = !sftpProfile;
    rename.hidden = selected.length !== 1;
    const favoritePath = selected.length === 1 && selected[0].directory
      ? selected[0].path : localPath;
    favorite.dataset.path = favoritePath;
    favorite.textContent = readLocalFavorites().some(path =>
      path.localeCompare(favoritePath, undefined, { sensitivity: "accent" }) === 0)
      ? "取消收藏目录" : "收藏目录";
    remove.hidden = !selected.length;
    remove.textContent = selected.length > 1
      ? `删除 ${selected.length} 个项目` : "删除";
    localContextMenu.hidden = false;
    const width = localContextMenu.offsetWidth;
    const height = localContextMenu.offsetHeight;
    localContextMenu.style.left =
      `${Math.max(8, Math.min(event.clientX, window.innerWidth - width - 8))}px`;
    localContextMenu.style.top =
      `${Math.max(8, Math.min(event.clientY, window.innerHeight - height - 8))}px`;
  };

  const validRemoteName = name => {
    const value = String(name || "").trim();
    return value && value !== "." && value !== ".." && !value.includes("/");
  };

  const runSftpOperation = (operation, path, targetPath, successMessage) => {
    if (!sftpProfile) return Promise.resolve();
    sftpStatusText.textContent = "正在执行远程文件操作…";
    return post("sftp.operation", {
      operation,
      index: sftpProfile.index,
      path,
      targetPath: targetPath || ""
    }).then(() => {
      sftpSelectedEntry = null;
      sftpStatusText.textContent = successMessage;
      return loadSftpDirectory(sftpPath, false);
    }).catch(error => {
      sftpStatusText.textContent = error.message;
      reportError(error);
    });
  };

  const createRemoteEntry = async directory => {
    if (!sftpProfile) return;
    const label = directory ? "文件夹" : "文件";
    const name = await requestText(`新建${label}`, `请输入新${label}名称：`, "");
    if (name === null) return;
    if (!validRemoteName(name)) {
      reportError(`${label}名称不能为空，且不能包含“/”`);
      return;
    }
    const path = remoteChildPath(sftpPath, name.trim());
    runSftpOperation(directory ? "mkdir" : "touch", path, "",
      `已创建${label}：${name.trim()}`);
  };

  const renameRemoteEntry = async entry => {
    if (!entry) return;
    const name = await requestText("重命名", "请输入新名称：", entry.name);
    if (name === null || name.trim() === entry.name) return;
    if (!validRemoteName(name)) {
      reportError("名称不能为空，且不能包含“/”");
      return;
    }
    const targetPath = remoteChildPath(remoteParent(entry.path), name.trim());
    runSftpOperation("rename", entry.path, targetPath,
      `已重命名为：${name.trim()}`);
  };

  const copyRemoteEntry = async entry => {
    if (!entry) return;
    const suggested = remoteChildPath(
      remoteParent(entry.path), `${entry.name} 副本`);
    const targetPath = await requestText(
      "复制远程项目", "请输入目标完整路径：", suggested);
    if (targetPath === null || !targetPath.trim()) return;
    runSftpOperation("copy", entry.path, normalizeRemotePath(targetPath.trim()),
      `已复制到：${targetPath.trim()}`);
  };

  const moveRemoteEntry = async entry => {
    if (!entry) return;
    const targetPath = await requestText(
      "移动远程项目", "请输入目标完整路径：", entry.path);
    if (targetPath === null || !targetPath.trim()
        || normalizeRemotePath(targetPath.trim()) === entry.path) return;
    runSftpOperation("rename", entry.path, normalizeRemotePath(targetPath.trim()),
      `已移动到：${targetPath.trim()}`);
  };

  const chmodRemoteEntry = async entry => {
    if (!entry) return;
    const mode = await requestText(
      "修改远程权限", "请输入八进制权限（例如 755 或 0644）：",
      entry.permissions === "—" ? "755" : entry.permissions);
    if (mode === null) return;
    const value = mode.trim();
    if (!/^[0-7]{3,4}$/.test(value)) {
      reportError("权限必须是 3 到 4 位八进制数字");
      return;
    }
    runSftpOperation("chmod", entry.path, value,
      `已将权限修改为：${value}`);
  };

  const showRemoteProperties = entry => {
    if (!entry) return;
    showActionDialog({
      title: "远程项目属性",
      message: `名称：${entry.name}\n路径：${entry.path}\n类型：${entry.directory ? "文件夹" : "文件"}\n大小：${entry.directory ? "—" : formatSize(entry.size)}\n修改时间：${entry.modified >= 0 ? new Date(entry.modified * 1000).toLocaleString() : "—"}\n权限：${entry.permissions || "—"}\n所有者：${entry.owner || "—"}`,
      buttons: [{ action: "ok", label: "确定", primary: true }]
    });
  };

  const removeRemoteEntry = async entry => {
    if (!entry) return;
    const type = entry.directory ? "文件夹及其中全部内容" : "文件";
    if (!await requestDeleteConfirmation(
        "删除远程项目", `确定永久删除${type}“${entry.name}”吗？`, "删除")) return;
    runSftpOperation("remove", entry.path, "", `已删除：${entry.name}`);
  };

  const removeSelectedRemoteEntries = async entries => {
    if (!entries.length || !sftpProfile) return;
    const description = entries.length === 1
      ? `“${entries[0].name}”` : `${entries.length} 个远程项目`;
    if (!await requestDeleteConfirmation(
        "删除远程项目", `确定永久删除${description}吗？文件夹中的内容也会被删除。`, "删除"))
      return;
    sftpStatusText.textContent = `正在删除 ${entries.length} 个远程项目…`;
    try {
      for (const entry of entries) {
        await post("sftp.operation", {
          operation: "remove",
          index: sftpProfile.index,
          path: entry.path,
          targetPath: ""
        });
      }
      sftpSelectedPaths.clear();
      await loadSftpDirectory(sftpPath, false);
    } catch (error) {
      sftpStatusText.textContent = error.message;
      reportError(error);
    }
  };

  const uploadSelectedLocalEntries = () => {
    const entries = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path))
      .filter(Boolean);
    if (!entries.length || !sftpProfile) return;
    resetConflictPolicyForNewSession();
    const remoteNames = new Set([...sftpEntriesByPath.values()]
      .map(entry => String(entry.name).toLocaleLowerCase()));
    const conflictEntry = entries.find(entry =>
      remoteNames.has(String(entry.name).toLocaleLowerCase()));
    const startBatch = policy => entries.forEach(entry => startSftpUpload(
      entry.path, false, true,
      remoteNames.has(String(entry.name).toLocaleLowerCase()) ? policy : "overwrite",
      entry.directory));
    if (!conflictEntry) {
      startBatch("overwrite");
      return;
    }
    const remoteConflict = [...sftpEntriesByPath.values()].find(entry =>
      entry.name.localeCompare(conflictEntry.name, undefined,
        { sensitivity: "accent" }) === 0);
    chooseConflictPolicy(conflictEntry.name, "当前远程目录中",
      formatConflictDetails(conflictEntry, remoteConflict)).then(policy => {
      if (policy === "overwrite-all") {
        transferConflictPolicy = "overwrite";
        startBatch("overwrite");
        return;
      }
      startBatch(policy || "skip");
    }).catch(reportError);
  };

  const syncLocalToRemote = async () => {
    if (!sftpProfile || !localPath) return;
    const result = await showActionDialog({
      title: "增量同步到远程",
      message:
        `将本机目录 “${localPath}” 增量同步到远程目录 “${sftpPath}”。\n`
        + "远程已存在且大小相同的文件将跳过，仅上传新增或变化的文件。继续？",
      value: null,
      buttons: [{ action: "sync", label: "开始同步", primary: true }]
    }).catch(() => null);
    if (!result || result.action !== "sync") return;
    startSftpUpload(localPath, false, true, "overwrite", true,
      sftpPath, "", true);
  };

  const runLocalOperation = async (operation, params, successMessage) => {
    try {
      await post("local.operation", { operation, ...params });
      localStatus.textContent = successMessage;
      await loadLocalDirectory(localPath, false);
    } catch (error) {
      localStatus.textContent = error.message;
      reportError(error);
    }
  };

  const createLocalEntry = async directory => {
    if (!localPath) return;
    const label = directory ? "文件夹" : "文件";
    const name = await requestText(`新建${label}`, `请输入新${label}名称：`, "");
    if (name === null) return;
    const value = name.trim();
    if (!validRemoteName(value) || value.includes("\\")) {
      reportError(`${label}名称不能为空，且不能包含路径分隔符`);
      return;
    }
    runLocalOperation(directory ? "mkdir" : "touch",
      { parentPath: localPath, name: value }, `已创建本机${label}：${value}`);
  };

  const renameLocalEntry = async entry => {
    if (!entry) return;
    const name = await requestText("重命名", "请输入新名称：", entry.name);
    if (name === null || name.trim() === entry.name) return;
    const value = name.trim();
    if (!validRemoteName(value) || value.includes("\\")) {
      reportError("名称不能为空，且不能包含路径分隔符");
      return;
    }
    runLocalOperation("rename", { path: entry.path, name: value },
      `已重命名为：${value}`);
  };

  const removeSelectedLocalEntries = async () => {
    const entries = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path)).filter(Boolean);
    if (!entries.length) return;
    const description = entries.length === 1
      ? `“${entries[0].name}”` : `${entries.length} 个本机项目`;
    if (!await requestDeleteConfirmation(
        "删除本机项目", `确定永久删除${description}吗？此操作无法撤销。`, "删除"))
      return;
    try {
      await post("local.remove", { paths: entries.map(entry => entry.path) });
      localSelectedPaths.clear();
      await loadLocalDirectory(localPath, false);
    } catch (error) {
      localStatus.textContent = error.message;
      reportError(error);
    }
  };

  const updateSftpSelection = () => {
    applyRowSelection(sftpFiles, sftpSelectedPaths);
    const selected = [...sftpSelectedPaths]
      .map(path => sftpEntriesByPath.get(path)).filter(Boolean);
    sftpSelectedEntry = selected.at(-1) || null;
    sftpDownloadButton.disabled = selected.length === 0;
  };

  const selectSftpEntry = (entry, additive = false) => {
    if (!additive)
      sftpSelectedPaths.clear();
    if (additive && sftpSelectedPaths.has(entry.path))
      sftpSelectedPaths.delete(entry.path);
    else
      sftpSelectedPaths.add(entry.path);
    updateSftpSelection();
  };

  const remoteFileExtension = name => {
    const match = String(name || "").match(/\.[^./\\]+$/);
    return match ? match[0].toLowerCase() : "<no-extension>";
  };
  const openRemoteFile = async (entry, mode = "associated") => {
    if (!sftpProfile || !entry || entry.directory) return;
    let applicationPath = "";
    let chooseApplication = false;
    const extension = remoteFileExtension(entry.name);
    if (mode === "choose") {
      let selected;
      try {
        selected = await post("local.chooseProgram", { path: "" });
      } catch (error) {
        reportError(error);
        return;
      }
      if (selected.cancelled || !selected.path) return;
      appSettings.editorAssociations = {
        ...(appSettings.editorAssociations || {}), [extension]: selected.path
      };
      localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
      applicationPath = selected.path;
    } else if (mode === "windows-dialog") {
      chooseApplication = true;
    } else {
      applicationPath = appSettings.editorAssociations?.[extension] || "";
    }
    sftpStatusText.textContent = `正在打开远程文件：${entry.name}`;
    sftpStatusText.title = entry.path;
    post("sftp.open", {
      index: sftpProfile.index,
      remotePath: entry.path,
      chooseApplication,
      applicationPath,
      autoSyncConflict: appSettings.remoteEditConflict
    }).then(result => {
      if (result.reused)
        sftpStatusText.textContent = `编辑副本已打开：${entry.name}（保存后自动同步）`;
      else
        sftpStatusText.textContent = `正在下载编辑副本：${entry.name}`;
    }).catch(error => {
      sftpStatusText.textContent = error.message;
      reportError(error);
    });
  };

  const openRemotePreview = async entry => {
    if (!sftpProfile || !entry || entry.directory) return;
    remotePreviewName.textContent = entry.name;
    remotePreviewContent.textContent = "正在读取远程文件内容…";
    remotePreviewStatus.textContent = "";
    remotePreviewDialog.showModal();
    try {
      const result = await post("sftp.preview", {
        index: sftpProfile.index,
        remotePath: entry.path,
        maxBytes: 524288
      });
      let text = "";
      if (result.data) {
        const binary = atob(result.data);
        const bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
        text = new TextDecoder("utf-8").decode(bytes);
      }
      remotePreviewContent.textContent = text;
      remotePreviewStatus.textContent = text
        ? `已显示前 ${text.length} 个字符（仅读取文件开头）`
        : "（文件内容为空）";
    } catch (error) {
      remotePreviewContent.textContent = "";
      remotePreviewStatus.textContent =
        error instanceof Error ? error.message : String(error);
    }
  };

  let sftpFilterQuery = "";
  const sftpFilterInput = document.querySelector("#sftp-filter-input");
  const sftpFilterClear = document.querySelector("#sftp-filter-clear");
  const updateSftpFilterClearVisibility = () => {
    if (sftpFilterClear) {
      sftpFilterClear.hidden = !(sftpFilterInput && sftpFilterInput.value);
    }
  };
  sftpFilterInput?.addEventListener("input", () => {
    sftpFilterQuery = (sftpFilterInput.value || "").trim().toLowerCase();
    updateSftpFilterClearVisibility();
    renderSftpEntries(sftpDirectoryEntries);
  });
  sftpFilterInput?.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      sftpFilterInput.value = "";
      sftpFilterQuery = "";
      updateSftpFilterClearVisibility();
      renderSftpEntries(sftpDirectoryEntries);
      event.preventDefault();
    }
  });
  sftpFilterClear?.addEventListener("click", () => {
    if (sftpFilterInput) {
      sftpFilterInput.value = "";
      sftpFilterQuery = "";
      updateSftpFilterClearVisibility();
      renderSftpEntries(sftpDirectoryEntries);
      sftpFilterInput.focus();
    }
  });

  let sftpJumpBuffer = "";
  let sftpJumpTimer = null;
  const sftpJumpIndicator = document.querySelector("#sftp-jump-indicator");

  const handleSftpTypeToJump = char => {
    clearTimeout(sftpJumpTimer);
    sftpJumpBuffer += char;
    if (sftpJumpIndicator) {
      sftpJumpIndicator.textContent = `跳转: ${sftpJumpBuffer}`;
      sftpJumpIndicator.hidden = false;
    }
    sftpJumpTimer = setTimeout(() => {
      sftpJumpBuffer = "";
      if (sftpJumpIndicator) sftpJumpIndicator.hidden = true;
    }, 800);

    const targetQuery = sftpJumpBuffer.toLowerCase();
    const rows = Array.from(sftpFiles.querySelectorAll("tr[data-path]"));
    if (!rows.length) return;

    let matchRow = rows.find(r => {
      const entry = sftpEntriesByPath.get(r.dataset.path);
      return entry && entry.name.toLowerCase().startsWith(targetQuery);
    });
    if (!matchRow && targetQuery.length > 1) {
      matchRow = rows.find(r => {
        const entry = sftpEntriesByPath.get(r.dataset.path);
        return entry && entry.name.toLowerCase().includes(targetQuery);
      });
    }

    if (matchRow) {
      const entry = sftpEntriesByPath.get(matchRow.dataset.path);
      if (entry) {
        selectSftpEntry(entry, false);
        matchRow.scrollIntoView({ block: "nearest", behavior: "smooth" });
      }
    }
  };

  const renderSftpEntries = entries => {
    sftpFiles.replaceChildren();
    sftpSelectedEntry = null;
    sftpSelectedPaths.clear();
    let candidateEntries = entries;
    if (sftpFilterQuery) {
      candidateEntries = entries.filter(entry =>
        entry.name.toLowerCase().includes(sftpFilterQuery));
    }
    const visibleEntries = sortDirectoryEntries(candidateEntries, sftpSort);
    sftpEntriesByPath = new Map(visibleEntries.map(entry => [entry.path, entry]));
    visibleEntries.forEach(entry => {
      const row = document.createElement("tr");
      row.dataset.path = entry.path;
      row.draggable = true;
      const nameCell = document.createElement("td");
      nameCell.className = `sftp-file-name ${entry.directory ? "directory" : "file"}`;
      nameCell.textContent = `${entry.directory ? "📁" : entry.kind === "l" ? "🔗" : "📄"}  ${entry.name}`;
      nameCell.title = entry.path;
      const sizeCell = document.createElement("td");
      sizeCell.textContent = entry.directory ? "—" : formatSize(entry.size);
      const modifiedCell = document.createElement("td");
      modifiedCell.textContent = entry.modified >= 0
        ? new Date(entry.modified * 1000).toLocaleString() : "—";
      const permissionsCell = document.createElement("td");
      permissionsCell.textContent = entry.permissions || "—";
      const ownerCell = document.createElement("td");
      ownerCell.textContent = entry.owner || "—";
      row.append(nameCell, sizeCell, modifiedCell, permissionsCell, ownerCell);
      row.addEventListener("click", event => {
        if (suppressSelectionClick) return;
        activeFilePane = "remote";
        selectSftpEntry(entry, event.ctrlKey || event.metaKey);
      });
      row.addEventListener("contextmenu", event => showSftpContextMenu(event, entry));
      row.addEventListener("dragstart", event =>
        beginFileRowDrag(
          event, entry, sftpSelectedPaths, selectSftpEntry, remoteDragType));
      row.addEventListener("dragend", finishFileRowDrag);
      if (entry.directory)
        row.addEventListener("dblclick", () => loadSftpDirectory(entry.path));
      else
        row.addEventListener("dblclick", () => openRemoteFile(entry));
      sftpFiles.appendChild(row);
    });
    sftpEmpty.hidden = visibleEntries.length !== 0;
    if (!visibleEntries.length)
      sftpEmpty.textContent = entries.length ? "没有匹配的项目" : "此目录为空";
    updateDirectorySortHeaders(document.querySelector(".sftp-table"), sftpSort);
  };

  const installMarqueeSelection = (container, tbody, selectedPaths,
                                    entriesByPath, updateSelection) => {
    container.addEventListener("selectstart", event => event.preventDefault());
    container.addEventListener("dragstart", event => {
      if (!event.target.closest("tr[draggable='true']"))
        event.preventDefault();
    });
    container.addEventListener("pointerdown", event => {
      if (event.button !== 0 || event.target.closest("button,input")
          || event.clientX >= container.getBoundingClientRect().right - 14)
        return;
      activeFilePane = tbody === localFiles ? "local" : "remote";
      const startX = event.clientX;
      const startY = event.clientY;
      const pressedPath = event.target.closest("tr[data-path]")?.dataset.path || "";
      // Rows use native HTML drag/drop for transfers between the two panes.
      // Marquee selection starts from the blank part of a file pane.
      if (pressedPath)
        return;
      const baseSelection = event.ctrlKey || event.metaKey
        ? new Set(selectedPaths) : new Set();
      let box = null;
      let moved = false;
      const move = moveEvent => {
        const distance = Math.hypot(moveEvent.clientX - startX, moveEvent.clientY - startY);
        if (!moved && distance < 5) return;
        moved = true;
        moveEvent.preventDefault();
        if (!container.hasPointerCapture(moveEvent.pointerId))
          container.setPointerCapture(moveEvent.pointerId);
        if (!box) {
          box = document.createElement("div");
          box.className = "selection-box";
          document.body.appendChild(box);
        }
        const left = Math.min(startX, moveEvent.clientX);
        const top = Math.min(startY, moveEvent.clientY);
        const right = Math.max(startX, moveEvent.clientX);
        const bottom = Math.max(startY, moveEvent.clientY);
        Object.assign(box.style, {
          left: `${left}px`,
          top: `${top}px`,
          width: `${right - left}px`,
          height: `${bottom - top}px`
        });
        selectedPaths.clear();
        baseSelection.forEach(path => selectedPaths.add(path));
        tbody.querySelectorAll("tr[data-path]").forEach(row => {
          const bounds = row.getBoundingClientRect();
          if (bounds.right >= left && bounds.left <= right
              && bounds.bottom >= top && bounds.top <= bottom
              && entriesByPath().has(row.dataset.path))
            selectedPaths.add(row.dataset.path);
        });
        updateSelection();
      };
      const finish = finishEvent => {
        if (container.hasPointerCapture(finishEvent.pointerId))
          container.releasePointerCapture(finishEvent.pointerId);
        container.removeEventListener("pointermove", move);
        container.removeEventListener("pointerup", finish);
        container.removeEventListener("pointercancel", finish);
        box?.remove();
        if (!moved) {
          const additive = finishEvent.ctrlKey || finishEvent.metaKey;
          if (pressedPath && entriesByPath().has(pressedPath)) {
            if (!additive)
              selectedPaths.clear();
            if (additive && selectedPaths.has(pressedPath))
              selectedPaths.delete(pressedPath);
            else
              selectedPaths.add(pressedPath);
            updateSelection();
          } else if (!additive) {
            selectedPaths.clear();
            updateSelection();
          }
        }
        suppressSelectionClick = true;
        setTimeout(() => { suppressSelectionClick = false; }, 0);
      };
      container.addEventListener("pointermove", move);
      container.addEventListener("pointerup", finish);
      container.addEventListener("pointercancel", finish);
    });
  };

  const installTableColumnResizers = (table, storageKey, defaults) => {
    const headers = [...table.tHead.rows[0].cells];
    let widths = defaults;
    try {
      const saved = JSON.parse(localStorage.getItem(storageKey) || "null");
      if (Array.isArray(saved) && saved.length === headers.length
          && saved.every(value => Number.isFinite(value) && value > 2))
        widths = saved;
    } catch {
      localStorage.removeItem(storageKey);
    }
    const total = widths.reduce((sum, value) => sum + value, 0) || 100;
    widths = widths.map(value => value * 100 / total);
    const colgroup = document.createElement("colgroup");
    const columns = widths.map(width => {
      const column = document.createElement("col");
      column.style.width = `${width}%`;
      colgroup.appendChild(column);
      return column;
    });
    table.insertBefore(colgroup, table.firstChild);
    headers.slice(0, -1).forEach((header, index) => {
      const handle = document.createElement("span");
      handle.className = "column-resizer";
      handle.addEventListener("pointerdown", event => {
        event.preventDefault();
        event.stopPropagation();
        const tableWidth = Math.max(1, table.getBoundingClientRect().width);
        const leftWidth = header.getBoundingClientRect().width * 100 / tableWidth;
        const rightWidth = headers[index + 1].getBoundingClientRect().width * 100 / tableWidth;
        const pairWidth = leftWidth + rightWidth;
        const minimum = Math.min(8, pairWidth / 3);
        const startX = event.clientX;
        handle.classList.add("resizing");
        handle.setPointerCapture(event.pointerId);
        const move = moveEvent => {
          const delta = (moveEvent.clientX - startX) * 100 / tableWidth;
          const nextLeft = Math.max(minimum,
            Math.min(pairWidth - minimum, leftWidth + delta));
          columns[index].style.width = `${nextLeft}%`;
          columns[index + 1].style.width = `${pairWidth - nextLeft}%`;
        };
        const finish = finishEvent => {
          if (handle.hasPointerCapture(finishEvent.pointerId))
            handle.releasePointerCapture(finishEvent.pointerId);
          handle.classList.remove("resizing");
          handle.removeEventListener("pointermove", move);
          handle.removeEventListener("pointerup", finish);
          handle.removeEventListener("pointercancel", finish);
          localStorage.setItem(storageKey, JSON.stringify(columns.map(column =>
            Number.parseFloat(column.style.width))));
        };
        handle.addEventListener("pointermove", move);
        handle.addEventListener("pointerup", finish);
        handle.addEventListener("pointercancel", finish);
      });
      header.appendChild(handle);
    });
  };

  const updateTransferControls = () => {
    sftpTransferRow.hidden = !Boolean(activeTransferId);
    sftpUploadButton.disabled = Boolean(activeTransferId) || transferStarting;
    if (!activeTransferId) {
      sftpCurrentTransferName.textContent = "";
      if (sftpProgressBar) sftpProgressBar.style.width = "0%";
      if (sftpProgressPercent) sftpProgressPercent.textContent = "0%";
      if (sftpProgressSpeed) sftpProgressSpeed.textContent = "—";
      if (sftpProgressEta) sftpProgressEta.textContent = "—";
    }
    updateLocalControls();
  };

  const formatTransferSpeed = (bytes, transferId) => {
    if (!transferId) return "—";
    const started = transferStartedAt.get(transferId) || Date.now();
    transferStartedAt.set(transferId, started);
    const seconds = Math.max(.1, (Date.now() - started) / 1000);
    return `${formatSize(bytes / seconds)}/秒`;
  };
  const transferBytesPerSecond = (bytes, transferId) => {
    if (!transferId) return 0;
    const started = transferStartedAt.get(transferId);
    if (!started) return 0;
    return Number(bytes || 0) / Math.max(.1, (Date.now() - started) / 1000);
  };
  const formatTransferEta = (done, total, transferId) => {
    const speed = transferBytesPerSecond(done, transferId);
    if (!(Number(total) > Number(done) && speed > 0)) return "—";
    const seconds = Math.round((Number(total) - Number(done)) / speed);
    if (seconds <= 0) return "剩余 <1秒";
    if (seconds < 60) return `剩余 ${seconds}秒`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `剩余 ${minutes}分${seconds % 60}秒`;
    return `剩余 ${Math.floor(minutes / 60)}时${minutes % 60}分`;
  };

  const compactTransferName = value => {
    const name = String(value || "文件");
    if (name.length <= 48) return name;
    return `${name.slice(0, 29)}…${name.slice(-18)}`;
  };

  const transferBatchLabel = batch => {
    const index = Number(batch?.index || 0);
    const total = Number(batch?.total || 0);
    return index > 0 && total > 0 ? `文件 ${index}/${total}` : "";
  };

  const transferTaskName = task => compactTransferName(task?.name
    || task?.entry?.name
    || task?.remoteName
    || String(task?.path || "").split(/[\\/]/).at(-1)
    || task?.file?.name || "文件");
  const transferDirection = task => task?.type === "download" ? "下载"
    : task?.type === "edit-sync" ? "同步" : "上传";
  const transferTaskSize = task => Number(task?.size ?? task?.file?.size
    ?? task?.entry?.size ?? 0);
  const formatTransferTime = value => value
    ? new Date(value).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })
    : "—";
  const formatResultSpeed = value => Number(value) > 0 ? `${formatSize(value)}/秒` : "—";
  const transferStateLabel = state => ({
    active: "传输中", queued: "等待", paused: "已暂停", success: "完成", skipped: "跳过", cancelled: "取消", error: "失败"
  }[state] || state);

  const isFailedTransfer = result => result?.state === "error"
    || result?.state === "cancelled";
  const transferFilterValue = task => [
    transferTaskName(task), task?.path, task?.destination, task?.remotePath,
    task?.remoteDirectory, task?.entry?.path, task?.entry?.name,
    task?.message, transferDirection(task), transferStateLabel(task?.state)
  ].filter(value => value != null && value !== "").join(" ").toLocaleLowerCase();
  const transferMatchesFilter = task => {
    const filter = transferFilterText.trim().toLocaleLowerCase();
    return !filter || transferFilterValue(task).includes(filter);
  };

  const showRdpConnectingNotice = session => {
    if (!session?.rdpConnectingNotice) return;
    session.rdpConnectingNotice.hidden = false;
    session.tab.removeAttribute("title");
    session.tab.title = "";
  };

  const hideRdpConnectingNotice = session => {
    if (!session?.rdpConnectingNotice) return;
    session.rdpConnectingNotice.hidden = true;
    session.tab.removeAttribute("title");
    session.tab.title = "";
  };

  const applyTransferGridColumns = () => {
    const visible = activeTransferTab === "queue" ? [0, 1, 2, 3, 5, 6] : [0, 1, 2, 3, 4, 5, 6];
    const value = visible.map(index => `${transferColumnWidths[index]}px`).join(" ");
    sftpTransferResultsHeader.style.setProperty("--transfer-grid-columns", value);
    sftpTransferResultsList.style.setProperty("--transfer-grid-columns", value);
    sftpTransferResultsHeader.style.minWidth = `${visible.reduce((sum, index) => sum + transferColumnWidths[index], 0) + (visible.length - 1) * 6}px`;
    sftpTransferResultsList.style.minWidth = sftpTransferResultsHeader.style.minWidth;
    sftpTransferResultsHeader.querySelectorAll("[data-transfer-column]").forEach((column, index) => {
      column.hidden = !visible.includes(index);
    });
  };

  const renderTransferPanel = () => {
    transferLastRenderAt = Date.now();
    const counts = {
      queue: transferQueue.length + (activeTransferId && activeTransferTask ? 1 : 0)
        + remoteEditTransfers.size,
      failed: transferResults.filter(result => result.state === "error"
        || result.state === "cancelled").length,
      success: transferResults.filter(result => result.state === "success").length
    };
    const failedCount = counts.failed;
    if (sftpTransferRetryFailedButton)
      sftpTransferRetryFailedButton.disabled = failedCount === 0;
    const sftpTab = functionTabs.querySelector('[data-panel="sftp"]');
    const busyCount = counts.queue;
    const hasFailures = counts.failed > 0;
    if (sftpFunctionBadge) {
      sftpFunctionBadge.hidden = busyCount === 0 && !hasFailures;
      sftpFunctionBadge.textContent = busyCount > 0
        ? String(Math.min(99, busyCount)) : "!";
      sftpFunctionBadge.title = busyCount > 0
        ? `${busyCount} 个传输任务${hasFailures ? `，${counts.failed} 个失败` : ""}`
        : `${counts.failed} 个失败任务`;
    }
    sftpTab?.classList.toggle("transfer-active", busyCount > 0);
    sftpTab?.classList.toggle("transfer-error", hasFailures);
    sftpTransferResults.hidden = false;
    const headers = activeTransferTab === "queue"
      ? ["状态", "文件名", "方向", "大小", "速度", "操作"]
      : activeTransferTab === "failed"
        ? ["状态", "文件名", "方向", "大小", "失败时间", "失败原因", "操作"]
        : ["状态", "文件名", "方向", "大小", "完成时间", "速度", "状态"];
    const headerColumns = [...sftpTransferResultsHeader.querySelectorAll("[data-transfer-column]")];
    const visibleHeaderIndexes = activeTransferTab === "queue" ? [0, 1, 2, 3, 5, 6] : [0, 1, 2, 3, 4, 5, 6];
    headerColumns.forEach((column, index) => {
      const textNode = [...column.childNodes].find(node => node.nodeType === Node.TEXT_NODE);
      if (textNode) textNode.textContent = headers[visibleHeaderIndexes.indexOf(index)] || "";
    });
    applyTransferGridColumns();
    sftpTransferTabs.forEach(tab => {
      const key = tab.dataset.transferTab;
      tab.classList.toggle("active", key === activeTransferTab);
      tab.setAttribute("aria-selected", String(key === activeTransferTab));
      tab.querySelector("span").textContent = `(${counts[key]})`;
    });
    let rows = [];
    if (activeTransferTab === "queue") {
      if (activeTransferId && activeTransferTask)
        rows.push({ ...activeTransferTask, state: "active", active: true, queueIndex: -1 });
      rows.push(...transferQueue.map((task, index) => ({ ...task, state: task.paused ? "paused" : "queued", queueIndex: index })));
      rows.push(...Array.from(remoteEditTransfers.entries()).map(([key, task]) => ({
        ...task, state: "active", active: true, remoteEdit: true, queueIndex: `edit:${key}`
      })));
    }
    else
      rows = transferResults.map((result, resultIndex) => ({ ...result, resultIndex }))
        .filter(result => result.state === activeTransferTab
          || (activeTransferTab === "failed" && result.state === "cancelled"));
    rows = rows.filter(transferMatchesFilter);
    const rowElements = rows.map((result, index) => {
      const item = document.createElement("div");
      const rowState = result.active && activeTransferPaused ? "paused" : result.state;
      item.className = `sftp-transfer-result ${rowState}`;
      item.dataset.transferKind = activeTransferTab;
      item.dataset.transferIndex = String(result.queueIndex ?? result.resultIndex ?? index);
      const targetKey = `${activeTransferTab}:${item.dataset.transferIndex}`;
      item.classList.toggle("selected", selectedTransferTarget?.key === targetKey);
      item.addEventListener("click", () => {
        selectedTransferTarget = {
          key: targetKey,
          kind: activeTransferTab,
          index: item.dataset.transferIndex
        };
        renderTransferPanel();
      });
      item.addEventListener("dblclick", () => {
        if (activeTransferTab === "failed")
          openTransferFailureDetails(result);
      });
      const icon = document.createElement("span");
      icon.className = "sftp-transfer-result-state";
      icon.textContent = result.state === "success" ? "✓"
        : (result.state === "error" || result.state === "cancelled") ? "✕" : "•";
      const direction = document.createElement("span");
      direction.className = "sftp-transfer-result-direction";
      direction.textContent = transferDirection(result);
      const name = document.createElement("span");
      name.className = "sftp-transfer-result-name";
      name.textContent = transferTaskName(result);
      if (result.active) {
        const progress = document.createElement("div");
        progress.className = "sftp-transfer-inline-progress";
        const track = document.createElement("div");
        track.className = "sftp-progress-track";
        const bar = document.createElement("div");
        bar.id = "sftp-inline-progress-bar";
        const inlineDone = result.fileTotal > 0 ? Number(result.fileDone || 0) : Number(result.done || 0);
        const inlineTotal = result.fileTotal > 0 ? Number(result.fileTotal) : Number(result.total || 0);
        const inlinePercent = inlineTotal > 0 ? Math.floor(inlineDone * 100 / inlineTotal) : 0;
        bar.style.width = `${inlinePercent}%`;
        track.appendChild(bar);
        const percent = document.createElement("span");
        percent.textContent = `${inlinePercent}%`;
        progress.append(track, percent);
        name.replaceChildren(document.createTextNode(transferTaskName(result)), progress);
      }
      const size = document.createElement("span");
      size.className = "sftp-transfer-result-size";
      size.textContent = transferTaskSize(result) > 0 ? formatSize(transferTaskSize(result)) : "—";
      const time = document.createElement("span");
      time.className = "sftp-transfer-result-time";
      time.textContent = formatTransferTime(result.completedAt);
      time.hidden = activeTransferTab === "queue";
      const speed = document.createElement("span");
      speed.className = "sftp-transfer-result-speed";
      speed.textContent = formatResultSpeed(result.speed);
      const status = document.createElement("span");
      status.className = "sftp-transfer-result-status";
      status.textContent = result.state === "error"
        ? result.message || "传输失败"
        : result.state === "cancelled"
          ? result.message || "已取消"
          : transferStateLabel(result.state);
      if (result.active) {
        if (result.remoteEdit) {
          const cancel = document.createElement("button");
          cancel.className = "sftp-transfer-active-cancel";
          cancel.type = "button";
          cancel.textContent = "停止同步";
          cancel.addEventListener("click", event => {
            event.stopPropagation();
            stopRemoteEditTask(result);
          });
          status.replaceChildren(cancel);
        } else {
          const pause = document.createElement("button");
          pause.className = "sftp-transfer-active-pause";
          pause.type = "button";
          pause.textContent = activeTransferPaused ? "继续" : "暂停";
          pause.addEventListener("click", event => {
            event.stopPropagation();
            toggleActiveTransferPause();
          });
          const cancel = document.createElement("button");
          cancel.className = "sftp-transfer-active-cancel";
          cancel.type = "button";
          cancel.textContent = "取消";
          cancel.addEventListener("click", event => {
            event.stopPropagation();
            cancelSftpTransfer();
          });
          status.replaceChildren(pause, cancel);
        }
      } else if (result.state === "queued" || result.state === "paused") {
        const pause = document.createElement("button");
        pause.className = "sftp-transfer-active-cancel";
        pause.type = "button";
        pause.textContent = result.state === "paused" ? "继续" : "暂停";
        pause.addEventListener("click", event => {
          event.stopPropagation();
          const task = transferQueue[result.queueIndex];
          if (task) {
            task.paused = !task.paused;
            renderTransferPanel();
            pumpTransferQueue();
          }
        });
        status.replaceChildren(pause);
      }
      item.append(icon, name, direction, size, time, speed, status);
      item.title = result.message || `${direction.textContent} ${name.textContent}：${status.textContent}`;
      return item;
    });
    if (!rowElements.length) {
      const empty = document.createElement("div");
      empty.className = "sftp-transfer-empty";
      empty.textContent = transferFilterText.trim()
        ? "没有匹配的传输任务"
        : activeTransferTab === "queue" ? "当前没有待处理的传输任务"
          : activeTransferTab === "failed" ? "当前没有失败的传输任务" : "当前没有成功的传输记录";
      rowElements.push(empty);
    }
    sftpTransferResultsList.replaceChildren(...rowElements);
    scheduleTransferPersist();
  };

  const recordTransferResult = (state, task, message = "", details = {}) => {
    transferResults.push({ ...task, size: transferTaskSize(task), completedAt: Date.now(),
      speed: 0, ...details, state, name: transferTaskName(task), message });
    if (transferResults.length > 20)
      transferResults.splice(0, transferResults.length - 20);
    renderTransferPanel();
  };

  const closeTransferContextMenu = () => {
    const wasVisible = hideVisibleElement(sftpTransferContextMenu);
    transferContextTarget = null;
    if (wasVisible) clearRdpOverlay();
  };
  const retryTransferTask = task => {
    if (!task) return;
    if (task.type === "edit-sync") {
      const key = `${task.profileIndex}:${task.remotePath}`;
      remoteEditTransfers.set(key, { ...task, state: "active" });
      renderTransferPanel();
      post("sftp.remoteEdit.retry", {
        index: task.profileIndex, remotePath: task.remotePath
      }).catch(error => {
        remoteEditTransfers.delete(key);
        recordTransferResult("error", task,
          error instanceof Error ? error.message : String(error));
      });
    } else if (task.type === "upload")
      startSftpUpload(task.path, task.temporary, true, task.conflict,
        task.directory, task.remoteDirectory, task.remoteName,
        task.syncMode || false,
        task.directory ? 0 : (Number(task.done) > 0 ? Number(task.done) : 0),
        task.verify || false, task.index);
    else if (task.type === "stream-upload")
      startDroppedFileStream(task.file, task.remoteDirectory, task.conflict, true, task.batch);
    else
      startSftpDownload(task.entry, true, task.destination, task.conflict,
        task.entry && task.entry.directory
          ? 0 : (Number(task.done) > 0 ? Number(task.done) : 0),
        task.verify || false, task.index);
  };
  const stopRemoteEditTask = task => {
    if (!task || task.type !== "edit-sync") return;
    const key = `${task.profileIndex}:${task.remotePath}`;
    post("sftp.remoteEdit.stop", {
      index: task.profileIndex, remotePath: task.remotePath
    }).then(() => {
      remoteEditTransfers.delete(key);
      recordTransferResult("cancelled", task, "已停止跟踪远程编辑副本");
    }).catch(reportError);
  };
  const openRemoteEditDirectory = task => {
    if (!task || task.type !== "edit-sync") return;
    post("sftp.remoteEdit.openDirectory", {
      index: task.profileIndex, remotePath: task.remotePath
    }).catch(reportError);
  };
  const openTransferFailureDetails = result => {
    if (!result) return;
    const reason = result.message
      || (result.state === "cancelled" ? "用户取消" : "传输失败");
    const localPath = result.path || result.destination || result.file?.name || "—";
    const remotePath = result.remotePath || result.remoteDirectory
      || result.entry?.path || result.entry?.name || "—";
    const checkpoint = result.checkpoint?.files
      ? `已记录文件进度：${Object.keys(result.checkpoint.files).length} 项`
      : result.done > 0 ? `已传输：${formatSize(result.done)}` : "未记录断点";
    showActionDialog({
      title: "传输详情",
      message:
        `文件名：${result.name || "未知"}\n`
        + `方向：${transferDirection(result)}\n`
        + `大小：${result.size > 0 ? formatSize(result.size) : "—"}\n`
        + `时间：${formatTransferTime(result.completedAt)}\n`
        + `状态：${transferStateLabel(result.state)}\n`
        + `本地路径：${localPath}\n`
        + `远程路径：${remotePath}\n`
        + `恢复信息：${checkpoint}\n`
        + `原因：${reason}`,
      value: null,
      buttons: [{ action: "ok", label: "确定", primary: true }]
    });
  };
  const retryAllFailedTransfers = () => {
    const failed = transferResults.filter(isFailedTransfer).map(result => ({ ...result }));
    if (!failed.length) return false;
    transferResults.splice(0, transferResults.length,
      ...transferResults.filter(result => !isFailedTransfer(result)));
    failed.forEach(retryTransferTask);
    sftpTransferStatusText.textContent = `已重新加入 ${failed.length} 个失败任务`;
    renderTransferPanel();
    pumpTransferQueue();
    return true;
  };
  const executeTransferContextAction = action => {
    const target = transferContextTarget;
    closeTransferContextMenu();
    if (action === "pause-all") {
      transferQueuePaused = true;
      transferQueue.forEach(task => { task.paused = true; });
      if (activeTransferId && !activeTransferPaused)
        post("sftp.pause", { transferId: activeTransferId }).catch(reportError);
      sftpTransferStatusText.textContent = "传输队列已暂停";
      renderTransferPanel();
      return;
    }
    if (action === "resume-all") {
      transferQueuePaused = false;
      transferQueue.forEach(task => { task.paused = false; });
      sftpTransferStatusText.textContent = "传输队列已继续";
      renderTransferPanel();
      pumpTransferQueue();
      return;
    }
    if (action === "retry-failed-all") {
      retryAllFailedTransfers();
      return;
    }
    if (action === "delete-all") {
      if (activeTransferTab === "queue") transferQueue.splice(0);
      else transferResults.splice(0, transferResults.length,
        ...transferResults.filter(result => result.state !== activeTransferTab
          && !(activeTransferTab === "failed" && result.state === "cancelled")));
      renderTransferPanel();
      persistTransferQueue();
      return;
    }
    if (!target) return;
    const remoteEditTask = target.kind === "queue" && String(target.index).startsWith("edit:")
      ? remoteEditTransfers.get(String(target.index).slice(5))
      : target.kind !== "queue" ? transferResults[Number(target.index)] : null;
    if (remoteEditTask?.type === "edit-sync") {
      if (action === "sync" || action === "retry") retryTransferTask(remoteEditTask);
      else if (action === "open-folder") openRemoteEditDirectory(remoteEditTask);
      else if (action === "stop-tracking" || action === "cancel") stopRemoteEditTask(remoteEditTask);
      return;
    }
    if (target.kind === "queue") {
      if (String(target.index).startsWith("edit:")) {
        const key = String(target.index).slice(5);
        const task = remoteEditTransfers.get(key);
        if (task && action === "cancel") stopRemoteEditTask(task);
        return;
      }
      const index = Number(target.index);
      if (index < 0) {
        if (action === "cancel") cancelSftpTransfer();
        return;
      }
      if (action === "cancel") {
        const [task] = transferQueue.splice(index, 1);
        if (task) {
          // A paused task may still hold a suspended worker process.
          if (task.resumeTransferId)
            post("sftp.cancel", { transferId: task.resumeTransferId })
              .catch(() => {});
          recordTransferResult("cancelled", task, "用户取消");
        }
      } else if (action === "pause") {
        const task = transferQueue[index];
        if (task) task.paused = !task.paused;
        renderTransferPanel();
        pumpTransferQueue();
        return;
      } else if (action === "up" && index > 0) {
        [transferQueue[index - 1], transferQueue[index]] =
          [transferQueue[index], transferQueue[index - 1]];
      } else if (action === "down" && index < transferQueue.length - 1) {
        [transferQueue[index + 1], transferQueue[index]] =
          [transferQueue[index], transferQueue[index + 1]];
      }
      renderTransferPanel();
      persistTransferQueue();
      return;
    }
    const result = transferResults[target.index];
    if (!result) return;
    if (action === "retry"
        && (result.state === "error" || result.state === "cancelled")) {
      transferResults.splice(target.index, 1);
      renderTransferPanel();
      retryTransferTask(result);
    } else if (action === "details") {
      openTransferFailureDetails(result);
    }
  };

  const formatConflictSide = (label, entry) => {
    if (!entry) return `${label}：无法读取元数据`;
    const isDirectory = Boolean(entry.directory || entry.isDirectory);
    const rawSize = entry.size ?? entry.file?.size;
    const size = isDirectory ? "文件夹" : Number.isFinite(Number(rawSize))
      ? formatSize(Number(rawSize)) : "未知";
    const rawModified = entry.modified ?? entry.lastModified
      ?? entry.file?.lastModified;
    const modified = Number.isFinite(Number(rawModified)) && Number(rawModified) > 0
      ? new Date(Number(rawModified) < 100000000000
        ? Number(rawModified) * 1000 : Number(rawModified)).toLocaleString()
      : "未知";
    return `${label}：大小 ${size}，修改时间 ${modified}`;
  };
  const formatConflictDetails = (localEntry, remoteEntry) =>
    `比较信息\n${formatConflictSide("本机", localEntry)}\n${formatConflictSide("远端", remoteEntry)}`;

  const chooseConflictPolicy = async (name, targetLabel, details = "") => {
    const result = await showActionDialog({
      title: "发现同名文件",
      message: `${targetLabel}已存在同名项目“${name}”。\n\n`
        + `${details || "比较信息：元数据暂不可用"}\n\n`
        + "请比较后选择处理方式；批量任务可将选择应用到全部冲突。",
      value: null,
      buttons: [
        { action: "skip", label: "跳过" },
        { action: "rename", label: "自动重命名" },
        { action: "overwrite", label: "覆盖", primary: true },
        { action: "overwrite-all", label: "全部覆盖" }
      ]
    });
    return result.action === "overwrite" || result.action === "rename"
      || result.action === "overwrite-all"
      ? result.action : null;
  };

  const resetConflictPolicyForNewSession = () => {
    if (!activeTransferId && !transferStarting && !transferQueue.length)
      transferConflictPolicy = null;
  };

  const resolveTransferConflict = async (
      name, targetLabel, current = "", details = "") => {
    if (transferConflictPolicy) return transferConflictPolicy;
    if (current && current !== "ask") return current;
    const conflict = await chooseConflictPolicy(name, targetLabel, details);
    if (!conflict) return null;
    if (conflict === "overwrite-all") {
      transferConflictPolicy = "overwrite";
      return "overwrite";
    }
    return conflict;
  };

  const localJoinPath = (directory, name) => {
    const separator = String(directory).includes("\\") ? "\\" : "/";
    return `${String(directory).replace(/[\\/]+$/, "")}${separator}${name}`;
  };

  const flushTransferRefresh = () => {
    if (activeTransferId || transferStarting || transferQueue.length) return;
    if (refreshRemoteAfterTransfers && sftpProfile)
      loadSftpDirectory(sftpPath, false);
    if (refreshLocalAfterTransfers && localPanelVisible && localLoaded)
      loadLocalDirectory(localPath, false);
    refreshRemoteAfterTransfers = false;
    refreshLocalAfterTransfers = false;
  };

  const pumpTransferQueue = () => {
    if (activeTransferId || transferStarting || transferQueuePaused) return;
    if (!transferQueue.length) {
      flushTransferRefresh();
      return;
    }
    const nextIndex = transferQueue.findIndex(task => !task.paused);
    if (nextIndex < 0) return;
    const [next] = transferQueue.splice(nextIndex, 1);
    renderTransferPanel();
    if (next.resumeTransferId) {
      resumePausedTransfer(next);
      return;
    }
    if (next.type === "upload")
      startSftpUpload(next.path, next.temporary, false, next.conflict,
        next.directory, next.remoteDirectory, next.remoteName,
        next.syncMode, 0, next.verify || false, next.index,
        !!next.resumeDirectory, next.checkpoint || null);
    else if (next.type === "stream-upload")
      startDroppedFileStream(
        next.file, next.remoteDirectory, next.conflict, false, next.batch);
    else
      startSftpDownload(next.entry, false, next.destination, next.conflict,
        0, next.verify || false, next.index,
        !!next.resumeDirectory, next.checkpoint || null);
  };

  // Re-attaches a paused transfer to its still-suspended worker so it
  // continues from the exact byte where it stopped.
  const resumePausedTransfer = task => {
    const transferId = task.resumeTransferId;
    const name = transferTaskName(task);
    post("sftp.resume", { transferId }).then(() => {
      activeTransferId = transferId;
      activeTransferOperation = task.type === "download" ? "download" : "upload";
      activeTransferTask = task;
      activeTransferPaused = false;
      activeTransferBatch = task.batch || null;
      transferStartedAt.set(transferId, Date.now());
      updateTransferControls();
      renderTransferPanel();
      sftpTransferStatusText.textContent = `已继续传输：${compactTransferName(name)}`;
    }).catch(() => {
      sftpTransferStatusText.textContent =
        `无法继续传输，重新开始：${compactTransferName(name)}`;
      if (task.type === "stream-upload")
        startDroppedFileStream(task.file, task.remoteDirectory, task.conflict,
          false, task.batch);
      else if (task.type === "upload")
        startSftpUpload(task.path, task.temporary, false, task.conflict,
          task.directory, task.remoteDirectory, task.remoteName,
          task.syncMode || false, 0, task.verify || false, task.index,
          !!task.resumeDirectory, task.checkpoint || null);
      else
        startSftpDownload(task.entry, false, task.destination, task.conflict,
          0, task.verify || false, task.index,
          !!task.resumeDirectory, task.checkpoint || null);
    });
  };

  const retryTransferOrFinish = error => {
    const task = activeTransferTask;
    if (task && transferRetryCount < maximumTransferRetries) {
      transferRetryCount += 1;
      const attempt = transferRetryCount;
      sftpTransferStatusText.textContent =
        `传输失败，正在重试（${attempt}/${maximumTransferRetries}）：${compactTransferName(task.name)}`;
      setTimeout(() => {
        if (task.type === "upload")
          startSftpUpload(task.path, task.temporary, false, task.conflict,
            task.directory, task.remoteDirectory, task.remoteName,
            task.syncMode || false,
            task.directory ? 0 : (Number(task.done) > 0 ? Number(task.done) : 0),
            task.verify || false, task.index,
            !!task.directory, task.checkpoint || null);
        else if (task.type === "stream-upload")
          startDroppedFileStream(task.file, task.remoteDirectory, task.conflict,
            false, task.batch);
        else
          startSftpDownload(task.entry, false, task.destination, task.conflict,
            task.entry && task.entry.directory
              ? 0 : (Number(task.done) > 0 ? Number(task.done) : 0),
            task.verify || false, task.index,
            !!task.entry?.directory, task.checkpoint || null);
      }, 300);
      return true;
    }
    transferRetryCount = 0;
    activeTransferTask = null;
    const message = error?.message || String(error || "SFTP 传输失败");
    recordTransferResult("error", task, message);
    sftpTransferStatusText.textContent = message;
    reportError(sftpTransferStatusText.textContent);
    pumpTransferQueue();
    return false;
  };

  const startSftpUpload = async (localFilePath = "", temporary = false, allowQueue = true,
                                 conflict = "", directory = false,
                                 remoteDirectory = "", remoteName = "",
                                 syncMode = false, resumeOffset = 0,
                                 verify = false, profileIndex = null,
                                 resumeDirectory = false, checkpoint = null) => {
    if (!sftpProfile && profileIndex == null) return;
    resetConflictPolicyForNewSession();
    if ((activeTransferId || transferStarting) && localFilePath && allowQueue) {
      transferQueue.push({
        type: "upload", path: localFilePath, temporary, conflict, directory,
        remoteDirectory, remoteName, syncMode, verify,
        resumeDirectory: !!resumeDirectory, checkpoint,
        index: profileIndex ?? sftpProfile?.index ?? -1
      });
      renderTransferPanel();
      sftpTransferStatusText.textContent = `已加入上传队列（剩余 ${transferQueue.length} 项）`;
      return;
    }
    if (activeTransferId || transferStarting) return;
    if (conflict === "skip") {
      recordTransferResult("skipped", { name: remoteName
        || String(localFilePath).split(/[\\/]/).at(-1) || "文件" });
      sftpTransferStatusText.textContent = `已跳过上传：${remoteName
        || String(localFilePath).split(/[\\/]/).at(-1) || "文件"}`;
      pumpTransferQueue();
      return;
    }
    transferStarting = true;
    activeTransferBatch = null;
    updateTransferControls();
    if (localFilePath && (!conflict || conflict === "ask")) {
      const fileName = remoteName
        || String(localFilePath).split(/[\\/]/).at(-1);
      const exists = [...sftpEntriesByPath.values()]
        .some(entry => entry.name === fileName);
      if (exists) {
        const remoteEntry = [...sftpEntriesByPath.values()].find(entry =>
          entry.name.localeCompare(fileName, undefined,
            { sensitivity: "accent" }) === 0);
        const localEntry = localEntriesByPath.get(localFilePath);
        conflict = await resolveTransferConflict(
          fileName, "当前远程目录中", conflict,
          formatConflictDetails(localEntry, remoteEntry));
        if (!conflict) {
          if (temporary)
            post("local.remove", { paths: [localFilePath] }).catch(() => {});
          sftpTransferStatusText.textContent = `已跳过上传：${fileName}`;
          transferStarting = false;
          updateTransferControls();
          pumpTransferQueue();
          return;
        }
      } else {
        conflict = "overwrite";
      }
    }
    const uploadDisplayName = remoteName
      || String(localFilePath).split(/[\\/]/).at(-1) || "文件";
    if (!activeTransferTask) transferRetryCount = 0;
    activeTransferTask = {
      type: "upload", name: uploadDisplayName, path: localFilePath, temporary,
      conflict: conflict || "overwrite", directory, remoteDirectory, remoteName,
      index: profileIndex ?? sftpProfile?.index ?? -1,
      resumeDirectory: !!resumeDirectory,
      checkpoint: checkpoint && typeof checkpoint === "object"
        ? checkpoint : { version: 1, files: {} }
    };
    sftpTransferStatusText.textContent = localFilePath
      ? `正在准备上传：${compactTransferName(uploadDisplayName)}`
      : "请选择要上传的本地文件…";
    sftpTransferStatusText.title = localFilePath ? uploadDisplayName : "";
    post("sftp.upload", {
      operation: directory ? "upload-tree" : "upload",
      index: profileIndex ?? sftpProfile.index,
      remotePath: remoteDirectory || sftpPath,
      localPath: localFilePath,
      remoteName,
temporary,
      conflict: conflict || "overwrite",
      syncMode: syncMode || undefined,
      ...(!directory && resumeOffset > 0 ? { resumeOffset } : {}),
      resumeDirectory: !!directory && !!resumeDirectory,
      verifyChecksum: verify || undefined
    }).then(result => {
      transferStarting = false;
      if (result.cancelled) {
        if (temporary && localFilePath)
          post("local.remove", { paths: [localFilePath] }).catch(() => {});
        sftpTransferStatusText.textContent = "已取消上传";
        updateTransferControls();
        pumpTransferQueue();
        return;
      }
      activeTransferId = result.transferId;
      activeTransferOperation = "upload";
      activeTransferPaused = false;
      transferStartedAt.set(activeTransferId, Date.now());
      updateTransferControls();
      renderTransferPanel();
    }).catch(error => {
      transferStarting = false;
      if (temporary && localFilePath)
        post("local.remove", { paths: [localFilePath] }).catch(() => {});
      updateTransferControls();
      retryTransferOrFinish(error);
    });
  };

  const startSftpDownload = async (entry, allowQueue = true, requestedDestination = "",
                                   conflict = "", resumeOffset = 0, verify = false,
                                   profileIndex = null, resumeDirectory = false,
                                   checkpoint = null) => {
    if ((!sftpProfile && profileIndex == null) || !entry) return;
    resetConflictPolicyForNewSession();
    const destination = requestedDestination || (localPanelVisible && localPath
      ? (entry.directory ? localPath : localJoinPath(localPath, entry.name)) : "");
    if ((activeTransferId || transferStarting) && allowQueue) {
      transferQueue.push({
        type: "download", entry, destination, conflict, verify,
        directory: !!entry.directory, resumeDirectory: !!resumeDirectory,
        checkpoint,
        index: profileIndex ?? sftpProfile?.index ?? -1
      });
      renderTransferPanel();
      sftpTransferStatusText.textContent = `已加入下载队列（剩余 ${transferQueue.length} 项）`;
      return;
    }
    if (activeTransferId || transferStarting) return;
    if (conflict === "skip") {
      recordTransferResult("skipped", entry);
      sftpTransferStatusText.textContent = `已跳过下载：${entry.name}`;
      pumpTransferQueue();
      return;
    }
    transferStarting = true;
    activeTransferBatch = null;
    updateTransferControls();
    if (destination && (!conflict || conflict === "ask")) {
      const conflictTarget = entry.directory
        ? localJoinPath(destination, entry.name) : destination;
      const exists = conflict === "ask" || [...localEntriesByPath.values()].some(item =>
        item.path.localeCompare(conflictTarget, undefined, { sensitivity: "accent" }) === 0);
      if (exists) {
        const localEntry = [...localEntriesByPath.values()].find(item =>
          item.path.localeCompare(conflictTarget, undefined,
            { sensitivity: "accent" }) === 0);
        conflict = await resolveTransferConflict(
          entry.name, "目标本机目录中", conflict,
          formatConflictDetails(localEntry, entry));
        if (!conflict) {
          sftpTransferStatusText.textContent = `已跳过下载：${entry.name}`;
          transferStarting = false;
          updateTransferControls();
          pumpTransferQueue();
          return;
        }
      } else {
        conflict = "overwrite";
      }
    }
    sftpTransferStatusText.textContent = destination
      ? `正在下载到：${destination}` : `请选择保存位置：${entry.name}`;
    if (!activeTransferTask) transferRetryCount = 0;
    activeTransferTask = {
      type: "download", name: entry.name, entry, destination,
      conflict: conflict || "overwrite",
      index: profileIndex ?? sftpProfile?.index ?? -1,
      directory: !!entry.directory, resumeDirectory: !!resumeDirectory,
      checkpoint: checkpoint && typeof checkpoint === "object"
        ? checkpoint : { version: 1, files: {} }
    };
    post("sftp.download", {
      operation: entry.directory ? "download-dir" : "download",
      index: profileIndex ?? sftpProfile.index,
      remotePath: entry.path,
      localPath: destination,
      conflict: conflict || "overwrite",
      ...(!entry.directory && resumeOffset > 0 ? { resumeOffset } : {}),
      resumeDirectory: !!entry.directory && !!resumeDirectory,
      verifyChecksum: verify || undefined
    }).then(result => {
      transferStarting = false;
      if (result.cancelled) {
        sftpTransferStatusText.textContent = "已取消下载";
        updateTransferControls();
        pumpTransferQueue();
        return;
      }
      activeTransferId = result.transferId;
      activeTransferOperation = "download";
      activeTransferPaused = false;
      transferStartedAt.set(activeTransferId, Date.now());
      updateTransferControls();
      renderTransferPanel();
    }).catch(error => {
      transferStarting = false;
      updateTransferControls();
      retryTransferOrFinish(error);
    });
  };

  const downloadSelectedRemoteEntries = async entries => {
    if (!entries.length) return;
    resetConflictPolicyForNewSession();
    if (entries.length === 1) {
      startSftpDownload(entries[0]);
      return;
    }
    try {
      const result = await post("local.chooseDirectory", {
        path: localPanelVisible ? localPath : ""
      });
      if (result.cancelled) {
        sftpTransferStatusText.textContent = "已取消批量下载";
        return;
      }
      const destinationDirectory = result.path;
      const existingNames = new Set((result.entries || [])
        .map(entry => String(entry.name).toLocaleLowerCase()));
      const conflictEntry = entries.find(entry =>
        existingNames.has(String(entry.name).toLocaleLowerCase()));
      const startBatch = policy => entries.forEach(entry => {
        const exists = existingNames.has(String(entry.name).toLocaleLowerCase());
        startSftpDownload(entry, true,
          entry.directory
            ? destinationDirectory
            : localJoinPath(destinationDirectory, entry.name),
          exists ? policy : "overwrite");
      });
      if (!conflictEntry) {
        startBatch("overwrite");
        return;
      }
      const localConflict = (result.entries || []).find(entry =>
        entry.name.localeCompare(conflictEntry.name, undefined,
          { sensitivity: "accent" }) === 0);
      const policy = await chooseConflictPolicy(
        conflictEntry.name, "目标本机目录中",
        formatConflictDetails(localConflict, conflictEntry));
      if (policy === "overwrite-all") {
        transferConflictPolicy = "overwrite";
        startBatch("overwrite");
        return;
      }
      startBatch(policy || "skip");
    } catch (error) {
      sftpTransferStatusText.textContent = error.message;
      reportError(error);
    }
  };

  const chooseAndStartSftpUpload = async () => {
    if (!sftpProfile) return;
    try {
      const result = await post("local.chooseFile", {
        path: localPanelVisible ? localPath : ""
      });
      if (!result.cancelled)
        startSftpUpload(result.path);
    } catch (error) {
      sftpTransferStatusText.textContent = error.message;
      reportError(error);
    }
  };

  const cancelSftpTransfer = () => {
    if (!activeTransferId) return;
    post("sftp.cancel", { transferId: activeTransferId }).catch(reportError);
  };

  const toggleActiveTransferPause = () => {
    if (!activeTransferId) return;
    post(activeTransferPaused ? "sftp.resume" : "sftp.pause",
      { transferId: activeTransferId }).catch(reportError);
  };

  const bytesToBase64 = bytes => {
    let binary = "";
    for (let offset = 0; offset < bytes.length; offset += 0x8000)
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    return btoa(binary);
  };

  const delay = milliseconds => new Promise(resolve =>
    setTimeout(resolve, milliseconds));

  const startDroppedFileStream = async (
      file, remoteDirectory = "", conflict = "", allowQueue = true,
      batch = null) => {
    if (!sftpProfile || !file) return;
    resetConflictPolicyForNewSession();
    if ((activeTransferId || transferStarting) && allowQueue) {
      transferQueue.push({
        type: "stream-upload", file, remoteDirectory, conflict, batch
      });
      renderTransferPanel();
      return;
    }
    if (activeTransferId || transferStarting) return;
    transferStarting = true;
    activeTransferPaused = false;
    activeTransferBatch = batch;
    updateTransferControls();
    const targetDirectory = remoteDirectory || sftpPath;
    if ((!conflict || conflict === "ask") && targetDirectory === sftpPath) {
      const exists = [...sftpEntriesByPath.values()]
        .some(entry => entry.name === file.name);
      if (exists) {
        const remoteEntry = [...sftpEntriesByPath.values()].find(entry =>
          entry.name.localeCompare(file.name, undefined,
            { sensitivity: "accent" }) === 0);
        conflict = await resolveTransferConflict(
          file.name, "当前远程目录中", conflict,
          formatConflictDetails(file, remoteEntry));
        if (!conflict) {
          transferStarting = false;
          activeTransferBatch = null;
          updateTransferControls();
          sftpTransferStatusText.textContent = `已跳过上传：${file.name}`;
          pumpTransferQueue();
          return;
        }
      } else {
        conflict = "overwrite";
      }
    }
    const batchLabel = transferBatchLabel(batch);
    if (!activeTransferTask) transferRetryCount = 0;
    activeTransferTask = {
      type: "stream-upload", name: file.name, file, remoteDirectory: targetDirectory,
      conflict: conflict || "overwrite", batch
    };
    sftpTransferStatusText.textContent = batchLabel
      ? `正在上传（${batchLabel}）：${compactTransferName(file.name)}`
      : `正在上传：${compactTransferName(file.name)}`;
    sftpTransferStatusText.title = file.name;
    let transferId = "";
    try {
      const begin = await post("sftp.upload", {
        operation: "upload-stream",
        index: sftpProfile.index,
        remotePath: targetDirectory,
        remoteName: file.name,
        size: file.size,
        conflict: conflict || "overwrite"
      });
      transferId = begin.transferId;
      transferStarting = false;
      activeTransferId = transferId;
      activeTransferOperation = "upload";
      transferStartedAt.set(transferId, Date.now());
      updateTransferControls();
      renderTransferPanel();
      if (batchLabel)
        if (sftpProgressPercent)
          sftpProgressPercent.textContent = `0% · ${batch.index}/${batch.total}`;
      const chunkSize = 256 * 1024;
      const maximumBufferedBytes = 2 * 1024 * 1024;
      let interrupted = false;
      for (let offset = 0; offset < file.size; offset += chunkSize) {
        if (activeTransferId !== transferId) { interrupted = true; break; }
        const buffer = await file.slice(offset, offset + chunkSize).arrayBuffer();
        if (activeTransferId !== transferId) { interrupted = true; break; }
        let streamState = await post("sftp.stream.chunk", {
          transferId,
          data: bytesToBase64(new Uint8Array(buffer))
        });
        while (Number(streamState.buffered || 0) > maximumBufferedBytes) {
          if (activeTransferId !== transferId) { interrupted = true; break; }
          await delay(25);
          streamState = await post("sftp.stream.status", { transferId });
        }
        if (interrupted) break;
      }
      // A paused task was re-queued (its worker already released); do not
      // finalize the suspended stream, otherwise the partial file would be
      // reported as a completed transfer.
      if (!interrupted)
        await post("sftp.stream.finish", { transferId });
    } catch (error) {
      transferStarting = false;
      if (transferId && activeTransferId === transferId)
        post("sftp.cancel", { transferId }).catch(() => {});
      if (!transferId || activeTransferId === transferId) {
        sftpTransferStatusText.textContent = `上传失败：${error.message}`;
        reportError(error);
      }
      if (!activeTransferId)
        activeTransferBatch = null;
      updateTransferControls();
      // Only retry a genuine failure.  A cancelled transfer has already been
      // handled by the "cancelled" event (which clears activeTransferId), so
      // it must not be retried or recorded as an error here.
      if (!transferId || activeTransferId === transferId)
        retryTransferOrFinish(error);
    }
  };

  const hasDraggedFiles = dataTransfer =>
    Array.from(dataTransfer?.types || []).includes("Files");
  const hasDraggedType = (dataTransfer, type) =>
    Array.from(dataTransfer?.types || []).includes(type);

  const fileFromEntry = entry => new Promise((resolve, reject) =>
    entry.file(resolve, reject));

  const readDirectoryEntries = async entry => {
    const reader = entry.createReader();
    const entries = [];
    while (true) {
      const batch = await new Promise((resolve, reject) =>
        reader.readEntries(resolve, reject));
      if (!batch.length) return entries;
      entries.push(...batch);
    }
  };

  const uniqueRemoteEntryName = name => {
    const names = new Set([...sftpEntriesByPath.values()]
      .map(entry => String(entry.name).toLocaleLowerCase()));
    if (!names.has(String(name).toLocaleLowerCase())) return name;
    for (let index = 1; index < 10000; index += 1) {
      const candidate = `${name} (${index})`;
      if (!names.has(candidate.toLocaleLowerCase())) return candidate;
    }
    return `${name}-${Date.now()}`;
  };

  const uploadDroppedDirectory = async entry => {
    let remoteName = entry.name;
    let conflict = "overwrite";
    const existing = [...sftpEntriesByPath.values()].find(item =>
      item.name.localeCompare(remoteName, undefined, { sensitivity: "accent" }) === 0);
    if (existing) {
      conflict = await resolveTransferConflict(
        remoteName, "当前远程目录中", conflict,
        formatConflictDetails(entry, existing));
      if (!conflict) {
        sftpTransferStatusText.textContent = `已跳过上传：${remoteName}`;
        return;
      }
      if (conflict === "rename") {
        remoteName = uniqueRemoteEntryName(remoteName);
        conflict = "overwrite";
      }
    }

    const remoteRoot = normalizeRemotePath(`${sftpPath}/${remoteName}`);
    await post("sftp.operation", {
      operation: "mkdir-p",
      index: sftpProfile.index,
      path: remoteRoot,
      targetPath: ""
    });

    const uploads = [];
    const collectUploads = async (directoryEntry, remoteDirectory) => {
      const children = await readDirectoryEntries(directoryEntry);
      for (const child of children) {
        if (child.isDirectory) {
          const childDirectory =
            normalizeRemotePath(`${remoteDirectory}/${child.name}`);
          await post("sftp.operation", {
            operation: "mkdir-p",
            index: sftpProfile.index,
            path: childDirectory,
            targetPath: ""
          });
          await collectUploads(child, childDirectory);
        } else if (child.isFile) {
          uploads.push({
            file: await fileFromEntry(child),
            remoteDirectory
          });
        }
      }
    };
    await collectUploads(entry, remoteRoot);
    if (!uploads.length) {
      sftpTransferStatusText.textContent = `文件夹已创建：${remoteName}`;
      refreshRemoteAfterTransfers = true;
      flushTransferRefresh();
      return;
    }
    for (let index = 0; index < uploads.length; index += 1) {
      const upload = uploads[index];
      await startDroppedFileStream(
        upload.file, upload.remoteDirectory, conflict, true,
        { index: index + 1, total: uploads.length });
    }
  };

  // Asks the user once per conflicting file name, upfront, so a later file's
  // conflict dialog does not wait for earlier files to finish uploading.
  // Returns the chosen policy ("overwrite"/"rename"), null to skip the file,
  // or an empty string when the name does not conflict at all.
  const resolveDroppedFileConflict = async name => {
    const existing = [...sftpEntriesByPath.values()].find(item =>
      item.name.localeCompare(name, undefined, { sensitivity: "accent" }) === 0);
    if (!existing) return "";
    const conflict = await resolveTransferConflict(
      name, "当前远程目录中", "",
      formatConflictDetails(null, existing));
    if (!conflict) {
      sftpTransferStatusText.textContent = `已跳过上传：${name}`;
      return null;
    }
    return conflict;
  };

  // Puts every dropped file into the queue before any upload starts, so the
  // queue shows the whole batch and later files no longer wait for earlier
  // ones to finish before they appear.
  const enqueueDroppedUploads = uploads => {
    if (!uploads.length) return;
    if (uploads.length === 1) {
      const upload = uploads[0];
      startDroppedFileStream(
        upload.file, upload.remoteDirectory, upload.conflict, true, null);
      return;
    }
    for (let index = 0; index < uploads.length; index += 1) {
      const upload = uploads[index];
      transferQueue.push({
        type: "stream-upload",
        file: upload.file,
        remoteDirectory: upload.remoteDirectory,
        conflict: upload.conflict,
        batch: { index: index + 1, total: uploads.length }
      });
    }
    renderTransferPanel();
    pumpTransferQueue();
  };

  const uploadExternalDrop = async dataTransfer => {
    transferConflictPolicy = null;
    const entries = [...(dataTransfer.items || [])]
      .filter(item => item.kind === "file")
      .map(item => item.webkitGetAsEntry?.())
      .filter(Boolean);
    if (entries.length) {
      const uploads = [];
      for (const entry of entries) {
        if (entry.isDirectory) {
          await uploadDroppedDirectory(entry);
        } else if (entry.isFile) {
          const file = await fileFromEntry(entry);
          const conflict = await resolveDroppedFileConflict(file.name);
          if (conflict === null) continue;
          uploads.push({
            file, remoteDirectory: "", conflict: conflict || "overwrite"
          });
        }
      }
      enqueueDroppedUploads(uploads);
      return;
    }
    const uploads = [];
    for (const file of [...(dataTransfer.files || [])]) {
      const conflict = await resolveDroppedFileConflict(file.name);
      if (conflict === null) continue;
      uploads.push({
        file, remoteDirectory: "", conflict: conflict || "overwrite"
      });
    }
    enqueueDroppedUploads(uploads);
  };

  // Prevent Chromium/WebView2 from navigating to a dropped local file before
  // the remote directory receives the event and turns it into an upload.
  window.addEventListener("dragover", event => {
    if (hasDraggedFiles(event.dataTransfer))
      event.preventDefault();
  }, true);
  window.addEventListener("drop", event => {
    if (hasDraggedFiles(event.dataTransfer))
      event.preventDefault();
  }, true);

  let sftpExternalDragDepth = 0;
  sftpTableWrap.addEventListener("dragenter", event => {
    if (!hasDraggedFiles(event.dataTransfer)
        && !hasDraggedType(event.dataTransfer, localDragType)) return;
    event.preventDefault();
    sftpExternalDragDepth += 1;
    sftpTableWrap.classList.add("drag-over");
  });
  sftpTableWrap.addEventListener("dragover", event => {
    if (!hasDraggedFiles(event.dataTransfer)
        && !hasDraggedType(event.dataTransfer, localDragType)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = "copy";
  });
  sftpTableWrap.addEventListener("dragleave", event => {
    if (!hasDraggedFiles(event.dataTransfer)
        && !hasDraggedType(event.dataTransfer, localDragType)) return;
    sftpExternalDragDepth = Math.max(0, sftpExternalDragDepth - 1);
    if (!sftpExternalDragDepth)
      sftpTableWrap.classList.remove("drag-over");
  });
  sftpTableWrap.addEventListener("drop", async event => {
    const localPaths = readDraggedPaths(event.dataTransfer, localDragType);
    if (!localPaths.length && !hasDraggedFiles(event.dataTransfer)) return;
    event.preventDefault();
    sftpExternalDragDepth = 0;
    sftpTableWrap.classList.remove("drag-over");
    if (!sftpProfile) {
      reportError("请先连接 SSH 服务器");
      return;
    }
    if (localPaths.length) {
      localPaths.map(path => localEntriesByPath.get(path)).filter(Boolean)
        .forEach(entry => startSftpUpload(
          entry.path, false, true, "", entry.directory));
      return;
    }
    try {
      await uploadExternalDrop(event.dataTransfer);
    } catch (error) {
      sftpTransferStatusText.textContent = `拖放上传失败：${error.message}`;
      reportError(error);
    }
  });

  let localDragDepth = 0;
  localTableWrap.addEventListener("dragenter", event => {
    if (!hasDraggedType(event.dataTransfer, remoteDragType)) return;
    event.preventDefault();
    localDragDepth += 1;
    localTableWrap.classList.add("drag-over");
  });
  localTableWrap.addEventListener("dragover", event => {
    if (!hasDraggedType(event.dataTransfer, remoteDragType)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = "copy";
  });
  localTableWrap.addEventListener("dragleave", event => {
    if (!hasDraggedType(event.dataTransfer, remoteDragType)) return;
    localDragDepth = Math.max(0, localDragDepth - 1);
    if (!localDragDepth)
      localTableWrap.classList.remove("drag-over");
  });
  localTableWrap.addEventListener("drop", event => {
    const remotePaths = readDraggedPaths(event.dataTransfer, remoteDragType);
    if (!remotePaths.length) return;
    event.preventDefault();
    localDragDepth = 0;
    localTableWrap.classList.remove("drag-over");
    remotePaths.map(path => sftpEntriesByPath.get(path)).filter(Boolean)
      .forEach(entry => startSftpDownload(
        entry, true,
        entry.directory ? localPath : localJoinPath(localPath, entry.name)));
  });

  const loadSftpDirectory = (requestedPath, recordHistory = true) => {
    if (!sftpProfile || !activeSftpSessionId) {
      sftpStatusText.textContent = "请从左侧 SSH 服务器卡片打开 SFTP";
      return;
    }
    const sessionId = activeSftpSessionId;
    const path = requestedPath ? normalizeRemotePath(requestedPath) : "";
    const generation = ++sftpGeneration;
    sftpPathInput.value = path || "/";
    sftpFiles.replaceChildren();
    sftpEmpty.hidden = false;
    sftpEmpty.textContent = `正在读取 ${path || "/"}…`;
    sftpStatusText.textContent = `正在连接 ${sftpProfile.name || sftpProfile.address}…`;
    post("sftp.list", { index: sftpProfile.index, path }).then(result => {
      if (generation !== sftpGeneration ||
          sessionId !== activeSftpSessionId || !sftpProfile ||
          result.index !== sftpProfile.index) return;
      sftpPath = result.path;
      sftpPathsByProfile.set(result.index, sftpPath);
      sftpPathInput.value = sftpPath;
      if (recordHistory) {
        sftpHistory = sftpHistory.slice(0, sftpHistoryIndex + 1);
        if (sftpHistory.at(-1) !== sftpPath)
          sftpHistory.push(sftpPath);
        sftpHistoryIndex = sftpHistory.length - 1;
      }
      updateSftpHistoryButtons();
      const entries = result.entries || [];
      sftpDirectoryEntries = entries;
      const state = sftpStatesBySession.get(sessionId);
      if (state) {
        state.path = sftpPath;
        state.history = [...sftpHistory];
        state.historyIndex = sftpHistoryIndex;
        state.entries = entries;
        state.loaded = true;
      }
      renderSftpEntries(entries);
      renderSftpFavorites();
      sftpStatusText.textContent =
        `${sftpProfile.name || sftpProfile.address} · ${result.entries?.length || 0} 个项目 · ${sftpPath}`;
    }).catch(error => {
      if (generation !== sftpGeneration ||
          sessionId !== activeSftpSessionId) return;
      sftpFiles.replaceChildren();
      sftpEmpty.hidden = false;
      sftpEmpty.textContent = `读取失败：${error.message}`;
      sftpStatusText.textContent = error.message;
      reportError(error);
    });
  };

  const clearSftpView = () => {
    ++sftpGeneration;
    activeSftpSessionId = null;
    sftpProfile = null;
    sftpPath = "/";
    sftpHistory = [];
    sftpHistoryIndex = -1;
    sftpSelectedEntry = null;
    sftpDirectoryEntries = [];
    sftpEntriesByPath = new Map();
    sftpSelectedPaths.clear();
    sftpPathInput.value = "/";
    sftpFiles.replaceChildren();
    sftpEmpty.hidden = false;
    sftpEmpty.textContent = "连接 SSH 后自动打开 SFTP";
    sftpStatusText.textContent = "尚未连接 SSH 服务器";
    updateSftpHistoryButtons();
    updateSftpSelection();
    updateSidebarPanelHeader();
  };

  const closeSftpForSession = (sessionId, returnToConnections = true) => {
    sftpStatesBySession.delete(sessionId);
    if (activeSftpSessionId !== sessionId) return;
    clearSftpView();
    if (returnToConnections)
      activateFunctionPanel("connections");
  };

  const openSftp = (profile, sessionId) => {
    if (!sessionId) return;
    if (activeSftpSessionId === sessionId && sftpProfile?.index === profile.index)
      return;
    ++sftpGeneration;
    activeSftpSessionId = sessionId;
    sftpProfile = profile;
    updateSidebarPanelHeader();
    let state = sftpStatesBySession.get(sessionId);
    if (!state) {
      state = {
        path: sftpPathsByProfile.get(profile.index) || "",
        history: [],
        historyIndex: -1,
        entries: [],
        loaded: false
      };
      sftpStatesBySession.set(sessionId, state);
    }
    sftpPath = state.path;
    sftpHistory = [...state.history];
    sftpHistoryIndex = state.historyIndex;
    sftpPathInput.value = sftpPath;
    sftpSelectedEntry = null;
    sftpSelectedPaths.clear();
    updateSftpHistoryButtons();
    renderSftpFavorites();
    updateLocalControls();
    if (state.loaded) {
      sftpDirectoryEntries = state.entries;
      renderSftpEntries(sftpDirectoryEntries);
      sftpStatusText.textContent =
        `${profile.name || profile.address} · ${state.entries.length} 个项目 · ${sftpPath}`;
    } else {
      loadSftpDirectory(sftpPath);
    }
  };

  const refreshSplitRightIds = () => {
    const rightGroup = splitViewRight?.querySelector("#split-right-tabs");
    splitRightIds = rightGroup
      ? Array.from(rightGroup.querySelectorAll(".terminal-tab"), tab => tab.dataset.sessionId)
        .filter(sessionId => sessions.has(sessionId))
      : [];
    if (splitRightActive >= splitRightIds.length)
      splitRightActive = splitRightIds.length - 1;
  };

  const focusSession = sessionId => {
    const target = sessions.get(sessionId);
    if (!target) return;
    focusedSessionId = sessionId;
    sessions.forEach(session =>
      session.tab.classList.toggle("focused", session.sessionId === sessionId));
    if (target.state === "connected" && target.connectionType === "ssh") {
      const profile = profilesByIndex.get(target.profileIndex);
      if (profile) openSftp(profile, target.sessionId);
    }
    // RDP is a desktop session rather than a remote file browser. Returning
    // to it must restore the connection list card instead of leaving the
    // sidebar on the previously focused SSH SFTP panel.
    if (target.connectionType === "rdp")
      activateFunctionPanel("connections");
    else if (rdpFullscreen)
      requestRdpFullscreen(false).catch(reportError);
    if (splitMode) {
      renderSplit();
    } else {
      sessions.forEach(session =>
        session.panel.classList.toggle("focused", session.sessionId === sessionId));
    }
    renderServerMetrics();
  };

  const refitSplitViews = () => {
    activeSession?.scheduleRefit?.();
    const rightId = splitRightIds[splitRightActive];
    sessions.get(rightId)?.scheduleRefit?.();
  };

  const applySplitRatio = ratio => {
    const parsed = Number(ratio);
    splitRatio = Math.max(.18, Math.min(.82,
      Number.isFinite(parsed) ? parsed : .5));
    terminalPanels.style.setProperty("--split-primary-size",
      `${Math.round(splitRatio * 10000) / 100}%`);
    refitSplitViews();
  };

  const createSplitDivider = mode => {
    const divider = document.createElement("div");
    divider.className = `split-divider ${mode}`;
    divider.setAttribute("role", "separator");
    divider.setAttribute("aria-orientation", mode === "vertical" ? "vertical" : "horizontal");
    divider.title = mode === "vertical" ? "拖动调整终端宽度" : "拖动调整终端高度";
    divider.addEventListener("pointerdown", event => {
      if (event.button !== 0) return;
      event.preventDefault();
      divider.classList.add("resizing");
      divider.setPointerCapture(event.pointerId);
      const move = moveEvent => {
        const bounds = terminalPanels.getBoundingClientRect();
        const total = mode === "vertical" ? bounds.width : bounds.height;
        if (total <= 0) return;
        const offset = mode === "vertical"
          ? moveEvent.clientX - bounds.left : moveEvent.clientY - bounds.top;
        applySplitRatio(offset / total);
      };
      const finish = finishEvent => {
        if (divider.hasPointerCapture(finishEvent.pointerId))
          divider.releasePointerCapture(finishEvent.pointerId);
        divider.classList.remove("resizing");
        divider.removeEventListener("pointermove", move);
        divider.removeEventListener("pointerup", finish);
        divider.removeEventListener("pointercancel", finish);
        refitSplitViews();
      };
      divider.addEventListener("pointermove", move);
      divider.addEventListener("pointerup", finish);
      divider.addEventListener("pointercancel", finish);
    });
    return divider;
  };

  const activateSplitRightSession = sessionId => {
    const index = splitRightIds.indexOf(sessionId);
    if (index < 0 || !sessions.has(sessionId)) return;
    splitRightActive = index;
    renderSplit();
    const target = sessions.get(sessionId);
    focusSession(sessionId);
    target.scheduleRefit();
    setTimeout(() => target.terminal.focus(), 0);
  };

  const getAllOpenTabs = () => {
    if (!splitMode) {
      return Array.from(terminalTabs.querySelectorAll(".terminal-tab:not(.split-nav-tab)"));
    }
    const rdpTabs = Array.from(terminalTabs.querySelectorAll(".terminal-tab:not(.split-nav-tab)"));
    const leftTabs = Array.from(splitViewLeft?.querySelectorAll(".terminal-tab") || []);
    const rightTabs = Array.from(splitViewRight?.querySelectorAll(".terminal-tab") || []);
    return [...rdpTabs, ...leftTabs, ...rightTabs];
  };

  const switchToTabByIndex = (number) => {
    const tabs = getAllOpenTabs();
    if (!tabs.length) return;
    let targetTab = null;
    if (number === 9 && tabs.length >= 9) {
      targetTab = tabs[8];
    } else if (number === 9) {
      targetTab = tabs[tabs.length - 1];
    } else {
      const idx = number - 1;
      if (idx >= 0 && idx < tabs.length) {
        targetTab = tabs[idx];
      }
    }
    const targetSessionId = targetTab?.dataset?.sessionId;
    if (targetSessionId && sessions.has(targetSessionId)) {
      activateSession(targetSessionId);
    }
  };

  const cycleTab = (forward = true) => {
    const tabs = getAllOpenTabs();
    if (tabs.length <= 1) return;
    const currentId = activeSession?.sessionId || focusedSessionId;
    const currentIdx = tabs.findIndex(tab => tab.dataset?.sessionId === currentId);
    let nextIdx = 0;
    if (currentIdx >= 0) {
      nextIdx = forward
        ? (currentIdx + 1) % tabs.length
        : (currentIdx - 1 + tabs.length) % tabs.length;
    }
    const targetSessionId = tabs[nextIdx]?.dataset?.sessionId;
    if (targetSessionId && sessions.has(targetSessionId)) {
      activateSession(targetSessionId);
    }
  };

  const activateSession = sessionId => {
    const activationGeneration = ++sessionActivationGeneration;
    if (splitMode && splitRightIds.includes(sessionId)) {
      activateSplitRightSession(sessionId);
      return;
    }
    const target = sessions.get(sessionId);
    if (!target) return;
    const commitActivation = () => {
      if (activationGeneration !== sessionActivationGeneration
          || sessions.get(sessionId) !== target) return;
      sessions.forEach(session => {
        const active = session === target;
        session.tab.classList.toggle("active", active);
        if (!splitMode)
          session.panel.classList.toggle("active", active);
      });
      activeSession = target;
      if (rdpKeyToolboxBtn) {
        rdpKeyToolboxBtn.hidden = target.connectionType !== "rdp";
        if (rdpKeyToolboxBtn.hidden) closeRdpKeyMenu();
      }
      if (rdpConnectedStatusSessionId
          && rdpConnectedStatusSessionId !== target.sessionId)
        clearRdpConnectedStatus(rdpConnectedStatusSessionId);
      if (target.connectionType === "rdp" && target.state === "connecting")
        syncRdpStatus(target);
      else if (target.connectionType !== "rdp"
          && rdpConnectingStatusSessionId) {
        // A connecting RDP tab may be left in the background. Its transient
        // header message must not follow the user onto an SSH/local tab.
        clearRdpConnectingStatus(rdpConnectingStatusSessionId);
      }
      if (splitMode) renderSplit();
      target.scheduleRefit();
      focusSession(target.sessionId);
      // The native RDP control is a child HWND above WebView2. Switching tabs
      // only needs a native surface handoff; refreshing the remote display
      // here makes mstscax renegotiate the desktop and looks like a reconnect.
      notifyRdpLayout(false);
      setTimeout(() => {
        if (activeSession !== target) return;
        target.scheduleRefit();
        target.terminal.focus();
      }, 0);
    };

    const preactivateNativeRdp = target.connectionType === "rdp"
      && target.state === "connected"
      && activeSession?.connectionType !== "rdp";
    if (!preactivateNativeRdp) {
      commitActivation();
      return;
    }

    // Keep the current WebView terminal visible until the native RDP child is
    // already above it. The host acknowledges rdpLayout after changing the
    // HWND z-order, so committing the DOM tab afterwards leaves no black frame
    // between an SSH/local/serial panel and a connected RDP desktop.
    post("session.rdpLayout", {
      sessionId: target.sessionId,
      visible: true,
      refresh: false,
      clearOcclusion: true
    }).catch(reportError).then(commitActivation);
  };

  let isSplitZoomed = false;
  let splitZoomedPane = null;
  const splitZoomBanner = document.querySelector("#split-zoom-banner");
  const splitZoomRestoreBtn = document.querySelector("#split-zoom-restore-btn");

  const toggleSplitZoom = () => {
    if (!splitMode || !splitViewLeft || !splitViewRight) return;
    if (!isSplitZoomed) {
      const zoomRight = focusedSessionId && splitRightIds.includes(focusedSessionId);
      splitZoomedPane = zoomRight ? "right" : "left";
      if (splitZoomedPane === "left") {
        splitViewRight.style.display = "none";
        if (splitDivider) splitDivider.style.display = "none";
        splitViewLeft.style.flex = "1 1 100%";
        splitViewLeft.style.maxWidth = "none";
      } else {
        splitViewLeft.style.display = "none";
        if (splitDivider) splitDivider.style.display = "none";
        splitViewRight.style.flex = "1 1 100%";
        splitViewRight.style.maxWidth = "none";
      }
      isSplitZoomed = true;
      if (splitZoomBanner) splitZoomBanner.hidden = false;
      refitSplitViews();
      showTransientStatus("已放大当前子窗口 (按 Alt+Z 还原)。");
    } else {
      splitViewLeft.style.display = "";
      splitViewRight.style.display = "";
      if (splitDivider) splitDivider.style.display = "";
      splitViewLeft.style.flex = "";
      splitViewLeft.style.maxWidth = "";
      splitViewRight.style.flex = "";
      splitViewRight.style.maxWidth = "";
      isSplitZoomed = false;
      splitZoomedPane = null;
      if (splitZoomBanner) splitZoomBanner.hidden = true;
      applySplitRatio(.5);
      refitSplitViews();
      showTransientStatus("已还原双窗分屏。");
    }
  };
  splitZoomRestoreBtn?.addEventListener("click", toggleSplitZoom);

  const switchToSplitView = () => {
    Array.from(sessions.values()).filter(s => s.connectionType === "rdp").forEach(s => {
      post("session.rdpLayout", { sessionId: s.sessionId, visible: false }).catch(reportError);
    });
    const leftGroup = splitViewLeft?.querySelector(".tab-group.left");
    const rightGroup = splitViewRight?.querySelector(".tab-group.right") || document.querySelector("#split-right-tabs");
    let targetId = null;
    if (focusedSessionId) {
      const s = sessions.get(focusedSessionId);
      if (s?.tab?.parentElement === leftGroup || s?.tab?.parentElement === rightGroup) {
        targetId = focusedSessionId;
      }
    }
    if (!targetId) {
      targetId = leftGroup?.querySelector(".terminal-tab.active")?.dataset.sessionId
        || leftGroup?.querySelector(".terminal-tab")?.dataset.sessionId
        || rightGroup?.querySelector(".terminal-tab.active")?.dataset.sessionId
        || rightGroup?.querySelector(".terminal-tab")?.dataset.sessionId;
    }
    if (targetId) {
      activateSession(targetId);
    }
    renderSplit();
  };

  const moveTabToTopLevel = (sessionId, targetBefore = null) => {
    const session = sessions.get(sessionId);
    if (!session || session.tab.parentElement === terminalTabs || session.connectionType === "rdp") return;
    const sourceGroup = session.tab.parentElement;
    insertTerminalTab(terminalTabs, session.tab, targetBefore);
    refreshSplitRightIds();
    if (sourceGroup?.classList?.contains("tab-group")
        && !sourceGroup.querySelector(".terminal-tab")) {
      exitSplit(sessionId);
      showTransientStatus("已合并分屏。");
      return;
    }
    activateSession(sessionId);
    renderSplit();
  };

  terminalTabs.addEventListener("dragover", event => {
    if (!splitMode) return;
    if (Array.from(event.dataTransfer.types).includes("application/x-masterterm-terminal-tab")) {
      event.preventDefault();
      terminalTabs.classList.add("drag-over");
    }
  });
  terminalTabs.addEventListener("dragleave", event => {
    if (!terminalTabs.contains(event.relatedTarget)) {
      terminalTabs.classList.remove("drag-over");
    }
  });
  terminalTabs.addEventListener("drop", event => {
    if (!splitMode) return;
    terminalTabs.classList.remove("drag-over");
    const sessionId = event.dataTransfer.getData("application/x-masterterm-terminal-tab");
    const session = sessions.get(sessionId);
    if (!session || session.tab.parentElement === terminalTabs || session.connectionType === "rdp") return;
    event.preventDefault();
    const dropTab = event.target.closest(".terminal-tab");
    let insertBefore = null;
    if (dropTab && dropTab.parentElement === terminalTabs && dropTab !== session.tab) {
      const before = event.clientX < dropTab.getBoundingClientRect().left + dropTab.offsetWidth / 2;
      insertBefore = before ? dropTab : dropTab.nextElementSibling;
    }
    moveTabToTopLevel(sessionId, insertBefore);
  });

  const setSplit = (mode, secondaryId) => {
    if (!mode) {
      exitSplit();
      return;
    }
    const secondary = sessions.get(secondaryId);
    if (activeSession?.connectionType === "rdp" || secondary?.connectionType === "rdp") {
      showTransientStatus("RDP 会话为原生桌面，不支持加入分屏。");
      return;
    }
    if (splitMode)
      exitSplit();
    const primaryId = activeSession?.sessionId;
    if (!primaryId || !secondaryId || primaryId === secondaryId)
      return;
    splitMode = mode;
    terminalTabs.classList.add("split-active");
    const hasRdp = hasRdpSession();
    terminalTabs.classList.toggle("has-rdp", hasRdp);

    const leftGroup = document.createElement("div");
    leftGroup.className = "tab-group left";
    const rightGroup = document.createElement("div");
    rightGroup.className = "tab-group right";
    rightGroup.id = "split-right-tabs";
    [...terminalTabs.querySelectorAll(".terminal-tab")]
      .filter(tab => sessions.get(tab.dataset.sessionId)?.connectionType !== "rdp")
      .forEach(tab => leftGroup.appendChild(tab));

    if (hasRdp) {
      let splitNavTab = terminalTabs.querySelector(".split-nav-tab");
      if (!splitNavTab) {
        splitNavTab = document.createElement("button");
        splitNavTab.type = "button";
        splitNavTab.className = "terminal-tab split-nav-tab active";
        splitNavTab.textContent = "🔲 终端分屏";
        splitNavTab.title = "点击切回活动终端分屏视图";
        splitNavTab.addEventListener("click", () => {
          switchToSplitView();
        });
        splitNavTab.addEventListener("contextmenu", event => {
          event.preventDefault();
          event.stopPropagation();
          closeTerminalContextMenu();
          closeTerminalTabsContextMenu();
          showSplitNavContextMenu(event);
        });
        terminalTabs.insertBefore(splitNavTab, terminalTabs.firstChild);
      }
    }

    const acceptMovedTab = (group, side) => {
      group.addEventListener("dragover", event => {
        if (Array.from(event.dataTransfer.types)
          .includes("application/x-masterterm-terminal-tab")) {
          event.preventDefault();
          group.classList.add("drag-over");
        }
      });
      group.addEventListener("dragleave", () =>
        group.classList.remove("drag-over"));
      group.addEventListener("drop", event => {
        event.preventDefault();
        group.classList.remove("drag-over");
        const sessionId = event.dataTransfer.getData(
          "application/x-masterterm-terminal-tab");
        const session = sessions.get(sessionId);
        if (!session || session.tab.parentElement === group || session.connectionType === "rdp") return;
        const sourceGroup = session.tab.parentElement;
        insertTerminalTab(group, session.tab);
        refreshSplitRightIds();
        if (sourceGroup?.classList?.contains("tab-group")
            && !sourceGroup.querySelector(".terminal-tab")) {
          exitSplit(sessionId);
          showTransientStatus("已合并分屏。");
          return;
        }
        if (side === "right") {
          splitRightActive = splitRightIds.indexOf(sessionId);
          if (activeSession === session || activeSession?.tab.parentElement !== leftGroup) {
            const nextLeftId = leftGroup.querySelector(".terminal-tab")?.dataset.sessionId;
            activeSession = nextLeftId ? sessions.get(nextLeftId) || null : null;
          }
          activateSplitRightSession(sessionId);
        } else {
          activeSession = session;
          activateSession(sessionId);
        }
        renderSplit();
      });
    };
    acceptMovedTab(leftGroup, "left");
    acceptMovedTab(rightGroup, "right");
    leftGroup.appendChild(cloneTerminalCreateGroup());
    if (!secondary) {
      exitSplit();
      return;
    }
    rightGroup.appendChild(secondary.tab);
    rightGroup.appendChild(cloneTerminalCreateGroup());
    splitRightIds = [secondaryId];
    splitRightActive = 0;
    terminalPanels.classList.add(
      mode === "vertical" ? "split-vertical" : "split-horizontal");
    splitViewLeft = document.createElement("div");
    splitViewLeft.className = "split-view left";
    splitViewRight = document.createElement("div");
    splitViewRight.className = "split-view right";
    splitViewLeft.appendChild(leftGroup);
    splitViewRight.appendChild(rightGroup);
    splitDivider = createSplitDivider(mode);
    terminalPanels.append(splitViewLeft, splitDivider, splitViewRight);
    applySplitRatio(.5);
    renderSplit();
  };

  const renderSplit = () => {
    if (!splitMode) return;
    refreshSplitRightIds();
    const hasTopLevelTabs = Array.from(sessions.values()).some(s => s.tab?.parentElement === terminalTabs);
    const hasRdp = hasRdpSession() || hasTopLevelTabs;
    terminalTabs.classList.toggle("has-rdp", hasRdp);

    const leftGroup = splitViewLeft?.querySelector(".tab-group.left");
    const rightGroup = splitViewRight?.querySelector(".tab-group.right");
    ensureTerminalQuickOpenLast(leftGroup);
    ensureTerminalQuickOpenLast(rightGroup);

    let splitNavTab = terminalTabs.querySelector(".split-nav-tab");
    if (hasRdp && !splitNavTab) {
      splitNavTab = document.createElement("button");
      splitNavTab.type = "button";
      splitNavTab.className = "terminal-tab split-nav-tab";
      splitNavTab.textContent = "🔲 终端分屏";
      splitNavTab.title = "点击切回活动终端分屏视图";
      splitNavTab.addEventListener("click", () => {
        switchToSplitView();
      });
      splitNavTab.addEventListener("contextmenu", event => {
        event.preventDefault();
        event.stopPropagation();
        closeTerminalContextMenu();
        closeTerminalTabsContextMenu();
        showSplitNavContextMenu(event);
      });
      terminalTabs.insertBefore(splitNavTab, terminalTabs.firstChild);
    } else if (!hasRdp && splitNavTab) {
      splitNavTab.remove();
      splitNavTab = null;
    }

    const isRdpActive = activeSession?.connectionType === "rdp";
    const isTopLevelActive = activeSession?.tab?.parentElement === terminalTabs || isRdpActive;
    if (splitNavTab) {
      splitNavTab.classList.toggle("active", !isTopLevelActive);
    }

    if (!isTopLevelActive) {
      if (!activeSession || (activeSession.tab.parentElement !== leftGroup && activeSession.tab.parentElement !== rightGroup)) {
        const leftId = leftGroup?.querySelector(".terminal-tab")?.dataset.sessionId;
        activeSession = leftId ? sessions.get(leftId) || null : null;
      }
    }

    if (splitViewLeft) splitViewLeft.hidden = isTopLevelActive;
    if (splitViewRight) splitViewRight.hidden = isTopLevelActive;
    if (splitDivider) splitDivider.hidden = isTopLevelActive;

    const rightId = splitRightIds[splitRightActive];
    sessions.forEach(session => {
      if (isTopLevelActive) {
        const active = session === activeSession;
        session.tab.classList.toggle("active", active);
        session.tab.classList.toggle("focused", active);
        session.panel.classList.toggle("active", active);
        if (active && session.panel.parentElement !== terminalPanels) {
          terminalPanels.appendChild(session.panel);
        }
      } else {
        const inLeft = splitMode && session === activeSession;
        const inRight = splitMode && session.sessionId === rightId;
        session.tab.classList.toggle("active", inLeft || inRight);
        session.tab.classList.toggle("focused", session.sessionId === focusedSessionId);
        session.panel.classList.toggle("active", inLeft || inRight);
        if (inLeft && session.panel.parentElement !== splitViewLeft)
          splitViewLeft.appendChild(session.panel);
        else if (inRight && session.panel.parentElement !== splitViewRight)
          splitViewRight.appendChild(session.panel);
        else if (!inLeft && !inRight
                 && session.panel.parentElement?.classList?.contains("split-view"))
          terminalPanels.appendChild(session.panel);
      }
    });

    if (isTopLevelActive) {
      if (activeSession?.connectionType === "rdp") {
        notifyRdpLayout(false);
      } else {
        Array.from(sessions.values()).filter(s => s.connectionType === "rdp").forEach(s => {
          post("session.rdpLayout", { sessionId: s.sessionId, visible: false }).catch(reportError);
        });
        activeSession?.scheduleRefit?.();
      }
    } else {
      Array.from(sessions.values()).filter(s => s.connectionType === "rdp").forEach(s => {
        post("session.rdpLayout", { sessionId: s.sessionId, visible: false }).catch(reportError);
      });
      splitViewLeft?.classList.toggle("focused", activeSession?.sessionId === focusedSessionId);
      splitViewRight?.classList.toggle("focused", rightId === focusedSessionId);
      refitSplitViews();
    }
  };

  const exitSplit = (preferredSessionId = activeSession?.sessionId) => {
    splitMode = null;
    splitRightIds = [];
    splitRightActive = -1;
    isSplitZoomed = false;
    splitZoomedPane = null;
    if (splitZoomBanner) splitZoomBanner.hidden = true;
    terminalTabs.querySelector(".split-nav-tab")?.remove();
    terminalTabs.classList.remove("split-active", "has-rdp");
    [splitViewLeft, splitViewRight].filter(Boolean)
      .forEach(view => appendTerminalTabs(
        terminalTabs, [...view.querySelectorAll(".tab-group .terminal-tab")]));
    terminalTabs.querySelectorAll(".tab-group").forEach(group => group.remove());
    terminalPanels.classList.remove("split-vertical", "split-horizontal");
    terminalPanels.style.removeProperty("--split-primary-size");
    splitDivider?.remove();
    splitDivider = null;
    splitViewLeft?.remove();
    splitViewRight?.remove();
    splitViewLeft = null;
    splitViewRight = null;
    sessions.forEach(session => {
      if (session.panel.parentElement !== terminalPanels)
        terminalPanels.appendChild(session.panel);
    });
    const nextId = sessions.has(preferredSessionId) ? preferredSessionId
      : Array.from(sessions.keys()).at(-1);
    if (nextId) activateSession(nextId);
    else {
      focusedSessionId = "";
      renderServerMetrics();
    }
  };

  const chooseHealthySession = sessionIds => {
    const candidates = sessionIds
      .map(sessionId => sessions.get(sessionId))
      .filter(session => session && !session.closing);
    return candidates.findLast(session => session.state === "connected")?.sessionId
      || candidates.findLast(session => session.state === "connecting")?.sessionId
      || candidates.at(-1)?.sessionId || "";
  };

  const disposeSession = sessionId => {
    hideRdpTabHover();
    const session = sessions.get(sessionId);
    if (!session) return;
    clearRdpConnectingStatus(sessionId);
    clearRdpConnectedStatus(sessionId);
    closeSftpForSession(sessionId, false);
    const wasActive = activeSession === session;
    const wasFocused = focusedSessionId === sessionId;
    if (session.leaveTimer) clearTimeout(session.leaveTimer);
    if (session.resizeTimer) clearTimeout(session.resizeTimer);
    session.terminal.dispose();
    session.tab.remove();
    session.panel.remove();
    sessions.delete(sessionId);
    if (splitMode) {
      const index = splitRightIds.indexOf(sessionId);
      if (index >= 0) {
        splitRightIds.splice(index, 1);
        if (splitRightActive > index)
          splitRightActive--;
        else if (splitRightActive >= splitRightIds.length)
          splitRightActive = Math.max(0, splitRightIds.length - 1);
      }
      if (!splitRightIds.length)
        setSplit(null, null);
      else
        renderSplit();
    }
    saveSessionRestore();
    if (wasActive) {
      activeSession = null;
      const remaining = Array.from(sessions.keys());
      const leftRemaining = splitMode
        ? remaining.filter(id => !splitRightIds.includes(id)) : remaining;
      // Closing the last left-view tab leaves no primary pane.  Return to the
      // normal tab strip first, then select a remaining session.
      if (splitMode && !leftRemaining.length)
        exitSplit();
      const next = chooseHealthySession(splitMode ? leftRemaining : remaining);
      if (next) {
        if (splitMode && !wasFocused) {
          activeSession = sessions.get(next) || null;
          renderSplit();
        } else {
          activateSession(next);
        }
      }
      activateFunctionPanel(activeSftpSessionId ? "sftp" : "connections");
    }
    if (wasFocused && !wasActive) {
      const next = splitMode
        ? splitRightIds[splitRightActive] || activeSession?.sessionId
        : chooseHealthySession(Array.from(sessions.keys()));
      if (next) activateSession(next);
      else focusedSessionId = "";
    }
    if (!sessions.size) focusedSessionId = "";
    renderServerMetrics();
    const empty = sessions.size === 0;
    terminalTabs.hidden = empty;
    terminalEmpty.hidden = !empty;
    if (empty && rdpKeyToolboxBtn) {
      rdpKeyToolboxBtn.hidden = true;
      closeRdpKeyMenu();
    }
  };

  const closeSession = sessionId => {
    const session = sessions.get(sessionId);
    if (!session) return Promise.resolve();
    clearRdpConnectingStatus(sessionId);
    clearRdpConnectedStatus(sessionId);
    session.closing = true;
    const closeRequest = session.connectionType === "rdp"
      ? post("session.rdpClose", { sessionId })
      : post("session.close", { sessionId });
    return closeRequest
      .then(() => disposeSession(sessionId))
      .catch(error => {
        if (session) session.closing = false;
        reportError(error);
      });
  };

  const closeTerminalContextMenu = () => {
    const wasVisible = hideVisibleElement(terminalContextMenu);
    contextMenuSessionId = null;
    if (wasVisible) clearRdpOverlay();
  };
  const closeTerminalOutputContextMenu = () => {
    const wasVisible = hideVisibleElement(terminalOutputContextMenu);
    terminalOutputContextSessionId = null;
    if (wasVisible) clearRdpOverlay();
  };
  const closeThemeMenu = () => {
    const wasVisible = hideVisibleElement(themeMenu);
    if (themeMenu) {
      themeMenu.querySelectorAll(".theme-menu-item-wrap").forEach(w => w.classList.remove("open"));
    }
    if (wasVisible) clearRdpOverlay();
  };
  const contextMenuFits = (left, top, width, height, avoidRect = null) => {
    if (left < 8 || top < 8
        || left + width > window.innerWidth - 8
        || top + height > window.innerHeight - 8)
      return false;
    if (!avoidRect) return true;
    return left + width <= avoidRect.left
      || left >= avoidRect.right
      || top + height <= avoidRect.top
      || top >= avoidRect.bottom;
  };
  const placeContextMenu = (menu, event, avoidRect = null) => {
    menu.style.left = "8px";
    menu.style.top = "8px";
    menu.hidden = false;
    const bounds = menu.getBoundingClientRect();
    const clampLeft = value => Math.max(8, Math.min(
      value, window.innerWidth - bounds.width - 8));
    const clampTop = value => Math.max(8, Math.min(
      value, window.innerHeight - bounds.height - 8));
    const candidates = [
      [clampLeft(event.clientX), clampTop(event.clientY)]
    ];
    if (avoidRect) {
      const top = clampTop(event.clientY);
      candidates.push(
        [clampLeft(avoidRect.left - bounds.width - 8), top],
        [clampLeft(avoidRect.right + 8), top],
        [clampLeft(avoidRect.left), clampTop(avoidRect.top - bounds.height - 8)],
        [clampLeft(avoidRect.left), clampTop(avoidRect.bottom + 8)]
      );
    }
    const selected = candidates.find(([left, top]) =>
      contextMenuFits(left, top, bounds.width, bounds.height, avoidRect))
      || candidates[0];
    menu.style.left = `${selected[0]}px`;
    menu.style.top = `${selected[1]}px`;
  };
  const showTerminalContextMenu = (event, sessionId) => {
    event.preventDefault();
    closeTerminalContextMenu();
    contextMenuSessionId = sessionId;
    const session = sessions.get(sessionId);
    const isRdp = session?.connectionType === "rdp";
    const hasRdp = hasRdpSession();
    ["diagnostics", "split-vertical", "split-horizontal", "batch-command"]
      .forEach(action => {
        const button = terminalContextMenu.querySelector(`[data-action="${action}"]`);
        if (button) button.hidden = isRdp;
      });
    ["split-vertical", "split-horizontal"].forEach(action => {
      const button = terminalContextMenu.querySelector(`[data-action="${action}"]`);
      if (button) {
        button.disabled = false;
        button.title = "";
      }
    });
    const reconnectButton = terminalContextMenu.querySelector('[data-action="reconnect"]');
    if (reconnectButton)
      reconnectButton.disabled = !session || (!isRdp
        && session.state !== "closed" && session.state !== "error");
    const fullscreenButton = terminalContextMenu.querySelector('[data-action="fullscreen"]');
    if (fullscreenButton) {
      fullscreenButton.hidden = !isRdp;
      fullscreenButton.textContent = rdpFullscreen ? "退出全屏" : "全屏显示";
    }
    const parentHost = session?.tab?.parentElement || terminalTabs;
    const tabs = Array.from(parentHost.querySelectorAll(".terminal-tab"));
    const position = tabs.findIndex(tab => tab.dataset.sessionId === sessionId);
    terminalContextMenu.querySelector('[data-action="move-left"]').disabled = position <= 0;
    terminalContextMenu.querySelector('[data-action="move-right"]').disabled =
      position < 0 || position >= tabs.length - 1;
    if (isRdp) {
      // A WebView2 child cannot out-rank the embedded RDP ActiveX child with
      // CSS z-index. Use the native top-level popup so the menu is always
      // above the RDP surface, including after mstscax raises its child.
      terminalContextMenu.hidden = true;
      post("session.rdpContextMenu", {
        sessionId,
        x: Math.round(event.clientX),
        y: Math.round(event.clientY)
      }).catch(reportError);
      return;
    }
    placeContextMenu(terminalContextMenu, event);
  };
  // ---- 终端搜索 ----
  const terminalSearchBar = document.querySelector("#terminal-search");
  const terminalSearchInput = document.querySelector("#terminal-search-input");
  const terminalSearchCount = document.querySelector("#terminal-search-count");
  const terminalSearchPrev = document.querySelector("#terminal-search-prev");
  const terminalSearchNext = document.querySelector("#terminal-search-next");
  const terminalSearchCase = document.querySelector("#terminal-search-case");
  const terminalSearchRegex = document.querySelector("#terminal-search-regex");
  const terminalSearchClose = document.querySelector("#terminal-search-close");
  const terminalSearchState = {
    session: null, query: "", matches: [], index: -1, decorations: [],
    caseSensitive: false, isRegex: false
  };

  const SearchMatchBackground = "#3b82f6";
  const SearchMatchForeground = "#f8fafc";
  const SearchCurrentBackground = "#f59e0b";
  const SearchCurrentForeground = "#0f172a";

  const clearTerminalSearchDecorations = () => {
    for (const decoration of terminalSearchState.decorations) {
      try {
        decoration.dispose();
      } catch {
        // Decoration may already be disposed with its terminal.
      }
    }
    terminalSearchState.decorations = [];
  };

  const createTerminalSearchDecoration = (terminal, match, background, foreground) => {
    if (!terminal.registerMarker || !terminal.registerDecoration)
      return false;
    const buffer = terminal.buffer.active;
    const cursorLine = (buffer.baseY ?? buffer.ybase ?? 0)
      + (buffer.cursorY ?? buffer.y ?? 0);
    let marker = null;
    try {
      marker = terminal.registerMarker(match.y - cursorLine);
    } catch {
      marker = null;
    }
    if (!marker)
      return false;
    let decoration = null;
    try {
      decoration = terminal.registerDecoration({
        marker,
        x: match.col,
        width: Math.max(1, match.length),
        height: 1,
        layer: "bottom",
        backgroundColor: background,
        foregroundColor: foreground
      });
    } catch {
      decoration = null;
    }
    if (!decoration)
      return false;
    terminalSearchState.decorations.push(decoration);
    return true;
  };

  const findTerminalMatches = (terminal, query) => {
    const matches = [];
    if (!query) return matches;
    const buffer = terminal.buffer.active;
    const { caseSensitive, isRegex } = terminalSearchState;
    let regex = null;
    if (isRegex) {
      try {
        regex = new RegExp(query, caseSensitive ? "g" : "gi");
      } catch {
        return matches;
      }
    }
    const target = caseSensitive ? query : query.toLowerCase();
    for (let y = 0; y < buffer.length; ++y) {
      const line = buffer.getLine(y);
      if (!line) continue;
      const rawText = line.translateToString(true);
      if (regex) {
        regex.lastIndex = 0;
        let match;
        while ((match = regex.exec(rawText)) !== null) {
          const matchLength = Math.max(1, match[0].length);
          matches.push({ y, col: match.index, length: matchLength });
          if (!regex.global || match.index === regex.lastIndex) break;
        }
      } else {
        const text = caseSensitive ? rawText : rawText.toLowerCase();
        let index = text.indexOf(target);
        while (index !== -1) {
          matches.push({ y, col: index, length: target.length });
          index = text.indexOf(target, index + Math.max(1, target.length));
        }
      }
    }
    return matches;
  };

  const renderTerminalSearchCount = () => {
    terminalSearchCount.textContent = terminalSearchState.matches.length
      ? `${terminalSearchState.index + 1}/${terminalSearchState.matches.length}`
      : (terminalSearchState.query ? "无匹配" : "");
  };

  const jumpToTerminalMatch = (session, index) => {
    const matches = terminalSearchState.matches;
    if (!matches.length) return;
    const normalized = ((index % matches.length) + matches.length) % matches.length;
    const match = matches[normalized];
    terminalSearchState.index = normalized;
    const terminal = session.terminal;
    terminal.scrollToLine(Math.max(0, match.y - Math.floor(terminal.rows / 2)));
    if (!terminalSearchState.decorations.length)
      terminal.select(match.col, match.y, match.length);
    renderTerminalSearchCount();
  };

  const runTerminalSearch = (session, direction = 1) => {
    const query = terminalSearchInput.value;
    terminalSearchState.query = query;
    terminalSearchState.matches = findTerminalMatches(session.terminal, query);
    clearTerminalSearchDecorations();
    if (!terminalSearchState.matches.length) {
      terminalSearchState.index = -1;
      renderTerminalSearchCount();
      return;
    }
    const start = terminalSearchState.index < 0
      ? (direction > 0 ? 0 : terminalSearchState.matches.length - 1)
      : (direction === 0 ? terminalSearchState.index : terminalSearchState.index + direction);
    const normalized = ((start % terminalSearchState.matches.length)
      + terminalSearchState.matches.length) % terminalSearchState.matches.length;
    terminalSearchState.index = normalized;
    terminalSearchState.matches.forEach((match, matchIndex) => {
      createTerminalSearchDecoration(
        session.terminal, match,
        matchIndex === terminalSearchState.index
          ? SearchCurrentBackground : SearchMatchBackground,
        matchIndex === terminalSearchState.index
          ? SearchCurrentForeground : SearchMatchForeground);
    });
    jumpToTerminalMatch(session, start);
  };

  const openTerminalSearch = session => {
    if (!session || session.connectionType === "rdp") return;
    clearTerminalSearchDecorations();
    terminalSearchState.session = session;
    terminalSearchState.matches = [];
    terminalSearchState.index = -1;
    terminalSearchBar.hidden = false;
    terminalSearchInput.value = session.searchQuery || "";
    terminalSearchCount.textContent = "";
    terminalSearchCase?.classList.toggle("active", terminalSearchState.caseSensitive);
    terminalSearchRegex?.classList.toggle("active", terminalSearchState.isRegex);
    terminalSearchInput.focus();
    terminalSearchInput.select();
    if (terminalSearchInput.value) {
      runTerminalSearch(session, 1);
    }
  };

  const closeTerminalSearch = () => {
    terminalSearchBar.hidden = true;
    const session = terminalSearchState.session;
    clearTerminalSearchDecorations();
    terminalSearchState.session = null;
    terminalSearchState.matches = [];
    terminalSearchState.index = -1;
    if (session && (session.state === "connected" || session.state === "connecting"))
      session.terminal.focus();
  };

  terminalSearchInput.addEventListener("keydown", event => {
    if (event.key === "Enter" || event.key === "F3") {
      if (terminalSearchState.session)
        runTerminalSearch(terminalSearchState.session, event.shiftKey ? -1 : 1);
      event.preventDefault();
    } else if (event.key === "Escape") {
      closeTerminalSearch();
      event.preventDefault();
    } else if (event.altKey && event.key.toLowerCase() === "c") {
      terminalSearchCase?.click();
      event.preventDefault();
    } else if (event.altKey && event.key.toLowerCase() === "r") {
      terminalSearchRegex?.click();
      event.preventDefault();
    }
  });
  terminalSearchInput.addEventListener("input", () => {
    if (terminalSearchState.session) {
      terminalSearchState.session.searchQuery = terminalSearchInput.value;
      runTerminalSearch(terminalSearchState.session, 1);
    }
  });
  terminalSearchPrev?.addEventListener("click", () => {
    if (terminalSearchState.session) runTerminalSearch(terminalSearchState.session, -1);
  });
  terminalSearchNext?.addEventListener("click", () => {
    if (terminalSearchState.session) runTerminalSearch(terminalSearchState.session, 1);
  });
  terminalSearchCase?.addEventListener("click", () => {
    terminalSearchState.caseSensitive = !terminalSearchState.caseSensitive;
    terminalSearchCase.classList.toggle("active", terminalSearchState.caseSensitive);
    if (terminalSearchState.session) runTerminalSearch(terminalSearchState.session, 0);
  });
  terminalSearchRegex?.addEventListener("click", () => {
    terminalSearchState.isRegex = !terminalSearchState.isRegex;
    terminalSearchRegex.classList.toggle("active", terminalSearchState.isRegex);
    if (terminalSearchState.session) runTerminalSearch(terminalSearchState.session, 0);
  });
  terminalSearchClose?.addEventListener("click", closeTerminalSearch);

  // ---- 命令收藏 ----
  const commandFavoritesKey = "masterterm.commandFavorites";
  let commandFavorites = [];
  let commandFavoriteEditingId = null;
  try {
    const stored = JSON.parse(localStorage.getItem(commandFavoritesKey) || "[]");
    if (Array.isArray(stored))
      commandFavorites = stored.filter(item =>
        item && typeof item.command === "string" && item.command.trim());
  } catch {
    commandFavorites = [];
  }

  const persistCommandFavorites = () => {
    try {
      localStorage.setItem(commandFavoritesKey, JSON.stringify(commandFavorites));
    } catch {
      // Storage unavailable (private mode / quota): favorites stay in-memory.
    }
  };

  // ---- 参数宏与命令片段 ----
  const commandMacroDialog = document.querySelector("#command-macro-dialog");
  const commandMacroClose = document.querySelector("#command-macro-close");
  const commandMacroCancel = document.querySelector("#command-macro-cancel");
  const commandMacroSubmit = document.querySelector("#command-macro-submit");
  const commandMacroFields = document.querySelector("#command-macro-fields");
  const commandMacroPreview = document.querySelector("#command-macro-preview");
  let activeMacroTemplate = null;

  const extractCommandMacros = command => {
    const matches = [...command.matchAll(/\{\{([^}]+)\}\}/g)];
    if (!matches.length) return [];
    const macros = [];
    const seen = new Set();
    for (const m of matches) {
      const raw = m[1].trim();
      const colon = raw.indexOf(":");
      const name = colon >= 0 ? raw.slice(0, colon).trim() : raw;
      const defaultValue = colon >= 0 ? raw.slice(colon + 1).trim() : "";
      if (!seen.has(name)) {
        seen.add(name);
        macros.push({ name, defaultValue, placeholder: m[0] });
      }
    }
    return macros;
  };

  const executeResolvedFavoriteCommand = finalCommand => {
    const session = activeSession;
    if (!session || session.state === "closed" || session.state === "error") {
      showTransientStatus("请先打开一个终端会话再发送收藏命令。");
      return false;
    }
    const normalized = finalCommand.replace(/\r?\n/g, "\r") + "\r";
    hideCompletion(session);
    post("session.input", { sessionId: session.sessionId, data: normalized })
      .catch(reportError);
    session.terminal.focus();
    showTransientStatus("已发送命令。");
    return true;
  };

  const sendFavoriteCommand = command => {
    const session = activeSession;
    if (!session || session.state === "closed" || session.state === "error") {
      showTransientStatus("请先打开一个终端会话再发送收藏命令。");
      return false;
    }
    const macros = extractCommandMacros(command);
    if (!macros.length) {
      return executeResolvedFavoriteCommand(command);
    }
    if (!commandMacroDialog) {
      return executeResolvedFavoriteCommand(command);
    }
    activeMacroTemplate = command;
    commandMacroFields.replaceChildren();
    let macroCache = {};
    try {
      macroCache = JSON.parse(localStorage.getItem("masterterm.macroCache") || "{}");
    } catch {
      macroCache = {};
    }

    const inputMap = new Map();
    const updatePreview = () => {
      let resolved = activeMacroTemplate;
      for (const macro of macros) {
        const input = inputMap.get(macro.name);
        const val = input ? (input.value || macro.defaultValue) : macro.defaultValue;
        const re = new RegExp(`\\{\\{${macro.name}(:[^}]+)?\\}\\}`, "g");
        resolved = resolved.replace(re, val);
      }
      if (commandMacroPreview) commandMacroPreview.textContent = resolved;
    };

    macros.forEach((macro, idx) => {
      const row = document.createElement("div");
      row.className = "command-macro-row";
      const label = document.createElement("label");
      label.className = "command-macro-label";
      label.textContent = macro.name;
      label.title = macro.name;
      const input = document.createElement("input");
      input.type = "text";
      input.className = "command-macro-input";
      input.dataset.macroName = macro.name;
      input.value = macroCache[macro.name] ?? macro.defaultValue;
      input.placeholder = macro.defaultValue ? `默认值: ${macro.defaultValue}` : `请输入 ${macro.name}`;
      input.addEventListener("input", updatePreview);
      input.addEventListener("keydown", event => {
        if (event.key === "Enter") {
          event.preventDefault();
          commandMacroSubmit?.click();
        }
      });
      inputMap.set(macro.name, input);
      row.append(label, input);
      commandMacroFields.appendChild(row);
      if (idx === 0) setTimeout(() => { input.focus(); input.select(); }, 50);
    });

    updatePreview();
    commandMacroDialog.showModal();
    return true;
  };

  commandMacroSubmit?.addEventListener("click", () => {
    if (!activeMacroTemplate) return;
    const macros = extractCommandMacros(activeMacroTemplate);
    let macroCache = {};
    try {
      macroCache = JSON.parse(localStorage.getItem("masterterm.macroCache") || "{}");
    } catch {
      macroCache = {};
    }

    let resolved = activeMacroTemplate;
    for (const macro of macros) {
      const input = commandMacroFields?.querySelector(`input[data-macro-name="${macro.name}"]`);
      const val = input ? (input.value || macro.defaultValue) : macro.defaultValue;
      if (input && input.value) macroCache[macro.name] = input.value;
      const re = new RegExp(`\\{\\{${macro.name}(:[^}]+)?\\}\\}`, "g");
      resolved = resolved.replace(re, val);
    }
    try {
      localStorage.setItem("masterterm.macroCache", JSON.stringify(macroCache));
    } catch {}
    commandMacroDialog?.close();
    executeResolvedFavoriteCommand(resolved);
  });
  commandMacroCancel?.addEventListener("click", () => commandMacroDialog?.close());
  commandMacroClose?.addEventListener("click", () => commandMacroDialog?.close());

  const renderCommandFavorites = () => {
    commandFavoritesList.textContent = "";
    commandFavoritesEmpty.hidden = commandFavorites.length > 0;
    for (const favorite of commandFavorites) {
      const item = document.createElement("div");
      item.className = "command-favorite-item";
      const name = document.createElement("div");
      name.className = "command-favorite-name";
      name.textContent = favorite.name || favorite.command.split(/\s+/)[0];
      name.title = favorite.name || favorite.command;
      name.addEventListener("click", () => {
        if (sendFavoriteCommand(favorite.command))
          commandFavoritesDialog.close();
      });
      const text = document.createElement("div");
      text.className = "command-favorite-text";
      text.textContent = favorite.command;
      text.title = favorite.command;
      const sendButton = document.createElement("button");
      sendButton.type = "button";
      sendButton.className = "command-favorite-send";
      sendButton.textContent = "发送";
      sendButton.addEventListener("click", () => {
        if (sendFavoriteCommand(favorite.command))
          commandFavoritesDialog.close();
      });
      const editButton = document.createElement("button");
      editButton.type = "button";
      editButton.textContent = "编辑";
      editButton.addEventListener("click", () => beginCommandFavoriteEdit(favorite.id));
      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.textContent = "删除";
      deleteButton.addEventListener("click", () => {
        commandFavorites = commandFavorites.filter(entry => entry.id !== favorite.id);
        persistCommandFavorites();
        if (commandFavoriteEditingId === favorite.id)
          cancelCommandFavoriteEdit();
        renderCommandFavorites();
      });
      item.append(name, text, sendButton, editButton, deleteButton);
      commandFavoritesList.appendChild(item);
    }
  };

  const cancelCommandFavoriteEdit = () => {
    commandFavoriteEditingId = null;
    commandFavoriteEditor.hidden = true;
    commandFavoriteEditName.value = "";
    commandFavoriteEditCommand.value = "";
  };

  const beginCommandFavoriteEdit = (id = null) => {
    commandFavoriteEditingId = id;
    if (id) {
      const favorite = commandFavorites.find(entry => entry.id === id);
      commandFavoriteEditName.value = favorite?.name || "";
      commandFavoriteEditCommand.value = favorite?.command || "";
    } else {
      commandFavoriteEditName.value = "";
      commandFavoriteEditCommand.value = "";
    }
    commandFavoriteEditor.hidden = false;
    commandFavoriteEditName.focus();
  };

  const saveCommandFavoriteEdit = () => {
    const command = commandFavoriteEditCommand.value.trim();
    if (!command) {
      showTransientStatus("收藏的命令内容不能为空。");
      return;
    }
    const name = commandFavoriteEditName.value.trim()
      || command.split(/\s+/)[0];
    if (commandFavoriteEditingId) {
      const favorite = commandFavorites.find(
        entry => entry.id === commandFavoriteEditingId);
      if (favorite) {
        favorite.name = name;
        favorite.command = command;
      }
    } else {
      commandFavorites.push({
        id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        name,
        command
      });
    }
    persistCommandFavorites();
    cancelCommandFavoriteEdit();
    renderCommandFavorites();
    showTransientStatus("收藏已保存。");
  };

  const openCommandFavorites = () => {
    cancelCommandFavoriteEdit();
    renderCommandFavorites();
    commandFavoritesDialog.showModal();
  };

  const openBatchCommand = () => {
    batchCommandStatus.textContent = "";
    batchCommandInput.value = "";
    batchCommandSessions.replaceChildren();
    const connected = [...sessions.values()].filter(
      session => session.connectionType === "ssh"
        && session.state === "connected");
    if (!connected.length) {
      const empty = document.createElement("div");
      empty.className = "batch-command-empty";
      empty.textContent = "没有已连接的 SSH 会话。请先连接服务器。";
      batchCommandSessions.append(empty);
      batchCommandRun.disabled = true;
    } else {
      batchCommandRun.disabled = false;
      for (const session of connected) {
        const label = document.createElement("label");
        label.className = "batch-command-session";
        const check = document.createElement("input");
        check.type = "checkbox";
        check.checked = true;
        check.dataset.sessionId = session.sessionId;
        const span = document.createElement("span");
        span.textContent = `${session.name}（${session.sessionId}）`;
        label.append(check, span);
        batchCommandSessions.append(label);
      }
    }
    batchCommandDialog.showModal();
    batchCommandInput.focus();
  };

  const runBatchCommand = async () => {
    const command = batchCommandInput.value.trim();
    if (!command) {
      batchCommandStatus.textContent = "请输入命令。";
      return;
    }
    const targets = [...batchCommandSessions.querySelectorAll("input:checked")]
      .map(input => input.dataset.sessionId);
    if (!targets.length) {
      batchCommandStatus.textContent = "请至少选择一个会话。";
      return;
    }
    batchCommandRun.disabled = true;
    batchCommandStatus.textContent = `正在发送到 ${targets.length} 个会话…`;
    const data = command + "\r";
    let succeeded = 0;
    let failed = 0;
    for (const sessionId of targets) {
      try {
        await post("session.input", { sessionId, data });
        succeeded++;
      } catch {
        failed++;
      }
    }
    batchCommandStatus.textContent =
      `已发送：${succeeded} 个成功${failed ? `，${failed} 个失败` : ""}`;
    batchCommandRun.disabled = false;
  };

  document.querySelector("#command-favorites-close").addEventListener("click", () => commandFavoritesDialog.close());
  document.querySelector("#command-favorites-done").addEventListener("click", () => commandFavoritesDialog.close());
  document.querySelector("#command-favorite-add").addEventListener("click", () => beginCommandFavoriteEdit());
  document.querySelector("#command-favorite-save-input").addEventListener("click", () => {
    const session = activeSession;
    const input = session?.inputLine?.trim();
    if (!input) {
      showTransientStatus("当前终端没有可保存的输入。");
      return;
    }
    const existing = commandFavorites.find(entry => entry.command === input);
    if (existing) {
      showTransientStatus("当前命令已在收藏中。");
      return;
    }
    commandFavorites.push({
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      name: input.split(/\s+/)[0],
      command: input
    });
    persistCommandFavorites();
    renderCommandFavorites();
    showTransientStatus("已收藏当前输入。");
  });
  document.querySelector("#command-favorite-edit-save").addEventListener("click", saveCommandFavoriteEdit);
  document.querySelector("#command-favorite-edit-cancel").addEventListener("click", cancelCommandFavoriteEdit);
  commandFavoriteEditCommand.addEventListener("keydown", event => {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
      event.preventDefault();
      saveCommandFavoriteEdit();
    }
  });
  document.querySelector("#batch-command-close").addEventListener("click", () => batchCommandDialog.close());
  document.querySelector("#batch-command-cancel").addEventListener("click", () => batchCommandDialog.close());
  batchCommandRun.addEventListener("click", runBatchCommand);
  document.querySelector("#open-log-directory").addEventListener("click", () => {
    post("logs.openDirectory").catch(reportError);
  });

  const showTerminalDiagnostics = sessionId => {
    const session = sessions.get(sessionId);
    if (!session) return;
    const terminal = session.terminal;
    const buffer = terminal.buffer.active;
    let cellWidth = "?";
    let cellHeight = "?";
    try {
      const core = terminal._core || terminal._renderService?.dimensions;
      const dims = terminal._core?.dimensions || terminal._renderService?.dimensions;
      cellWidth = String(dims?.css?.cell?.width ?? "?");
      cellHeight = String(dims?.css?.cell?.height ?? "?");
    } catch { /* older xterm internals */ }
    const shell = session.connectionType === "local"
      ? "Windows 本地终端（cmd.exe）"
      : session.connectionType === "serial"
        ? "串口字节流（无 Shell）"
        : "远端默认登录 Shell";
    const locale = session.connectionType === "local"
      ? "zh_CN.UTF-8（chcp 65001 + set LANG/LC_CTYPE/LC_ALL）"
      : session.connectionType === "serial"
        ? "—"
        : "C.UTF-8（客户端请求 LANG/LC_CTYPE/LC_ALL，受 sshd AcceptEnv 影响）";
    terminalDiagnosticsContent.textContent = [
      `会话：${session.name} · ${session.sessionId}`,
      `状态：${session.state}`,
      `连接类型：${session.connectionType}`,
      `Shell：${shell}`,
      `编码：UTF-8`,
      `locale：${locale}`,
      `终端尺寸：${terminal.cols} 列 × ${terminal.rows} 行`,
      `字体：${terminal.options.fontFamily || "?"} · ${terminal.options.fontSize || "?"}pt`,
      `字元度量：宽 ${cellWidth}px × 高 ${cellHeight}px`,
      `设备像素比：${window.devicePixelRatio || "?"}`,
      `缓冲区：${buffer.length} 行，视口位置 ${buffer.viewportY}`,
      `光标：第 ${buffer.cursorY + 1} 行，第 ${buffer.cursorX + 1} 列`,
      `alternate screen：${session.alternateScreen ? "是" : "否"}`,
      `远端 alternate screen：${session.remoteAlternateScreen ? "是" : "否"}`,
      "",
      "最近原始字节（十六进制）：",
      session.lastRawBytes || "（暂无数据）",
      "",
      "最近 UTF-8 解码结果：",
      session.lastDecodedText || "（暂无数据）"
    ].join("\n");
    terminalDiagnosticsDialog.showModal();
  };
  document.querySelector("#terminal-diagnostics-close").addEventListener("click", () => terminalDiagnosticsDialog.close());
  document.querySelector("#terminal-diagnostics-close-action").addEventListener("click", () => terminalDiagnosticsDialog.close());
  const reconnectSession = async (sessionId, options = {}) => {
    const session = sessions.get(sessionId);
    if (!session) return;
    const profile = profilesByIndex.get(session.profileIndex);
    if (!profile) {
      showTransientStatus("连接配置不存在，无法重新连接。");
      return;
    }
    const wasActive = (activeSession === session);
    const keepBackground = options.keepBackground ?? (!wasActive);
    if (session.connectionType === "rdp") {
      await closeSession(sessionId);
      await connectProfile(profile, { keepBackground });
      return;
    }
    if (session.state !== "closed" && session.state !== "error") return;
    try {
      await post("session.close", { sessionId });
      disposeSession(sessionId);
      await connectProfile(profile, { keepBackground });
    } catch (error) {
      reportError(error);
    }
  };
  const moveTerminalTab = (sessionId, direction) => {
    const tab = document.querySelector(
      `.terminal-tab[data-session-id="${CSS.escape(sessionId)}"]`);
    if (!tab) return;
    const host = tab.parentElement;
    if (!host) return;
    const sibling = direction < 0 ? tab.previousElementSibling : tab.nextElementSibling;
    if (!sibling || sibling.classList.contains("terminal-create-group") || sibling.classList.contains("split-nav-tab")) return;
    host.insertBefore(direction < 0 ? tab : sibling,
      direction < 0 ? sibling : tab);
    tab.scrollIntoView({ block: "nearest", inline: "nearest" });
    if (splitMode) refreshSplitRightIds();
  };
  const copyTerminalSelection = session => {
    const selected = session?.terminal.getSelection() || "";
    if (!selected) return false;
    post("clipboard.write", { text: selected })
      .then(() => { showToast("内容已复制"); })
      .catch(reportError);
    return true;
  };
  const pasteIntoTerminal = session => {
    if (!session || session.state === "closed" || session.state === "error") return;
    post("clipboard.read")
      .then(text => {
        if (!text) return;
        hideCompletion(session);
        // Do not invoke xterm's browser paste path here. Ctrl+Shift+V can
        // otherwise trigger both that path and this explicit clipboard read
        // after WebView2 grants clipboard permission.
        post("session.input", { sessionId: session.sessionId, data: text }).catch(reportError);
        session.terminal.focus();
      })
      .catch(reportError);
  };
  const selectionContainsTerminalPoint = (session, event) => {
    const selection = session.terminal.getSelectionPosition?.();
    if (!selection?.start || !selection?.end || !session.terminal.getSelection()) return false;
    const screen = session.output.querySelector(".xterm-screen") || session.output.querySelector(".xterm");
    const bounds = screen?.getBoundingClientRect();
    if (!bounds?.width || !bounds.height) return false;
    const column = Math.max(0, Math.min(session.terminal.cols - 1,
      Math.floor((event.clientX - bounds.left) * session.terminal.cols / bounds.width)));
    const viewportY = session.terminal.buffer.active.viewportY;
    const row = viewportY + Math.max(0, Math.min(session.terminal.rows - 1,
      Math.floor((event.clientY - bounds.top) * session.terminal.rows / bounds.height)));
    const { start, end } = selection;
    if (row < start.y || row > end.y) return false;
    if (start.y === end.y) return column >= start.x && column < end.x;
    if (row === start.y) return column >= start.x;
    if (row === end.y) return column < end.x;
    return true;
  };
  const showTerminalOutputContextMenu = (event, session) => {
    event.preventDefault();
    terminalOutputContextSessionId = session.sessionId;
    const hasSelection = selectionContainsTerminalPoint(session, event);
    terminalOutputContextMenu.querySelector('[data-action="copy"]').hidden = !hasSelection;
    terminalOutputContextMenu.querySelector('[data-action="paste"]').hidden = hasSelection;
    terminalOutputContextMenu.hidden = false;
    const bounds = terminalOutputContextMenu.getBoundingClientRect();
    terminalOutputContextMenu.style.left = `${Math.max(8, Math.min(event.clientX, window.innerWidth - bounds.width - 8))}px`;
    terminalOutputContextMenu.style.top = `${Math.max(8, Math.min(event.clientY, window.innerHeight - bounds.height - 8))}px`;
  };

  // 内置常用运维命令通配符与参数模板库（70+高频实用指令）
  const BUILTIN_COMMAND_TEMPLATES = [
    // --- tar / zip / 打包与解压缩 ---
    { cmd: "tar", value: 'tar -xzvf <archive.tar.gz> -C <target_dir>', desc: "解压 .tar.gz 文件到目标目录" },
    { cmd: "tar", value: 'tar -czvf <archive.tar.gz> <source_dir>', desc: "将指定目录打包压缩为 .tar.gz 格式" },
    { cmd: "tar", value: 'tar -xjvf <archive.tar.bz2> -C <target_dir>', desc: "解压 .tar.bz2 文件到目标目录" },
    { cmd: "tar", value: 'tar -cjvf <archive.tar.bz2> <source_dir>', desc: "将目录压缩为高压缩率 .tar.bz2 格式" },
    { cmd: "tar", value: 'tar -xvf <archive.tar.xz> -C <target_dir>', desc: "解压 .tar.xz 文件到目标目录" },
    { cmd: "tar", value: 'tar -tf <archive.tar.gz>', desc: "不解压直接列出压缩包内所有文件清单" },
    { cmd: "tar", value: 'tar -czvf <archive.tar.gz> --exclude="<node_modules>" <source_dir>', desc: "打包目录并排除指定的子目录或文件" },
    { cmd: "unzip", value: 'unzip <archive.zip> -d <target_dir>', desc: "解压 .zip 压缩包到目标目录" },
    { cmd: "zip", value: 'zip -r <archive.zip> <folder>', desc: "递归压缩文件夹为 .zip 压缩包" },

    // --- find (文件与目录查找) ---
    { cmd: "find", value: 'find <path> -type f', desc: "查找指定路径下的所有普通文件" },
    { cmd: "find", value: 'find <path> -type d', desc: "查找指定路径下的所有目录" },
    { cmd: "find", value: 'find <path> -type l', desc: "查找指定路径下的所有软链接" },
    { cmd: "find", value: 'find <path> -type f -mtime -7', desc: "查找最近 7 天内修改过的普通文件" },
    { cmd: "find", value: 'find <path> -type f -mtime -1', desc: "查找最近 24 小时(1天)内修改过的文件" },
    { cmd: "find", value: 'find <path> -type f -mtime +30', desc: "查找 30 天前修改的老旧文件" },
    { cmd: "find", value: 'find <path> -type f -mmin -60', desc: "查找最近 60 分钟内修改过的文件" },
    { cmd: "find", value: 'find <path> -type f -size +100M', desc: "查找大于 100MB 的大文件" },
    { cmd: "find", value: 'find <path> -type f -size +1G', desc: "查找大于 1GB 的超大文件" },
    { cmd: "find", value: 'find <path> -type f -name "*.log"', desc: "在指定目录下查找普通日志文件" },
    { cmd: "find", value: 'find <path> -name "*.tmp" -exec rm -f {} +', desc: "按通配符批量查找并彻底删除文件" },
    { cmd: "find", value: 'find <path> -maxdepth 2 -type f -name "*.conf"', desc: "限制最大目录深度查找配置文件" },
    { cmd: "find", value: 'find <path> -type f -perm 0777', desc: "查找权限为 777 的全开放危险文件" },
    { cmd: "find", value: 'find <path> -empty -type f', desc: "查找所有 0 字节的空文件" },
    { cmd: "find", value: 'find <path> -name "<pattern>" -not -path "*/.*"', desc: "排除隐藏文件/目录进行模式查找" },
    { cmd: "find", value: 'find <path> -type d -name "node_modules"', desc: "查找所有指定名称的目录" },
    { cmd: "find", value: 'find . -name "<*.log>"', desc: "在当前目录递归查找指定名称通配符文件" },
    { cmd: "find", value: 'find <path> -name "<*.log>" -type f', desc: "在指定目录下查找普通文件" },

    // --- grep (文本关键字搜索) ---
    { cmd: "grep", value: 'grep -rn "<keyword>" <path>', desc: "递归查找当前目录下包含关键字的文件及行号" },
    { cmd: "grep", value: 'grep -in "<keyword>" <file>', desc: "忽略大小写在文件中搜索指定内容及行号" },
    { cmd: "grep", value: 'grep -v "^#" <file>', desc: "排除以 # 开头的配置注释行" },
    { cmd: "grep", value: 'grep -C <5> "<ERROR>" <file>', desc: "显示匹配行前后各 5 行的上下文内容" },
    { cmd: "grep", value: 'grep -E "<pattern>" <file>', desc: "使用扩展正则表达式进行模式匹配" },
    { cmd: "grep", value: 'grep -rl "<keyword>" <path>', desc: "递归查找并仅列出包含关键字的文件路径" },

    // --- docker & docker compose (容器与编排管理) ---
    { cmd: "docker", value: 'docker ps -a --format "table {{.ID}}\t{{.Names}}\t{{.Status}}\t{{.Ports}}"', desc: "格式化单行查看所有容器的运行状态与端口" },
    { cmd: "docker", value: 'docker logs -f --tail <100> <container>', desc: "实时追踪查看指定容器的最新 100 行日志" },
    { cmd: "docker", value: 'docker exec -it <container> /bin/bash', desc: "进入容器交互式 Bash 命令行终端" },
    { cmd: "docker", value: 'docker exec -it <container> /bin/sh', desc: "进入轻量级容器 sh 命令行终端" },
    { cmd: "docker", value: 'docker run -d -p <8080:80> --name <name> --restart always <image>', desc: "后台启动容器并映射端口与重启策略" },
    { cmd: "docker", value: 'docker restart <container>', desc: "重启指定容器" },
    { cmd: "docker", value: 'docker stop <container>', desc: "优雅停止正在运行的容器" },
    { cmd: "docker", value: 'docker rm -f <container>', desc: "强制删除指定容器" },
    { cmd: "docker", value: 'docker rmi <image>', desc: "删除指定 Docker 镜像" },
    { cmd: "docker", value: 'docker system prune -a --volumes -f', desc: "彻底清理所有无用容器、镜像与数据卷" },
    { cmd: "docker", value: 'docker stats --no-stream', desc: "单次抓取并列出所有容器当前 CPU 与内存占用" },
    { cmd: "docker", value: 'docker compose -f <docker-compose.yml> up -d', desc: "后台启动 Docker Compose 编排服务" },
    { cmd: "docker", value: 'docker compose logs -f --tail <100> <service>', desc: "实时跟踪 Docker Compose 编排服务日志" },
    { cmd: "docker", value: 'docker compose restart <service>', desc: "重启 Docker Compose 编排的指定服务" },
    { cmd: "docker", value: 'docker compose down', desc: "停止并移除 Docker Compose 容器与网络" },

    // --- systemctl / journalctl (系统服务与日志) ---
    { cmd: "systemctl", value: 'systemctl status <service>', desc: "查看指定 systemd 服务的运行状态与详情" },
    { cmd: "systemctl", value: 'systemctl restart <service>', desc: "重启指定系统服务" },
    { cmd: "systemctl", value: 'systemctl start <service>', desc: "启动指定系统服务" },
    { cmd: "systemctl", value: 'systemctl stop <service>', desc: "停止指定系统服务" },
    { cmd: "systemctl", value: 'systemctl enable --now <service>', desc: "配置开机自启并立即启动该服务" },
    { cmd: "systemctl", value: 'systemctl disable <service>', desc: "禁用指定服务的开机自启" },
    { cmd: "systemctl", value: 'systemctl daemon-reload', desc: "重新加载 systemd 守护进程配置文件" },
    { cmd: "journalctl", value: 'journalctl -u <service> -f --no-tail', desc: "实时跟踪指定服务的 systemd 日志" },
    { cmd: "journalctl", value: 'journalctl -xeu <service>', desc: "查看服务启动失败时的详细上下文与报错" },
    { cmd: "journalctl", value: 'journalctl --since "<today>" -u <service>', desc: "查看今天以来的指定服务运行日志" },
    { cmd: "journalctl", value: 'journalctl --vacuum-size=<500M>', desc: "清理压缩 systemd 历史日志至指定体积" },

    // --- 网络与端口诊断 (ss / netstat / lsof / tcpdump) ---
    { cmd: "ss", value: 'ss -tulpn | grep :<port>', desc: "快速查看占用指定端口的进程与套接字" },
    { cmd: "netstat", value: 'netstat -tulnp | grep <port>', desc: "查看当前监听的网络端口及所属 PID" },
    { cmd: "lsof", value: 'lsof -i :<port>', desc: "列出正在监听或连接指定端口的进程详情" },
    { cmd: "lsof", value: 'lsof -p <PID>', desc: "查看指定进程打开的所有文件句柄与网络连接" },
    { cmd: "tcpdump", value: 'tcpdump -i <eth0> -nn -s0 -c 100 port <port>', desc: "抓取指定网卡指定端口的前 100 个网络数据包" },

    // --- 进程管理 (ps / kill / pkill / top) ---
    { cmd: "ps", value: 'ps aux | grep <process_name>', desc: "查看指定名称进程的 CPU/内存占用与 PID" },
    { cmd: "ps", value: 'ps -ef | grep <process_name>', desc: "查看父子进程层级与完整启动参数命令行" },
    { cmd: "kill", value: 'kill -9 <PID>', desc: "向指定 PID 进程发送 SIGKILL 强制终止" },
    { cmd: "kill", value: 'kill -15 <PID>', desc: "向指定 PID 进程发送 SIGTERM 优雅退出信号" },
    { cmd: "pkill", value: 'pkill -9 -f <process_name>', desc: "根据进程名称全字匹配并强制终止所有同名进程" },
    { cmd: "killall", value: 'killall -9 <process_name>', desc: "强制杀死指定命令名称的所有进程实例" },

    // --- HTTP 与网络请求 (curl / wget) ---
    { cmd: "curl", value: 'curl -I <https://domain>', desc: "发送 HEAD 请求仅获取远程 HTTP 响应头" },
    { cmd: "curl", value: 'curl -O -C - <https://domain/file>', desc: "大文件断点续传并按原文件名保存到本地" },
    { cmd: "curl", value: 'curl -X POST -H "Content-Type: application/json" -d \'<{"key":"val"}>\' <url>', desc: "向远程接口发送 JSON 格式的 POST 请求" },
    { cmd: "curl", value: 'curl -sSL <https://domain/script.sh> | bash', desc: "静默下载远程 Shell 脚本并直接执行安装" },
    { cmd: "wget", value: 'wget -c <url>', desc: "使用 wget 断点续传下载远程大文件" },

    // --- 文件权限与安全锁定 (chmod / chown / chattr) ---
    { cmd: "chmod", value: 'chmod 755 <path>', desc: "设置为标准所有者读写执行、其他人读执行权限" },
    { cmd: "chmod", value: 'chmod 600 ~/.ssh/<id_rsa>', desc: "将 SSH 私钥文件权限设置为安全只读" },
    { cmd: "chmod", value: 'chmod 644 <path>', desc: "将普通文件权限设置为所有者读写、其他人只读" },
    { cmd: "chmod", value: 'chmod -R 755 <path>', desc: "递归设置目录下所有文件的标准执行权限" },
    { cmd: "chown", value: 'chown -R <www-data:www-data> <path>', desc: "递归更改目录归属用户和所属用户组" },
    { cmd: "chattr", value: 'chattr +i <file>', desc: "锁定关键文件禁止任何用户修改、删除或重命名" },
    { cmd: "chattr", value: 'chattr -i <file>', desc: "解除关键文件的不可修改只读属性" },

    // --- 远程数据同步 (rsync / scp / ssh) ---
    { cmd: "rsync", value: 'rsync -avzP <source_dir/> <user@host:/target_dir/>', desc: "带进度条与断点续传同步目录到远程" },
    { cmd: "rsync", value: 'rsync -avz --delete <src/> <dst/>', desc: "镜像同步目录并删除目标端多余的多出文件" },
    { cmd: "scp", value: 'scp -P <port> -r <local_dir> <user@host:/remote_dir>', desc: "通过指定端口递归拷贝本地目录至远程" },
    { cmd: "ssh", value: 'ssh -p <port> <user@host>', desc: "连接指定端口的远程 SSH 服务器" },

    // --- 磁盘与目录空间排查 (df / du) ---
    { cmd: "df", value: 'df -h', desc: "以易读的 GB/MB 单位列出所有挂载磁盘剩余容量" },
    { cmd: "df", value: 'df -i', desc: "查看各文件系统的 inode 节点使用情况" },
    { cmd: "du", value: 'du -sh * | sort -hr | head -n 10', desc: "分析当前目录下体积最大的前 10 个文件或文件夹" },
    { cmd: "du", value: 'du -h --max-depth=1 <path> | sort -hr', desc: "统计并排序指定路径下一级子目录的占用空间" },

    // --- 代码版本控制 (git) ---
    { cmd: "git", value: 'git status -s', desc: "以单行精简模式显示当前仓库文件变更状态" },
    { cmd: "git", value: 'git log --oneline --graph -n <15>', desc: "查看单行带分支演进图谱的最近 15 次提交" },
    { cmd: "git", value: 'git diff <file>', desc: "查看指定工作区文件的未暂存代码差异" },
    { cmd: "git", value: 'git checkout -b <branch_name>', desc: "创建并切换到指定名称的新分支" },
    { cmd: "git", value: 'git reset --hard HEAD~1', desc: "硬回退撤销最近一次提交并丢弃工作区修改" },
    { cmd: "git", value: 'git stash push -m "<message>"', desc: "附带说明文字暂存当前未提交的修改" },
    { cmd: "git", value: 'git stash pop', desc: "恢复并应用最近一次暂存的工作区修改" },

    // --- 文本流处理 (sed / awk) ---
    { cmd: "sed", value: "sed -i 's/<old_text>/<new_text>/g' <file>", desc: "在指定文件中就地批量全局替换指定文本" },
    { cmd: "awk", value: "awk '{print $<1>}' <file> | sort | uniq -c | sort -nr | head -n 10", desc: "统计文本中指定列频次最高的 Top 10" },
    { cmd: "awk", value: "awk -F':' '{print $1,$3}' /etc/passwd", desc: "以冒号为分隔符提取第 1 列与第 3 列" },

    // --- 日志与文本查看 (tail / head) ---
    { cmd: "tail", value: 'tail -f -n <100> <file.log>', desc: "动态实时追踪文件末尾最新 100 行日志" },
    { cmd: "tail", value: 'tail -n <500> <file.log> | grep "<ERROR>"', desc: "在文件末尾 500 行日志中过滤关键词" },
    { cmd: "head", value: 'head -n <20> <file>', desc: "快速查看文件前 20 行内容" },

    // --- 防火墙管理 (ufw / firewalld) ---
    { cmd: "ufw", value: 'ufw status numbered', desc: "查看带规则编号的 ufw 防火墙放行列表" },
    { cmd: "ufw", value: 'ufw allow <port>/tcp', desc: "在 ufw 防火墙中放行指定 TCP 端口" },
    { cmd: "firewall-cmd", value: 'firewall-cmd --list-all', desc: "查看 firewalld 当前区域的所有开放策略" },
    { cmd: "firewall-cmd", value: 'firewall-cmd --add-port=<port>/tcp --permanent && firewall-cmd --reload', desc: "firewalld 永久开放端口并重载" },

    // --- 计划任务 (crontab) ---
    { cmd: "crontab", value: 'crontab -l', desc: "列出当前登录用户的所有定时计划任务" },
    { cmd: "crontab", value: 'crontab -e', desc: "编辑当前登录用户的定时计划任务配置" }
  ];

  // 解析模板分段与占位符
  const parseTemplateSegments = raw => {
    if (typeof raw !== "string") return [];
    const regex = /<([^>]+)>/g;
    let match;
    let lastIndex = 0;
    const segments = [];
    while ((match = regex.exec(raw)) !== null) {
      const prefix = raw.slice(lastIndex, match.index);
      const placeholder = match[0];
      segments.push({ prefix, placeholder });
      lastIndex = regex.lastIndex;
    }
    const suffix = raw.slice(lastIndex);
    if (segments.length === 0) {
      segments.push({ prefix: raw, placeholder: "" });
    } else if (suffix) {
      segments.push({ prefix: suffix, placeholder: "" });
    }
    return segments;
  };

  BUILTIN_COMMAND_TEMPLATES.forEach(t => {
    t.segments = parseTemplateSegments(t.value);
    t.hasPlaceholders = /<[^>]+>/.test(t.value);
  });

  // 词法拆解命令行或模板（支持引号包裹）
  const tokenizeCommandArgs = str => {
    if (!str) return [];
    const tokens = [];
    const regex = /[^\s"']+|"([^"]*)"|'([^']*)'/g;
    let match;
    while ((match = regex.exec(str)) !== null) {
      tokens.push(match[0]);
    }
    return tokens;
  };

  const hasTemplatePlaceholder = tok => /<[^>]+>/.test(tok);
  const isTemplatePlaceholder = tok => /^<[^>]+>$/.test(tok) || /^"<[^>]+>"$/.test(tok);

  // 提取占位符中的默认值（例如 -<7> 提取为 -7，+<100M> 提取为 +100M，"<*.log>" 提取为 "*.log"）
  const extractDefaultValueFromPlaceholder = tok => {
    if (typeof tok !== "string") return "";
    let val = tok.replace(/<([^>]+)>/g, '$1');
    if (val.startsWith('"') && val.endsWith('"')) {
      val = val.slice(1, -1);
    }
    return val;
  };

  // 评估模板与用户输入的匹配度、计算缺失增量 (incrementalText) 与对齐后的命令
  const evaluateTemplateMatch = (commandLine, template) => {
    if (!template.segments) {
      if (typeof parseTemplateSegments === "function") {
        template.segments = parseTemplateSegments(template.value);
      } else if (typeof template.value === "string") {
        const segIdx = template.value.indexOf("<");
        const prefix = segIdx !== -1 ? template.value.slice(0, segIdx) : template.value;
        template.segments = [{ prefix, placeholder: "" }];
      } else {
        template.segments = [];
      }
    }
    const rawInput = commandLine;
    const hasTrailingSpace = /\s$/.test(rawInput);
    const userTokens = tokenizeCommandArgs(rawInput);
    if (!userTokens.length) return null;

    const uCmd = userTokens[0].toLowerCase();
    const tplTokens = tokenizeCommandArgs(template.value);
    if (!tplTokens.length) return null;
    const tCmd = template.cmd.toLowerCase();

    const seg0Prefix = template.segments[0]?.prefix || template.value;
    const seg0Lower = seg0Prefix.toLowerCase();
    const inputLower = rawInput.toLowerCase();

    // 1. 如果用户输入属于模板首段固定前缀（如 "ps" 匹配 "ps -ef | grep "，或 "journal" 匹配 "journalctl --since \""）
    // 一次性补全到首个占位符前的全部固定内容
    if (seg0Lower.startsWith(inputLower) && inputLower.length >= 2) {
      const incrementalText = seg0Prefix.slice(rawInput.length);
      if (incrementalText.length > 0) {
        return {
          score: 350 - incrementalText.length,
          incrementalText,
          resolvedValue: template.value,
          displayBadge: incrementalText.trim() || seg0Prefix.trim(),
          extraTokens: tplTokens.length - userTokens.length
        };
      }
    }

    // 如果用户仅输入了命令名一部分（例如只输了单字符前缀或 tCmd.startsWith(uCmd)）
    if (userTokens.length === 1 && !hasTrailingSpace) {
      if (tCmd.startsWith(uCmd)) {
        const remaining = tCmd.slice(uCmd.length);
        return {
          score: 200,
          incrementalText: remaining + " ",
          resolvedValue: template.value,
          displayBadge: tCmd,
          extraTokens: tplTokens.length - 1
        };
      }
      return null;
    }

    // 2. 如果命令首词不匹配
    if (uCmd !== tCmd) return null;

    // 3. 多参数与选项匹配：如果用户键入了选项（以 - 开头），检查模板是否包含该选项
    const userArgs = userTokens.slice(1);
    const tplArgs = tplTokens.slice(1);

    const userFlags = userArgs.filter(a => a.startsWith("-")).map(a => a.toLowerCase());
    const tplFlags = tplArgs.filter(a => a.startsWith("-")).map(a => a.toLowerCase());

    // 必须满足：用户输入的每一个完整选项（或者当前正在输入的选项前缀），模板必须能够支持！
    for (let i = 0; i < userFlags.length; i++) {
      const uf = userFlags[i];
      const isLastToken = (i === userFlags.length - 1) && !hasTrailingSpace && userArgs[userArgs.length - 1].toLowerCase() === uf;
      if (isLastToken) {
        if (!tplFlags.some(tf => tf.startsWith(uf))) {
          return null;
        }
      } else {
        if (!tplFlags.includes(uf)) {
          return null;
        }
      }
    }

    // 4. 对齐用户参数与模板参数
    let uIdx = 0;
    let tIdx = 0;
    let score = 300;
    let partialTokenCompletion = "";
    let placeholderReplacements = [];

    while (uIdx < userArgs.length && tIdx < tplArgs.length) {
      const u = userArgs[uIdx];
      const t = tplArgs[tIdx];

      if (isTemplatePlaceholder(t) || (t.startsWith("<") && t.endsWith(">"))) {
        if (!u.startsWith("-") || t.startsWith("<-")) {
          placeholderReplacements.push({ placeholder: t, value: u });
          score += 40;
          uIdx++;
          tIdx++;
          continue;
        } else {
          const laterIdx = tplArgs.findIndex((item, idx) => idx > tIdx && item.toLowerCase() === u.toLowerCase());
          if (laterIdx !== -1) {
            tIdx = laterIdx;
            continue;
          }
        }
      }

      if (u.toLowerCase() === t.toLowerCase()) {
        score += 80;
        uIdx++;
        tIdx++;
        continue;
      }

      if (uIdx === userArgs.length - 1 && !hasTrailingSpace && t.toLowerCase().startsWith(u.toLowerCase())) {
        score += 60;
        partialTokenCompletion = t.slice(u.length);
        uIdx++;
        tIdx++;
        break;
      }

      const foundIdx = tplArgs.findIndex((item, idx) => idx > tIdx && item.toLowerCase() === u.toLowerCase());
      if (foundIdx !== -1) {
        score += 30;
        tIdx = foundIdx + 1;
        uIdx++;
        continue;
      }

      // 如果是非选项参数（如 status / daemon-reload / checkout / ps），且模板对应位置也是固定非选项词，且模板后续不包含该词：
      // 说明命令分支/子命令不匹配，必须直接拒绝匹配！
      if (!u.startsWith("-") && !isTemplatePlaceholder(t)) {
        return null;
      }

      if (u.startsWith("-")) {
        score -= 200;
      } else {
        score -= 30;
      }
      uIdx++;
    }

    if (score < 100) return null;

    // 5. 计算增量追加文本 incrementalText
    let incrementalText = "";
    let displayBadge = "";

    if (partialTokenCompletion) {
      const remainingAfterPartial = [];
      let nextIsPh = false;
      for (let i = tIdx; i < tplArgs.length; i++) {
        const tok = tplArgs[i];
        if (isTemplatePlaceholder(tok)) {
          nextIsPh = true;
          break;
        }
        remainingAfterPartial.push(tok);
      }
      const tail = remainingAfterPartial.length ? " " + remainingAfterPartial.join(" ") : "";
      incrementalText = partialTokenCompletion + tail + (nextIsPh ? " " : "");
      displayBadge = incrementalText.trim();
    } else if (tIdx < tplArgs.length) {
      const remainingTokens = [];
      let nextIsPlaceholder = false;

      for (let i = tIdx; i < tplArgs.length; i++) {
        const tok = tplArgs[i];
        if (isTemplatePlaceholder(tok)) {
          nextIsPlaceholder = true;
          break;
        }
        remainingTokens.push(tok);
      }

      if (remainingTokens.length > 0) {
        const joined = remainingTokens.join(" ");
        incrementalText = (hasTrailingSpace ? "" : " ") + joined + (nextIsPlaceholder ? " " : "");
        displayBadge = joined;
      } else if (nextIsPlaceholder) {
        incrementalText = hasTrailingSpace ? "" : " ";
        displayBadge = tplArgs[tIdx];
      }
    }

    let resolvedValue = template.value;
    for (const item of placeholderReplacements) {
      resolvedValue = resolvedValue.replace(item.placeholder, item.value);
    }

    return {
      score: score - (tplArgs.length * 2),
      incrementalText,
      resolvedValue,
      displayBadge,
      placeholderReplacements,
      extraTokens: Math.max(0, tplArgs.length - userArgs.length)
    };
  };

  const completionCandidates = session => {
    const input = session.commandLine;
    if (!input) return [];
    const ranked = [];

    // 1. 优先匹配内置运维常用通配符与参数模板（基于多 Token 打分与参数对齐）
    for (const t of BUILTIN_COMMAND_TEMPLATES) {
      const match = evaluateTemplateMatch(input, t);
      if (match) {
        ranked.push({
          value: match.resolvedValue || t.value,
          templateValue: t.value,
          desc: t.desc,
          isTemplate: true,
          hasPlaceholders: t.hasPlaceholders,
          segments: t.segments,
          incrementalText: match.incrementalText,
          displayBadge: match.displayBadge,
          score: match.score,
          extraTokens: match.extraTokens,
          phrase: true,
          frequency: 999,
          recency: 9999
        });
      }
    }

    // 2. 匹配历史命令
    const tokenStart = input.search(/\S*$/);
    const commandPrefix = input.slice(0, tokenStart);
    const tokenPrefix = input.slice(tokenStart);
    const phrases = new Map();
    session.commandHistory.forEach((command, index) => {
      const words = command.trim().split(/\s+/).filter(Boolean);
      if (words.length < 3 || words[0] === words[1]) return;
      for (const count of [2, 3]) {
        if (words.length < count) continue;
        const phrase = words.slice(0, count).join(" ");
        const stat = phrases.get(phrase) || { frequency: 0, recency: index };
        stat.frequency += 1;
        stat.recency = index;
        phrases.set(phrase, stat);
      }
    });
    for (const [value, stat] of phrases) {
      const wordCount = value.split(" ").length;
      if ((wordCount < 3 || stat.frequency >= 2) && value.startsWith(input) && value.length > input.length)
        ranked.push({
          value,
          phrase: true,
          frequency: stat.frequency,
          recency: stat.recency,
          incrementalText: value.slice(input.length),
          score: 100 + stat.frequency * 10
        });
    }
    for (let index = session.commandHistory.length - 1; index >= 0; --index) {
      const value = session.commandHistory[index];
      if (!value.startsWith(commandPrefix) || value.length <= input.length
          || !value.slice(tokenStart).startsWith(tokenPrefix) || ranked.some(item => item.value === value)) continue;
      ranked.push({
        value,
        phrase: false,
        frequency: 1,
        recency: index,
        incrementalText: value.startsWith(input) ? value.slice(input.length) : "",
        score: 50 + index
      });
    }

    // 综合打分排序：
    // 1. 严格保证系统常用命令模板 (isTemplate) 置顶排在上方，历史命令排在下方
    // 2. 优先存在真正增量 (incrementalText) 的候选
    // 3. 模板内部根据打分、词组频次与输入长度排序
    ranked.sort((left, right) => {
      const leftIsTpl = Boolean(left.isTemplate);
      const rightIsTpl = Boolean(right.isTemplate);
      if (rightIsTpl !== leftIsTpl) {
        return rightIsTpl ? 1 : -1;
      }

      const leftHasInc = Boolean(left.incrementalText && left.incrementalText.length > 0);
      const rightHasInc = Boolean(right.incrementalText && right.incrementalText.length > 0);
      if (rightHasInc !== leftHasInc) return rightHasInc ? 1 : -1;

      return (right.score || 0) - (left.score || 0)
        || Number(right.phrase) - Number(left.phrase)
        || right.frequency - left.frequency || left.value.length - right.value.length
        || right.recency - left.recency;
    });

    return ranked.slice(0, 100);
  };
  const hideCompletion = session => {
    session.completion.visible = false;
    session.completion.explicitlySelected = false;
    session.completion.popup.hidden = true;
    session.completion.candidates = [];
  };
  const getTerminalBufferLineInfo = session => {
    try {
      if (session?.fullScreen || session?.remoteAlternateScreen) {
        return { commandLine: "", isAtEnd: true };
      }
      const buffer = session?.terminal?.buffer?.active;
      if (buffer) {
        const line = buffer.getLine(buffer.baseY + buffer.cursorY);
        if (line) {
          const fullLine = line.translateToString(false);
          const textBeforeCursor = fullLine.slice(0, buffer.cursorX);
          const textAfterCursor = fullLine.slice(buffer.cursorX);
          const match = textBeforeCursor.match(/^(?:.*?[#$%>❯➜]|\S+>)\s*(.*)$/);
          if (match && typeof match[1] === "string") {
            return {
              commandLine: match[1],
              isAtEnd: textAfterCursor.trim().length === 0
            };
          }
        }
      }
    } catch (_) {}
    return {
      commandLine: session?.commandLine || "",
      isAtEnd: (session?.commandCursor || 0) === (session?.commandLine?.length || 0)
    };
  };
  const colorizeTerminalLine = line => {
    if (!line || line.includes("\x1b")) return line;
    const errorPattern = /\b(error|failed|failure|fatal|exception|denied|refused|timed out|timeout)\b/i;
    const warningPattern = /\b(warn|warning|deprecated|retrying|unavailable)\b/i;
    const successPattern = /\b(connected|connection established|success|successful|started|running|active|ready|enabled|online)\b/i;
    const networkDiagnostic = /flags=|RX packets|TX packets|RX errors|TX errors|dropped |overruns |carrier |collisions |netmask/i.test(line);
    if (networkDiagnostic) {
      const tokenPattern = /\b(RX|TX|inet6?|ether|mtu|netmask|broadcast|txqueuelen|scopeid|prefixlen|ipv6|errors|dropped|overruns|carrier|collisions|UP|RUNNING|BROADCAST|MULTICAST|LOOPBACK)\b|\b(?:\d{1,3}\.){3}\d{1,3}\b|\b(?:[0-9A-Fa-f]{1,4}:){2,}[0-9A-Fa-f:.%]+\b/gi;
      return line.replace(/^([A-Za-z0-9_.-]+:)(?=\s*flags=)/, "\x1b[96;1m$1\x1b[0m")
        .replace(tokenPattern, token => {
          const word = token.toUpperCase();
          const color = word === "UP" || word === "RUNNING" ? "92;1"
            : /^(BROADCAST|MULTICAST|LOOPBACK)$/.test(word) ? "93"
            : /^(RX|TX)$/.test(word) ? "96;1"
            : /^(ERRORS|DROPPED|OVERRUNS|CARRIER|COLLISIONS)$/.test(word) ? "91"
            : token.includes(":") || /^\d/.test(token) ? "95;1" : "95";
          return `\x1b[${color}m${token}\x1b[0m`;
        });
    }
    if (errorPattern.test(line)) return `\x1b[31m${line}\x1b[0m`;
    if (warningPattern.test(line)) return `\x1b[33m${line}\x1b[0m`;
    if (successPattern.test(line)) return `\x1b[32m${line}\x1b[0m`;
    return line.replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, "\x1b[35m$&\x1b[0m");
  };
  const colorizeTerminalOutput = text => {
    if (!text || text.includes("\x1b")) return text;
    return text.replace(/([^\r\n]*)(\r\n|\r|\n)/g,
      (_, line, ending) => `${colorizeTerminalLine(line)}${ending}`);
  };
  const positionCompletion = session => {
    const popup = session.completion.popup;
    const panelBounds = session.panel.getBoundingClientRect();
    const cursor = session.output.querySelector(".xterm-cursor");
    const cursorBounds = cursor?.getBoundingClientRect();
    const margin = 8;
    const fallbackBottom = 42;
    const maximumWidth = Math.max(180, panelBounds.width - margin * 2);
    popup.style.width = `${Math.min(540, maximumWidth)}px`;
    const visibleRows = Math.min(50, Math.max(1, Number(appSettings.completionLimit) || 5));
    const preferredHeight = 28 + visibleRows * 26;
    popup.style.maxHeight = `${Math.max(48, Math.min(preferredHeight, panelBounds.height - margin * 2))}px`;
    const popupHeight = popup.offsetHeight;
    const cursorLeft = cursorBounds ? cursorBounds.left - panelBounds.left : margin;
    const cursorTop = cursorBounds ? cursorBounds.top - panelBounds.top
      : panelBounds.height - fallbackBottom;
    const cursorHeight = cursorBounds?.height || 18;
    const belowTop = cursorTop + cursorHeight + 4;
    const aboveTop = cursorTop - popupHeight - 4;
    const opensBelow = belowTop + popupHeight <= panelBounds.height - margin
      || panelBounds.height - belowTop > cursorTop - margin;
    const top = opensBelow ? belowTop : aboveTop;
    popup.style.left = `${Math.max(margin, Math.min(cursorLeft, panelBounds.width - popup.offsetWidth - margin))}px`;
    popup.style.top = `${Math.max(margin, Math.min(top, panelBounds.height - popupHeight - margin))}px`;
  };
  const renderCompletion = session => {
    const completion = session.completion;
    const candidates = completionCandidates(session);
    completion.candidates = candidates;
    completion.selected = Math.min(completion.selected, Math.max(0, candidates.length - 1));
    completion.popup.replaceChildren();
    const title = document.createElement("div");
    title.className = "terminal-completion-title";
    title.textContent = candidates.length ? "命令补全与常用模板建议 (方向右键分段追加，Tab保持原生补全)" :
      (completion.loaded ? "没有匹配的历史或常用命令" : "正在加载建议…");
    completion.popup.appendChild(title);
    let selectedRow = null;
    candidates.forEach((candidate, index) => {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "terminal-completion-item" + (candidate.isTemplate ? " template-item" : "");
      row.classList.toggle("selected", index === completion.selected);

      if (candidate.isTemplate) {
        const cmdSpan = document.createElement("span");
        cmdSpan.className = "completion-cmd-text";

        // 将 <placeholder> 渲染为专门的占位符高亮徽标
        const raw = candidate.value;
        const parts = raw.split(/(<[^>]+>)/g);
        for (const part of parts) {
          if (part.startsWith("<") && part.endsWith(">")) {
            const ph = document.createElement("span");
            ph.className = "completion-placeholder";
            ph.textContent = part;
            cmdSpan.appendChild(ph);
          } else if (part) {
            cmdSpan.appendChild(document.createTextNode(part));
          }
        }

        const badgeSpan = document.createElement("span");
        badgeSpan.className = "completion-badge";
        if (candidate.displayBadge) {
          badgeSpan.textContent = candidate.displayBadge;
        } else {
          badgeSpan.textContent = candidate.isNextSegment ? "分段后续" : (candidate.hasPlaceholders ? "分段补全" : "常用模板");
        }

        const descSpan = document.createElement("span");
        descSpan.className = "completion-desc";
        descSpan.textContent = candidate.desc || "";

        row.append(cmdSpan, badgeSpan, descSpan);
      } else {
        const textSpan = document.createElement("span");
        textSpan.className = "completion-cmd-text";
        textSpan.textContent = candidate.value;
        row.appendChild(textSpan);
      }

      row.addEventListener("click", () => {
        session.completion.selected = index;
        session.completion.explicitlySelected = true;
        acceptCompletion(session, index);
      });
      if (index === completion.selected) selectedRow = row;
      completion.popup.appendChild(row);
    });
    completion.popup.hidden = false;
    positionCompletion(session);
    requestAnimationFrame(() => selectedRow?.scrollIntoView({ block: "nearest" }));
  };
  const showCompletion = session => {
    if (session.connectionType !== "ssh" && session.connectionType !== "local") return;
    session.completion.visible = true;
    session.completion.selected = 0;
    session.completion.explicitlySelected = false;
    renderCompletion(session);
  };
  const sendSessionInput = (session, data) => {
    if (!session || !data) return;
    post("session.input", { sessionId: session.sessionId, data }).catch(reportError);
    if (broadcastInputActive) {
      sessions.forEach((targetSession, targetId) => {
        if (targetId !== session.sessionId && targetSession.state !== "closed" && targetSession.state !== "error" && targetSession.connectionType !== "rdp") {
          post("session.input", { sessionId: targetId, data }).catch(() => {});
        }
      });
    }
  };
  const acceptCompletion = (session, selected = session.completion.selected) => {
    const candidate = session.completion.candidates[selected];
    if (!candidate) return hideCompletion(session);

    // 1. 智能计算出的增量追加内容 (incrementalText)
    if (candidate.incrementalText && candidate.incrementalText.length > 0) {
      sendSessionInput(session, candidate.incrementalText);
      session.commandLine += candidate.incrementalText;
      session.commandCursor = session.commandLine.length;

      // 增量填充后，智能刷新候选项列表，实现无缝连贯的逐步引导式补全
      const nextCandidates = completionCandidates(session);
      const actionable = nextCandidates.filter(c => c.incrementalText && c.incrementalText.length > 0);
      if (actionable.length > 0) {
        session.completion.candidates = actionable;
        session.completion.selected = 0;
        session.completion.explicitlySelected = false;
        renderCompletion(session);
      } else {
        hideCompletion(session);
      }
      return;
    }

    // 2. 兼容分段后续 (isNextSegment / segmentPrefix / activeSegmentedTemplate)
    if (candidate.isNextSegment && candidate.segmentPrefix) {
      const prefix = candidate.segmentPrefix;
      sendSessionInput(session, prefix);
      session.commandLine += prefix;
      session.commandCursor = session.commandLine.length;
      if (session.activeSegmentedTemplate) {
        session.activeSegmentedTemplate.segmentIndex = candidate.segmentIndex;
        session.activeSegmentedTemplate.completedPrefix = session.commandLine;
        if (candidate.segmentIndex + 1 >= (candidate.template?.segments?.length || 0)) {
          session.activeSegmentedTemplate = null;
        }
      }
      hideCompletion(session);
      return;
    }

    // 3. 如果增量为空，说明当前候选已经完全输入完毕，无需追加，直接安全关闭浮层，绝不回退或退格擦除！
    hideCompletion(session);
  };

  const openTerminal = (sessionId, name, connectionType = "ssh", profileIndex = -1, options = {}) => {
    if (sessions.has(sessionId)) {
      pendingNewTerminalGroup = null;
      activateSession(sessionId);
      return sessions.get(sessionId);
    }
    if (typeof Terminal === "undefined") throw new Error("xterm.js 未加载");
    if (typeof FitAddon === "undefined") throw new Error("xterm fit 插件未加载");

    terminalEmpty.hidden = true;
    terminalTabs.hidden = false;

    const matchingSessions = Array.from(sessions.values()).filter(session =>
      profileIndex >= 0 ? session.profileIndex === profileIndex : session.name === name);
    // Number the display name from the lowest unused slot so closing a tab
    // releases its number: with only "wsl (2)" open a new session becomes
    // "wsl", and with "wsl" + "wsl (2)" the next one is "wsl (3)".
    const usedNumbers = new Set();
    let bareNameInUse = false;
    for (const session of matchingSessions) {
      const labelled = String(session.displayName || session.name || "");
      const match = labelled.match(/^(.*) \((\d+)\)$/);
      if (match && match[1] === name) {
        usedNumbers.add(Number(match[2]));
      } else if (labelled === name) {
        bareNameInUse = true;
      }
    }
    let displayNumber = bareNameInUse ? 2 : 1;
    while (usedNumbers.has(displayNumber)) displayNumber++;
    const displayName = displayNumber === 1 ? name : `${name} (${displayNumber})`;

    const tab = document.createElement("div");
    tab.className = "terminal-tab" + (broadcastInputActive ? " broadcast-active" : "");
    tab.dataset.state = "connecting";
    tab.dataset.sessionId = sessionId;
    if (connectionType !== "rdp") {
      tab.title = `${displayName} · ${sessionId}`;
    }
    const stateDot = document.createElement("span");
    stateDot.className = "terminal-tab-state";
    tab.dataset.connectionType = connectionType;
    tab.style.setProperty("--terminal-tab-type-color",
      connectionTypeColor(connectionType));
    const tabLabel = document.createElement("span");
    tabLabel.className = "terminal-tab-label";
    tabLabel.textContent = displayName;
    const closeButton = document.createElement("button");
    closeButton.className = "terminal-tab-close";
    closeButton.type = "button";
    closeButton.title = "关闭会话";
    closeButton.textContent = "×";
    tab.append(stateDot, tabLabel, closeButton);
    const requestedGroup = pendingNewTerminalGroup;
    pendingNewTerminalGroup = null;
    const requestedHost = requestedGroup?.classList?.contains("terminal-create-group")
      ? requestedGroup.parentElement : requestedGroup;
    const activeGroup = (activeSession?.tab?.parentElement?.classList?.contains("tab-group") || activeSession?.tab?.parentElement === terminalTabs)
      ? activeSession.tab.parentElement : null;
    const defaultGroup = activeGroup || splitViewLeft?.querySelector(".tab-group.left") || terminalTabs;
    let tabHost = terminalTabs;
    if (splitMode) {
      if (connectionType === "rdp") {
        tabHost = terminalTabs;
      } else if (requestedHost?.isConnected) {
        tabHost = requestedHost;
      } else {
        tabHost = defaultGroup;
      }
    }
    insertTerminalTab(tabHost, tab);

    const panel = document.createElement("div");
    panel.className = "terminal-panel";
    panel.dataset.nativeRdp = connectionType === "rdp" ? "true" : "false";
    const toolbar = document.createElement("div");
    toolbar.className = "terminal-toolbar";
    const title = document.createElement("div");
    title.className = "terminal-title";
    title.textContent = `${displayName} · ${sessionId}`;
    const disconnectButton = document.createElement("button");
    disconnectButton.className = "terminal-disconnect";
    disconnectButton.type = "button";
    disconnectButton.textContent = "断开";
    const favoritesButton = document.createElement("button");
    favoritesButton.className = "terminal-favorites";
    favoritesButton.type = "button";
    favoritesButton.title = "命令收藏";
    favoritesButton.textContent = "☆ 收藏";
    favoritesButton.addEventListener("click", openCommandFavorites);
    const output = document.createElement("div");
    output.className = "terminal-output";
    output.style.setProperty("--terminal-background", terminalTheme().background);
    toolbar.append(title, favoritesButton, disconnectButton);
    panel.append(toolbar, output);
    let rdpDisconnectNotice = null;
    let rdpDisconnectReason = null;
    let rdpConnectingNotice = null;
    let rdpReconnectButton = null;
    let rdpCloseNoticeButton = null;
    if (connectionType === "rdp") {
      rdpConnectingNotice = document.createElement("div");
      rdpConnectingNotice.className = "rdp-connecting-notice";
      rdpConnectingNotice.hidden = false;
      rdpConnectingNotice.setAttribute("role", "status");
      const connectingCard = document.createElement("div");
      connectingCard.className = "rdp-connecting-card";
      const connectingIcon = document.createElement("div");
      connectingIcon.className = "rdp-connecting-icon";
      connectingIcon.textContent = "·";
      const connectingContent = document.createElement("div");
      connectingContent.className = "rdp-connecting-content";
      const connectingHeading = document.createElement("strong");
      connectingHeading.textContent = "正在连接远程桌面";
      const connectingReason = document.createElement("p");
      connectingReason.textContent = "正在建立连接，请稍候…";
      connectingContent.append(connectingHeading, connectingReason);
      connectingCard.append(connectingIcon, connectingContent);
      rdpConnectingNotice.append(connectingCard);
      panel.append(rdpConnectingNotice);

      rdpDisconnectNotice = document.createElement("div");
      rdpDisconnectNotice.className = "rdp-disconnect-notice";
      rdpDisconnectNotice.hidden = true;
      rdpDisconnectNotice.setAttribute("role", "status");
      const card = document.createElement("div");
      card.className = "rdp-disconnect-card";
      const icon = document.createElement("div");
      icon.className = "rdp-disconnect-icon";
      icon.textContent = "!";
      const content = document.createElement("div");
      content.className = "rdp-disconnect-content";
      const heading = document.createElement("strong");
      heading.textContent = "远程桌面连接已中断";
      rdpDisconnectReason = document.createElement("p");
      const actions = document.createElement("div");
      actions.className = "rdp-disconnect-actions";
      rdpReconnectButton = document.createElement("button");
      rdpReconnectButton.type = "button";
      rdpReconnectButton.className = "primary";
      rdpReconnectButton.textContent = "重新连接";
      rdpCloseNoticeButton = document.createElement("button");
      rdpCloseNoticeButton.type = "button";
      rdpCloseNoticeButton.textContent = "关闭标签";
      actions.append(rdpReconnectButton, rdpCloseNoticeButton);
      content.append(heading, rdpDisconnectReason, actions);
      card.append(icon, content);
      rdpDisconnectNotice.append(card);
      panel.append(rdpDisconnectNotice);
    }
    terminalPanels.appendChild(panel);
    const completionPopup = document.createElement("div");
    completionPopup.className = "terminal-completion";
    completionPopup.hidden = true;
    completionPopup.addEventListener("wheel", event => event.stopPropagation(), { passive: true });
    panel.appendChild(completionPopup);

    const terminal = new Terminal({
      allowProposedApi: true,
      unicodeVersion: "11",
      cursorBlink: appSettings.cursorBlink,
      scrollback: appSettings.scrollback,
      fontFamily: terminalFontStack(appSettings.fontFamily),
      fontSize: appSettings.fontSize,
      cursorStyle: appSettings.cursorStyle,
      theme: terminalTheme()
    });
    const fit = new FitAddon.FitAddon();
    terminal.loadAddon(fit);
    terminal.open(output);
    if (terminal.parser?.registerOscHandler) {
      terminal.parser.registerOscHandler(52, data => {
        const separator = data.indexOf(";");
        if (separator < 0) return true;
        const payload = data.slice(separator + 1).replaceAll(/\s/g, "");
        if (!payload || payload === "?") return true;
        try {
          const binary = atob(payload);
          const bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
          const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
          post("clipboard.write", { text }).catch(() => {});
        } catch {
          // Ignore malformed or unsupported OSC 52 payloads.
        }
        return true;
      });
    }

    const session = {
      sessionId,
      name,
      displayName,
      connectionType,
      profileIndex,
      state: "connecting",
      tab,
      panel,
      output,
      terminal,
      fit,
      disconnectButton,
      rdpDisconnectNotice,
      rdpDisconnectReason,
      rdpConnectingNotice,
      rdpDisconnectNotified: false,
      rdpLastState: "",
      closing: false,
      decoder: new TextDecoder("utf-8"),
      inputLine: "",
      commandLine: "",
      commandCursor: 0,
      commandHistory: connectionType === "local" ? [...localCommandHistory] : [],
      metrics: { state: "waiting", receivedAt: 0, cpuTotal: -1, cpuIdle: -1, netRx: -1, netTx: -1 },
      completion: { visible: false, explicitlySelected: false,
        loaded: connectionType !== "ssh" && connectionType !== "local", selected: 0,
        candidates: [], popup: completionPopup, diagnostics: { state: "尚未发起" } },
      fullScreen: false,
      alternateScreen: false,
      remoteAlternateScreen: false,
      alternateScreenRestorePromise: null,
      suppressNextEnterData: false,
      suppressNextEnterTimer: 0,
      leaveTimer: 0,
      resizeTimer: 0,
      pendingExit: false,
      sawTopExit: false,
      reconnectAttempts: 0,
      lastColumns: 0,
      lastRows: 0,
      lastRawBytes: "",
      lastDecodedText: ""
    };

    rdpReconnectButton?.addEventListener("click", () => {
      if (session.profileIndex >= 0) {
        rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
      }
      reconnectSession(sessionId, { keepBackground: false }).catch(reportError);
    });
    rdpCloseNoticeButton?.addEventListener("click", () => {
      if (session.profileIndex >= 0) {
        rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
      }
      closeSession(sessionId);
    });

    const sendSize = () => {
      if (session.connectionType !== "ssh" && session.connectionType !== "local") return;
      if (session.state === "closed" || session.state === "error") return;
      if (terminal.cols === session.lastColumns && terminal.rows === session.lastRows) return;
      session.lastColumns = terminal.cols;
      session.lastRows = terminal.rows;
      post("session.resize", {
        sessionId,
        columns: terminal.cols,
        rows: terminal.rows
      }).catch(reportError);
    };
    const refit = () => {
      // In a split view the right session is visible without being the
      // primary activeSession.  Fit every visible panel, but never fit a
      // hidden panel (its zero-sized container would overwrite the PTY size).
      if (!session.panel.classList.contains("active")) return;
      try {
        fit.fit();
        sendSize();
        if (session.completion.visible) positionCompletion(session);
      } catch (error) {
        terminal.resize(80, 24);
        sendSize();
        if (session.completion.visible) positionCompletion(session);
        showTransientStatus(`终端自适应失败，已使用 80×24：${error.message}`);
      }
    };
    session.scheduleRefit = () => {
      if (session.resizeTimer) clearTimeout(session.resizeTimer);
      session.resizeTimer = setTimeout(() => {
        session.resizeTimer = 0;
        refit();
      }, 80);
    };

    const enterAlternateScreen = () => {
      if (session.alternateScreen) return;
      terminal.write("\x1b[?1049h");
      session.alternateScreen = true;
      session.fullScreen = true;
    };
    const syncAfterRemoteAlternateExit = () => {
      // xterm has already switched back to its normal buffer when this runs.
      // Deferring avoids competing with xterm's own synchronous scroll work.
      // still observe the former alternate viewport for 1047-based TUIs.
      setTimeout(() => {
        if (session.state === "closed" || session.state === "error") return;
        if (session.remoteAlternateScreen || session.alternateScreen) return;
        terminal.scrollToBottom();
        terminal.refresh(0, Math.max(0, terminal.rows - 1));
        if (activeSession === session) session.scheduleRefit();
      }, 0);
    };
    session.syncAfterRemoteAlternateExit = syncAfterRemoteAlternateExit;
    const nonInteractiveOpenCodeCommands = new Set([
      "web", "serve", "acp", "run", "agent", "plugin", "plug", "pr",
      "db", "debug", "uninstall", "auth", "export", "import", "upgrade",
      "completion", "mcp", "stats"
    ]);
    const isInteractiveAlternateCommand = command => {
      const normalized = String(command || "").trim().replace(/\s+/g, " ");
      if (/^(?:(?:sudo\s+)?top)(?:\s|$)/i.test(normalized)) return true;
      const match = normalized.match(/^(?:(?:sudo\s+)?opencode)(?:\s+(.*))?$/i);
      if (!match) return false;
      const args = (match[1] || "").trim();
      if (!args) return true;
      // Help/version output and headless/server subcommands stay on the
      // normal shell buffer. Only the TUI needs the local fallback screen.
      if (/(?:^|\s)(?:-h|--help|-v|--version)(?:\s|$)/i.test(args))
        return false;
      const firstToken = args.split(/\s+/, 1)[0].toLowerCase();
      return !nonInteractiveOpenCodeCommands.has(firstToken);
    };
    const resetAlternateScreenState = () => {
      session.alternateScreen = false;
      session.remoteAlternateScreen = false;
      session.fullScreen = false;
      session.pendingExit = false;
      session.sawTopExit = false;
    };
    const restoreLocalAlternateScreen = () => {
      if (session.leaveTimer) {
        clearTimeout(session.leaveTimer);
        session.leaveTimer = 0;
      }
      if (session.alternateScreenRestorePromise)
        return session.alternateScreenRestorePromise;
      const buffer = terminal.buffer;
      const alternateActive = session.alternateScreen
        || session.remoteAlternateScreen
        || (buffer?.alternate && buffer.active === buffer.alternate);
      if (!alternateActive) {
        resetAlternateScreenState();
        return Promise.resolve(false);
      }
      const restorePromise = new Promise(resolve => {
        // This is intentionally a local xterm write. The SSH transport may
        // already be gone, so the sequence must not be sent to the remote.
        terminal.write("\x1b[?12l\x1b[?25h\x1b[K\x1b[?1049l", () => {
          resetAlternateScreenState();
          terminal.scrollToBottom();
          terminal.refresh(0, Math.max(0, terminal.rows - 1));
          resolve(true);
        });
      });
      session.alternateScreenRestorePromise = restorePromise;
      restorePromise.then(() => {
        if (session.alternateScreenRestorePromise === restorePromise)
          session.alternateScreenRestorePromise = null;
      });
      return restorePromise;
    };
    session.restoreLocalAlternateScreen = restoreLocalAlternateScreen;
    const leaveAlternateScreen = () => {
      const buffer = terminal.buffer;
      if (!session.alternateScreen && !session.remoteAlternateScreen
          && (!buffer?.alternate || buffer.active !== buffer.alternate)) return;
      // The remote program's exit bytes have already been written to xterm.
      // Replaying the tail here duplicates prompt/control output; OpenCode's
      // keyboard/render protocol can then leave literals such as
      // "^[[51;1;42;48M" in the shell line. Only perform the local cleanup
      // that is still needed to leave the fallback alternate screen.
      restoreLocalAlternateScreen();
    };
    session.enterAlternateScreen = enterAlternateScreen;
    session.leaveAlternateScreen = leaveAlternateScreen;

    terminal.attachCustomKeyEventHandler(event => {
      if (event.type !== "keydown") return true;
      // OpenCode recognizes Shift+Enter as the CSI-u key sequence below.
      // xterm.js normally turns both Enter variants into the same CR, so
      // intercept it while an SSH session is in the OpenCode alternate
      // screen.  Do not change ordinary shell Enter behavior.
      if (event.key === "Enter" && event.shiftKey
          && !event.ctrlKey && !event.altKey && !event.metaKey
          && session.connectionType === "ssh"
          && (session.fullScreen || session.remoteAlternateScreen)) {
        session.suppressNextEnterData = true;
        if (session.suppressNextEnterTimer)
          clearTimeout(session.suppressNextEnterTimer);
        session.suppressNextEnterTimer = setTimeout(() => {
          session.suppressNextEnterData = false;
          session.suppressNextEnterTimer = 0;
        }, 250);
        event.preventDefault();
        event.stopPropagation();
        post("session.input", { sessionId, data: "\x1b[13;2u" })
          .catch(reportError);
        return false;
      }
      const completionKey = event.code === "Space" && event.ctrlKey
        && !event.shiftKey && !event.metaKey;
      if (completionKey) {
        const bufferInfo = getTerminalBufferLineInfo(session);
        session.commandLine = bufferInfo.commandLine;
        session.commandCursor = session.commandLine.length;
        if (bufferInfo.isAtEnd) showCompletion(session);
        return false;
      }
      if (event.ctrlKey && event.shiftKey && !event.altKey && !event.metaKey
          && event.key.toLowerCase() === "c") {
        copyTerminalSelection(session);
        return false;
      }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "c" && !event.shiftKey && !event.altKey) {
        if (!session.terminal?.hasSelection?.()) {
          session.commandLine = "";
          session.commandCursor = 0;
          session.inputLine = "";
          session.activeSegmentedTemplate = null;
          hideCompletion(session);
        }
      }
      if (event.ctrlKey && event.shiftKey && !event.altKey && !event.metaKey
          && event.key.toLowerCase() === "v") {
        pasteIntoTerminal(session);
        return false;
      }
      if (event.ctrlKey && !event.shiftKey && !event.altKey && !event.metaKey
          && event.key.toLowerCase() === "f") {
        openTerminalSearch(session);
        return false;
      }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "w" && !event.shiftKey && !event.altKey) {
        closeCurrentTab();
        return false;
      }
      if ((event.ctrlKey || event.metaKey) && (event.key === "," || event.code === "Comma") && !event.shiftKey && !event.altKey) {
        openSettingsDialog();
        return false;
      }
      if (event.ctrlKey && event.key === "Tab") {
        cycleTab(!event.shiftKey);
        return false;
      }
      if (event.altKey && !event.ctrlKey && !event.shiftKey && !event.metaKey
          && event.key >= "1" && event.key <= "9") {
        switchToTabByIndex(parseInt(event.key, 10));
        return false;
      }
      if (event.altKey && !event.ctrlKey && !event.metaKey
          && (event.key.toLowerCase() === "m" || event.key.toLowerCase() === "z")) {
        if (splitMode) {
          toggleSplitZoom();
          return false;
        }
      }
      // 1. Tab 键分流：100% 优先保证 Linux 原生 Shell 补全，禁止浏览器焦点跳转
      if (event.key === "Tab" && !event.ctrlKey && !event.altKey && !event.metaKey && !event.shiftKey) {
        if (session.completion.visible && session.completion.explicitlySelected && session.completion.candidates.length) {
          event.preventDefault();
          acceptCompletion(session);
          return false;
        }
        if (session.completion.visible) {
          hideCompletion(session);
        }
        event.preventDefault();
        sendSessionInput(session, "\t");
        return false;
      }

      // 2. Enter 键：仅在显式上下键选中模板时填充，否则执行当前已输入命令
      if (event.key === "Enter" && !event.shiftKey && !event.ctrlKey && !event.altKey) {
        if (session.completion.visible && session.completion.explicitlySelected && session.completion.candidates.length) {
          event.preventDefault();
          acceptCompletion(session);
          return false;
        }
        if (session.completion.visible) {
          hideCompletion(session);
        }
        return true;
      }

      // 3. 方向右键 (ArrowRight)：智能增量分段补全核心按键
      if (event.key === "ArrowRight") {
        const bufferInfo = getTerminalBufferLineInfo(session);
        session.commandLine = bufferInfo.commandLine;
        session.commandCursor = session.commandLine.length;

        if (!bufferInfo.isAtEnd) {
          hideCompletion(session);
          return true;
        }

        if (!session.completion.visible) {
          const candidates = completionCandidates(session);
          const actionable = candidates.filter(c => c.incrementalText && c.incrementalText.length > 0);
          if (actionable.length > 0) {
            session.completion.candidates = actionable;
            session.completion.selected = 0;
            acceptCompletion(session, 0);
            return false;
          }
          return true;
        }
        if (session.completion.candidates.length) {
          const topCandidate = session.completion.candidates[session.completion.selected || 0];
          if (topCandidate && topCandidate.incrementalText && topCandidate.incrementalText.length > 0) {
            acceptCompletion(session);
            return false;
          }
        }
        hideCompletion(session);
        return true;
      }

      // 4. 浮层处于显示状态时的导航与退出
      if (!session.completion.visible) return true;

      if (event.key === "Escape") {
        hideCompletion(session);
        return false;
      }
      if (event.key === "ArrowLeft") {
        hideCompletion(session);
        // Do not consume the key: xterm must receive it so the visible and
        // tracked cursor positions both move left.
        return true;
      }
      if (event.key === "ArrowUp" || event.key === "ArrowDown") {
        const count = session.completion.candidates.length;
        if (count) {
          session.completion.selected = Math.max(0, Math.min(count - 1,
            session.completion.selected + (event.key === "ArrowUp" ? -1 : 1)));
          session.completion.explicitlySelected = true;
          renderCompletion(session);
        }
        return false;
      }
      return true;
    });

    terminal.onData(data => {
      if (session.state === "closed" || session.state === "error") return;
      if (session.suppressNextEnterData && (data === "\r" || data === "\n")) {
        session.suppressNextEnterData = false;
        if (session.suppressNextEnterTimer) {
          clearTimeout(session.suppressNextEnterTimer);
          session.suppressNextEnterTimer = 0;
        }
        return;
      }
      let inputData = data;
      if (data === "\r" || data === "\n") {
        const submittedCommand = session.commandLine.trim();
        if (session.connectionType === "ssh" &&
            isInteractiveAlternateCommand(session.inputLine.trim()))
          enterAlternateScreen();
        session.inputLine = "";
        if (submittedCommand) {
          session.commandHistory = mergeCommandHistory(
            session.commandHistory, [submittedCommand]);
          if (session.connectionType === "local") {
            rememberLocalCommand(submittedCommand);
            if (submittedCommand.toLowerCase() === "clear")
              // cmd.exe has already received the typed characters through
              // ConPTY. Erase them before replacing the command with cls.
              inputData = "\x7f".repeat(submittedCommand.length) + "cls\r";
          }
        }
        session.commandLine = "";
        session.commandCursor = 0;
        session.activeSegmentedTemplate = null;
        hideCompletion(session);
      } else if (data === "\x7f" || data === "\b") {
        session.inputLine = session.inputLine.slice(0, -1);
        if (session.commandCursor > 0) {
          session.commandLine = session.commandLine.slice(0, session.commandCursor - 1)
            + session.commandLine.slice(session.commandCursor);
          session.commandCursor -= 1;
        } else {
          const bufferInfo = getTerminalBufferLineInfo(session);
          if (bufferInfo.commandLine) {
            session.commandLine = bufferInfo.commandLine.slice(0, -1);
            session.commandCursor = session.commandLine.length;
          }
        }
        if (session.completion.visible) renderCompletion(session);
        if (!session.commandLine) hideCompletion(session);
      } else if (data === "\x03" || data === "\x15" || data === "\x0c" || data === "\x1a") {
        session.commandLine = "";
        session.commandCursor = 0;
        session.inputLine = "";
        session.activeSegmentedTemplate = null;
        hideCompletion(session);
      } else if (data === "\x1b[A" || data === "\x1bOA" || data === "\x1b[B" || data === "\x1bOB") {
        session.commandLine = "";
        session.commandCursor = 0;
        hideCompletion(session);
      } else if (data === "\x1b[D" || data === "\x1bOD" || data === "\x1b[1D") {
        session.commandCursor = Math.max(0, session.commandCursor - 1);
        hideCompletion(session);
      } else if (data === "\x1b[C" || data === "\x1bOC" || data === "\x1b[1C") {
        session.commandCursor = Math.min(session.commandLine.length, session.commandCursor + 1);
        hideCompletion(session);
      } else if (data.length === 1 && data >= " " && data <= "~") {
        if (!session.commandLine && !session.commandCursor) {
          const bufferInfo = getTerminalBufferLineInfo(session);
          if (bufferInfo.commandLine) {
            session.commandLine = bufferInfo.commandLine;
            session.commandCursor = session.commandLine.length;
          }
        }
        session.inputLine += data;
        session.commandLine = session.commandLine.slice(0, session.commandCursor)
          + data + session.commandLine.slice(session.commandCursor);
        session.commandCursor += data.length;
        if (session.completion.visible) {
          renderCompletion(session);
        } else if (session.commandLine.trim().length >= 2 && session.commandCursor === session.commandLine.length) {
          const cands = completionCandidates(session);
          if (cands.length > 0) {
            showCompletion(session);
          }
        }
      }
      const leavingFullScreen = session.connectionType === "ssh" &&
        session.fullScreen && (data === "q" || data === "\x03");
      if (leavingFullScreen) {
        session.fullScreen = false;
        session.pendingExit = true;
        session.sawTopExit = false;
      }
      post("session.input", { sessionId, data: inputData }).catch(reportError);
      if (broadcastInputActive) {
        sessions.forEach((targetSession, targetId) => {
          if (targetId !== sessionId && targetSession.state !== "closed" && targetSession.state !== "error" && targetSession.connectionType !== "rdp") {
            post("session.input", { sessionId: targetId, data: inputData }).catch(() => {});
          }
        });
      }
      if (leavingFullScreen) {
        if (session.leaveTimer) clearTimeout(session.leaveTimer);
        session.leaveTimer = setTimeout(leaveAlternateScreen, 600);
      }
    });
    terminal.onResize(sendSize);
    terminal.onSelectionChange(() => {
      if (!appSettings.copyOnSelect) return;
      const selected = terminal.getSelection();
      if (selected) post("clipboard.write", { text: selected })
        .then(() => { showToast("内容已复制"); })
        .catch(() => {});
    });
    output.addEventListener("wheel", event => {
      if (!event.ctrlKey || !event.deltaY) return;
      event.preventDefault();
      const current = Number(terminal.options.fontSize) || appSettings.fontSize;
      const next = Math.min(32, Math.max(8, current + (event.deltaY < 0 ? 1 : -1)));
      if (next === current) return;
      terminal.options.fontSize = next;
      session.scheduleRefit();
      showTransientStatus(`当前终端字号：${next}px（仅当前会话）`);
    }, { passive: false });
    output.addEventListener("contextmenu", event => showTerminalOutputContextMenu(event, session));
    output.addEventListener("pointerdown", () => focusSession(sessionId), true);
    output.addEventListener("focusin", () => focusSession(sessionId), true);
    output.addEventListener("paste", event => {
      // Clipboard input is handled explicitly by pasteIntoTerminal(), which
      // prevents WebView2 from dispatching the same paste twice after a
      // clipboard-permission prompt.
      event.preventDefault();
      event.stopImmediatePropagation();
    }, true);

    tab.addEventListener("click", () => {
      hideRdpTabHover();
      activateSession(sessionId);
    });
    tab.addEventListener("contextmenu", event => {
      hideRdpTabHover();
      showTerminalContextMenu(event, sessionId);
    });
    tab.addEventListener("mouseenter", () => {
      if (connectionType === "rdp") showRdpTabHover(tab, sessionId);
    });
    tab.addEventListener("mouseleave", () => {
      if (connectionType === "rdp") hideRdpTabHover();
    });
    tab.addEventListener("dragstart", event => {
      hideRdpTabHover();
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("application/x-masterterm-terminal-tab", sessionId);
      tab.classList.add("dragging");
    });
    tab.addEventListener("dragend", () => {
      document.querySelectorAll(".terminal-tab.drag-over, .terminal-tab.dragging")
        .forEach(item => item.classList.remove("drag-over", "dragging"));
    });
    tab.addEventListener("dragover", event => {
      if (!Array.from(event.dataTransfer.types)
        .includes("application/x-masterterm-terminal-tab")) return;
      event.preventDefault();
      tab.classList.add("drag-over");
    });
    tab.addEventListener("dragleave", () => tab.classList.remove("drag-over"));
    tab.addEventListener("drop", event => {
      const draggedId = event.dataTransfer.getData("application/x-masterterm-terminal-tab");
      const dragged = sessions.get(draggedId)?.tab;
      if (!dragged || dragged === tab) return;
      const tabGroup = tab.parentElement;
      if (tabGroup === terminalTabs && dragged.parentElement !== terminalTabs && splitMode) {
        event.preventDefault();
        event.stopPropagation();
        tab.classList.remove("drag-over");
        const before = event.clientX < tab.getBoundingClientRect().left + tab.offsetWidth / 2;
        moveTabToTopLevel(draggedId, before ? tab : tab.nextElementSibling);
        return;
      }
      // Let the destination group's drop handler process a cross-view move.
      // It owns the session placement and keeps splitRightIds in sync.
      if (tabGroup?.classList.contains("tab-group")
          && dragged.parentElement !== tabGroup)
        return;
      event.preventDefault();
      event.stopPropagation();
      const before = event.clientX < tab.getBoundingClientRect().left + tab.offsetWidth / 2;
      if (tabGroup?.classList.contains("tab-group")) {
        tabGroup.insertBefore(dragged, before ? tab : tab.nextElementSibling);
        refreshSplitRightIds();
      } else {
        insertTerminalTab(terminalTabs, dragged,
          before ? tab : tab.nextElementSibling);
      }
      tab.classList.remove("drag-over");
    });
    closeButton.addEventListener("click", event => {
      event.stopPropagation();
      closeSession(sessionId);
    });
    disconnectButton.addEventListener("click", () => {
      if (connectionType === "rdp") {
        closeSession(sessionId);
        return;
      }
      disconnectButton.disabled = true;
      post("session.disconnect", { sessionId }).catch(error => {
        disconnectButton.disabled = false;
        reportError(error);
      });
    });

    sessions.set(sessionId, session);
    if (connectionType === "local") {
      loadLocalCommandHistory().then(commands => {
        if (!sessions.has(sessionId)) return;
        session.commandHistory = mergeCommandHistory(commands, session.commandHistory);
        session.completion.loaded = true;
        if (session.completion.visible) renderCompletion(session);
      });
    }
    saveSessionRestore();
    if (options.keepBackground) {
      if (splitMode) {
        terminalTabs.classList.toggle("has-rdp", hasRdpSession());
        renderSplit();
      }
    } else {
      if (splitMode && tabHost.classList.contains("right")) {
        refreshSplitRightIds();
        splitRightActive = splitRightIds.indexOf(sessionId);
        renderSplit();
        focusSession(sessionId);
        setTimeout(() => session.terminal.focus(), 0);
      } else {
        activateSession(sessionId);
      }
    }
    setTimeout(refit, 0);
    setTimeout(refit, 250);
    return session;
  };

  let layoutResizeTimer = 0;
  const scheduleViewportAdapt = () => {
    if (layoutResizeTimer) clearTimeout(layoutResizeTimer);
    layoutResizeTimer = setTimeout(() => {
      layoutResizeTimer = 0;
      adaptCardLayoutToViewport();
      notifyRdpLayout(true);
    }, 80);
  };
  window.addEventListener("masterterm-host-resize", scheduleViewportAdapt);
  window.addEventListener("resize", () => {
    scheduleViewportAdapt();
    renderServerMetrics();
  });

  const appendOutput = (session, text) => session.terminal.write(text);
  const handleEvent = message => {
    const payload = message.payload || {};
    if (message.event === "app.close-request") {
      (async () => {
        const result = await showActionDialog({
          title: "关闭 MasterTerm",
          message: "关闭主窗口时，SSH、串口和本地终端会话将如何处理？\n所选行为将成为默认，可在终端设置中修改。",
          value: null,
          buttons: [
            { action: "tray", label: "最小化到托盘", primary: true },
            { action: "exit", label: "直接退出" }
          ]
        });
        const decision = result.action === "exit" ? "exit" : "tray";
        post("app.closeDecision", { decision }).catch(reportError);
      })();
      return;
    }
    if (message.event === "app.shortcut" && payload.name === "close-tab") {
      closeCurrentTab();
      return;
    }
    if (message.event === "app.shortcut" && payload.name === "open-settings") {
      openSettingsDialog();
      return;
    }
    if (message.event === "app.shortcut" && payload.name === "history-completion") {
      const session = sessions.get(focusedSessionId) || activeSession;
      if (session && (session.connectionType === "ssh"
          || session.connectionType === "local")) showCompletion(session);
      return;
    }
    if (message.event === "app.shortcut" && payload.name === "rdp-toggle-fullscreen") {
      const session = sessions.get(focusedSessionId) || activeSession;
      if (session?.connectionType === "rdp")
        requestRdpFullscreen(!rdpFullscreen).catch(reportError);
      return;
    }
    if (message.event === "app.nativeThemeSelected") {
      const theme = String(payload.theme || "");
      const preset = String(payload.preset || "");
      if (theme && themes[theme]) {
        applyTheme(theme);
      }
      if (preset === "custom") {
        appSettings.terminalThemePreset = "custom";
        appSettings.terminalBackground = "";
        appSettings.terminalForeground = "";
        applyTerminalSettings();
        saveSettings();
        updateThemeMenuChecks();
      } else if (preset && terminalThemePresets[preset]) {
        appSettings.terminalThemePreset = preset;
        appSettings.terminalBackground = terminalThemePresets[preset].background;
        appSettings.terminalForeground = terminalThemePresets[preset].foreground;
        applyTerminalSettings();
        saveSettings();
        updateThemeMenuChecks();
      }
      closeThemeMenu();
      return;
    }
    if (message.event === "tunnel.state") {
      refreshTunnelsPanel?.();
      return;
    }
    if (message.event === "app.nativeToolsAction") {
      const action = String(payload.action || "");
      if (action === "broadcast") {
        toggleBroadcastInput();
      } else if (action === "tunnel") {
        activateFunctionPanel("tunnels");
      } else if (action === "batch-cmd") {
        renderBatchCommandSessions();
        document.querySelector("#batch-command-dialog")?.showModal();
      } else if (action === "cloud-sync") {
        activateFunctionPanel("cloud");
      } else if (action === "diagnostics") {
        document.querySelector("#terminal-diagnostics-dialog")?.showModal();
      }
      return;
    }
    if (message.event === "app.nativeHelpAction") {
      const action = String(payload.action || "");
      if (action === "shortcuts") {
        openShortcutsDialog();
      } else if (action === "check-updates") {
        openUpdateDialog();
      } else if (action === "about") {
        openAboutDialog();
      }
      return;
    }
    if (message.event === "app.nativeServerContextAction") {
      const profileIndex = Number(payload.index);
      const action = String(payload.action || "");
      const profile = profilesCache.find(item => item.index === profileIndex);
      if (!profile || !action) return;
      // The native popup has already closed. Reuse the existing action
      // listener so batch dialogs, reorder validation and refresh behavior
      // stay identical to the normal HTML menu.
      contextMenuProfile = profile;
      serverContextMenu.querySelector(
        `[data-action="${action}"]`)?.click();
      return;
    }
    if (message.event === "app.nativeOpenConnection") {
      const profileIndex = Number(payload.index);
      const targetGroup = nativeQuickOpenGroup;
      nativeQuickOpenGroup = null;
      pendingNewTerminalGroup = targetGroup?.isConnected && splitMode
        ? targetGroup : null;
      if (profileIndex < 0) {
        connectLocalTerminal(null, targetGroup);
      } else {
        const profile = profilesCache.find(item => item.index === profileIndex);
        if (profile) openExistingConnection(profile);
      }
      return;
    }
    if (message.event === "update.info") {
      showUpdateDialog(payload);
      return;
    }
    if (message.event === "update.downloadProgress") {
      if (updateProgressHandler) updateProgressHandler(payload);
      return;
    }
    if (message.event === "rdp.contextAction") {
      const sessionId = String(payload.sessionId || message.sessionId || "");
      const action = String(payload.action || message.action || "");
      if (!sessionId || !sessions.has(sessionId)) return;
      if (action === "reconnect") {
        reconnectSession(sessionId);
      } else if (action === "close") {
        closeSession(sessionId);
      } else if (action === "close-others") {
        Promise.all(Array.from(sessions.keys())
          .filter(id => id !== sessionId)
          .map(id => closeSession(id))).catch(reportError);
      } else if (action === "close-all") {
        Promise.all(Array.from(sessions.keys())
          .map(id => closeSession(id))).catch(reportError);
      } else if (action === "move-left") {
        moveTerminalTab(sessionId, -1);
      } else if (action === "move-right") {
        moveTerminalTab(sessionId, 1);
      } else if (action === "fullscreen") {
        requestRdpFullscreen(!rdpFullscreen, sessionId).catch(reportError);
      }
      return;
    }
    if (message.event === "app.nativeTabsContextAction") {
      const action = String(payload.action || message.action || "");
      if (action && handleTerminalTabsAction) {
        handleTerminalTabsAction(action, terminalTabsContextGroup);
      }
      return;
    }
    if (message.event === "rdp.fullscreen") {
      rdpFullscreen = Boolean(payload.enabled);
      if (rdpFullscreen && sidebarAutoHide)
        hideSidebarFlyout(true);
      if (rdpFullscreen)
        rdpFullscreenSessionId = String(message.sessionId || "");
      if (!rdpFullscreen)
        rdpFullscreenNativeBar = false;
      // Start fullscreen with a transient toolbar. The native overlay reveals
      // it when the pointer reaches the top edge; pinning is explicit.
      rdpFullscreenBarPinned = false;
      rdpFullscreenBarVisible = false;
      rdpFullscreenMenu = "";
      if (rdpFullscreenBarTimer) {
        clearTimeout(rdpFullscreenBarTimer);
        rdpFullscreenBarTimer = 0;
      }
      syncRdpFullscreenUi();
      // setRdpFullscreen() already performed the single native display
      // transition. The frontend state change only needs to refresh bounds;
      // asking mstscax to renegotiate again causes visible multi-flashes.
      notifyRdpLayout(false);
      if (!rdpFullscreen)
        rdpFullscreenSessionId = "";
      return;
    }
    if (message.event === "rdp.fullscreenBar") {
      if (!rdpFullscreenNativeBar && payload.visible !== false)
        showRdpFullscreenBar();
      return;
    }
    if (message.event === "rdp.fullscreenNativeBar") {
      // A native bar may auto-hide while fullscreen remains active. Keep the
      // HTML fallback suppressed until the native overlay is destroyed on
      // fullscreen exit.
      if (payload.visible !== false)
        rdpFullscreenNativeBar = true;
      syncRdpFullscreenUi();
      return;
    }
    if (message.event === "rdp.quality") {
      const sessionId = String(message.sessionId || payload.sessionId || "");
      if (sessionId) {
        rdpQualityBySession.set(sessionId, { ...payload });
        if (focusedSessionId === sessionId)
          renderServerMetrics();
      }
      if (!rdpQualityPanel?.hidden
          && sessionId === (rdpFullscreenSessionId || activeRdpSessionId()))
        renderRdpQualityPanel(sessionId);
      return;
    }
    if (message.event === "rdp.state") {
      const state = String(payload.state || "");
      const session = sessions.get(message.sessionId);
      // The native close request is synchronous but its lifecycle event can
      // reach WebView before the request response disposes the tab. Ignore
      // both that expected close and any late event for an already removed tab.
      if (!session || session.closing) {
        clearRdpConnectingStatus(message.sessionId);
        clearRdpConnectedStatus(message.sessionId);
        return;
      }
      if (session.rdpLastState === state) {
        // Duplicate native events are normally ignored, but still reconcile
        // the shared header in case the tab was activated after the first
        // event or the previous status update was superseded.
        syncRdpStatus(session);
        return;
      }
      session.rdpLastState = state;
      // OnWarning is informational and must not turn a healthy RDP tab into
      // a disconnected tab. The native layer uses a separate prefix so the
      // failure cleanup path only runs for terminal errors.
      if (state.startsWith("warning:")) {
        if (activeSession === session)
          showTransientStatus(
            state.slice(8) || "远程桌面控件报告警告");
        return;
      }
      const tabState = state === "connected" ? "connected"
        : state === "connecting" ? "connecting"
        : state.startsWith("error:") ? "error" : "closed";
      updateSessionState(session, tabState);
      if (state === "connected" || state === "connecting") {
        hideRdpDisconnectNotice(session, state === "connected");
        if (state === "connected") {
          if (session.profileIndex >= 0) {
            rdpAutoReconnectAttemptsByProfile.delete(session.profileIndex);
          }
          hideRdpConnectingNotice(session);
          session.rdpDisconnectNotified = false;
          if (activeSession === session) {
            notifyRdpLayout(false);
          }
        } else {
          showRdpConnectingNotice(session);
          if (activeSession === session) {
            notifyRdpLayout(false, true);
          }
        }
      } else {
        hideRdpConnectingNotice(session);
        clearRdpConnectingStatus(session.sessionId);
        const rawState = String(state || "");
        let reason = "";
        if (rawState.startsWith("conflict:")) {
          reason = rawState.slice(9) || "远程桌面已断开：检测到另一处设备已接入该会话，已停止重连以避免互相踢下线。";
        } else if (rawState.startsWith("logoff:")) {
          reason = rawState.slice(7) || "远程桌面会话已注销。";
        } else if (rawState.startsWith("disconnected:")) {
          reason = rawState.slice(13) || "远程桌面连接已正常断开。";
        } else if (rawState.startsWith("error:")) {
          reason = rawState.slice(6) || "远程桌面连接发生错误。";
        } else {
          reason = "连接已被远端、服务器或另一登录会话中断。";
        }
        showRdpDisconnectNotice(session, reason);
        // Only the active failed tab needs an immediate native-layout reset.
        // A background failure is already hidden by the native state handler.
        if (activeSession === session)
          notifyRdpLayout(false, true);
        if (!session.rdpDisconnectNotified) {
          session.rdpDisconnectNotified = true;
          notify("RDP 连接已中断", `${session.displayName}：${reason}`);
        }
      }
      if (focusedSessionId === session.sessionId)
        renderServerMetrics();
      // Background and superseded RDP sessions must not overwrite the status
      // of the tab the user is currently viewing.
      if (activeSession === session) {
        if (state === "connected") {
          clearRdpConnectingStatus(session.sessionId);
          showRdpConnectedStatus(session.sessionId);
        } else if (state === "connecting") {
          rdpConnectingStatusSessionId = session.sessionId;
          setHeaderStatus("正在连接远程桌面…");
        }
        else if (state.startsWith("error:"))
          showTransientStatus(state.slice(6));
        else if (state === "disconnected")
          showTransientStatus("远程桌面已断开");
      }
      return;
    }
    if (message.event === "tunnel.state") {
      if (tunnelDialog.open) refreshTunnelList();
      return;
    }
    if (message.event === "sftp.transfer") {
      if (!activeTransferId)
        activeTransferId = payload.transferId || null;
      if (payload.transferId !== activeTransferId) return;
      if (payload.transferId && !transferStartedAt.has(payload.transferId))
        transferStartedAt.set(payload.transferId, Date.now());
      if (payload.state === "progress") {
        const done = Number(payload.done || 0);
        const total = Number(payload.total || 0);
        const fileDone = payload.fileDone >= 0 ? Number(payload.fileDone) : -1;
        const fileTotal = Number(payload.fileTotal || 0);
        const filePercent = fileTotal > 0
          ? Math.floor(Math.max(0, fileDone) * 100 / fileTotal) : 0;
        const overallPercent = total > 0 ? Math.floor(done * 100 / total) : 0;
        const totalFiles = Number(activeTransferBatch?.total
          || payload.totalFiles || 0);
        const completedFiles = Number(activeTransferBatch?.index
          || payload.completedFiles || 0);
        const fileProgress = totalFiles > 0
          ? `文件 ${completedFiles}/${totalFiles}`
          : "";
        const transferName = compactTransferName(payload.name);
        const operationLabel = activeTransferOperation === "download"
          ? "正在下载" : "正在上传";
        sftpTransferStatusText.textContent =
          `${operationLabel}${fileProgress ? `（${fileProgress}）` : ""} ${transferName}`;
        sftpTransferStatusText.title = String(payload.name || "");
        if (sftpProgressBar) sftpProgressBar.style.width = `${filePercent}%`;
        if (sftpProgressPercent) {
          sftpProgressPercent.textContent =
            `${filePercent}%${fileProgress ? ` · ${fileProgress}` : ""}`;
          sftpProgressPercent.title = `整体 ${overallPercent}%（${formatSize(done)} / ${formatSize(total)}）`;
        }
        if (sftpProgressSpeed)
          sftpProgressSpeed.textContent = formatTransferSpeed(done, payload.transferId);
        if (sftpProgressEta)
          sftpProgressEta.textContent = formatTransferEta(done, total, payload.transferId);
        if (activeTransferTask) {
          activeTransferTask.done = done;
          activeTransferTask.total = total;
          activeTransferTask.size = total;
          activeTransferTask.fileDone = fileTotal > 0 ? fileDone : done;
          activeTransferTask.fileTotal = fileTotal > 0 ? fileTotal : total;
          activeTransferTask.speed = transferBytesPerSecond(done, payload.transferId);
          if (activeTransferTask.directory && fileTotal > 0
              && payload.name) {
            activeTransferTask.resumeDirectory = true;
            if (!activeTransferTask.checkpoint
                || typeof activeTransferTask.checkpoint !== "object")
              activeTransferTask.checkpoint = { version: 1, files: {} };
            if (!activeTransferTask.checkpoint.files
                || typeof activeTransferTask.checkpoint.files !== "object")
              activeTransferTask.checkpoint.files = {};
            const checkpointName = String(payload.name);
            activeTransferTask.checkpoint.files[checkpointName] = {
              done: Math.max(0, fileDone), total: Math.max(0, fileTotal),
              state: fileTotal > 0 && fileDone >= fileTotal
                ? "complete" : "partial"
            };
            activeTransferTask.checkpoint.updatedAt = Date.now();
          }
        }
        if (Date.now() - transferLastRenderAt >= 200)
          renderTransferPanel();
      } else if (payload.state === "paused") {
        // The worker stays suspended mid-transfer.  Put the task back into
        // the queue (marked paused) so the queue can continue with the next
        // task and the paused task stays reorderable.
        const pausedTask = activeTransferTask;
        if (pausedTask && activeTransferId) {
          // Stream uploads carry their data over the worker stdin, so a
          // suspended worker cannot be resumed at the same offset.  Release
          // the worker; "继续" restarts the file from the beginning.
          const streamUpload = pausedTask.type === "stream-upload";
          if (streamUpload)
            post("sftp.cancel", { transferId: activeTransferId }).catch(() => {});
          transferQueue.unshift({
            ...pausedTask,
            paused: true,
            ...(streamUpload ? {} : { resumeTransferId: activeTransferId })
          });
          activeTransferTask = null;
        }
        activeTransferPaused = false;
        activeTransferId = null;
        activeTransferOperation = "";
        activeTransferBatch = null;
        updateTransferControls();
        renderTransferPanel();
        pumpTransferQueue();
        sftpTransferStatusText.textContent = "传输已暂停";
      } else if (payload.state === "active") {
        activeTransferPaused = false;
        sftpTransferStatusText.textContent =
          activeTransferTask ? "传输已继续" : "传输中";
        updateTransferControls();
        renderTransferPanel();
      } else if (payload.state === "completed") {
        activeTransferPaused = false;
        const completedDirection =
          activeTransferOperation === "download" ? "下载" : "上传";
        if (activeTransferOperation === "upload")
          refreshRemoteAfterTransfers = true;
        else if (activeTransferOperation === "download")
          refreshLocalAfterTransfers = true;
        activeTransferId = null;
        activeTransferOperation = "";
        activeTransferBatch = null;
        const completedTask = activeTransferTask;
        const completedBytes = Number(payload.total || transferTaskSize(completedTask));
        const completedSpeed = transferBytesPerSecond(completedBytes, payload.transferId);
        activeTransferTask = null;
        transferRetryCount = 0;
        recordTransferResult("success", completedTask || { name: payload.name }, "",
          { size: completedBytes, speed: completedSpeed });
        transferStartedAt.delete(payload.transferId);
        updateTransferControls();
        sftpTransferStatusText.textContent = transferQueue.length
          ? `传输完成，继续处理队列（剩余 ${transferQueue.length} 项）`
          : "传输完成";
        if (!transferQueue.length)
          notify(
            "传输完成",
            `${completedDirection} ${completedTask?.name || payload.name || "文件"}`
          );
        pumpTransferQueue();
      } else if (payload.state === "cancelled") {
        activeTransferPaused = false;
        activeTransferId = null;
        activeTransferOperation = "";
        activeTransferBatch = null;
        const cancelledTask = activeTransferTask;
        activeTransferTask = null;
        transferRetryCount = 0;
        recordTransferResult("cancelled", cancelledTask || { name: payload.name }, "用户取消");
        transferStartedAt.delete(payload.transferId);
        updateTransferControls();
        sftpTransferStatusText.textContent = "传输已取消";
        pumpTransferQueue();
      } else if (payload.state === "error") {
        activeTransferPaused = false;
        notify(
          "传输失败",
          `${payload.name || "文件"}${payload.message ? `：${payload.message}` : ""}`
        );
        activeTransferId = null;
        activeTransferOperation = "";
        activeTransferBatch = null;
        transferStartedAt.delete(payload.transferId);
        updateTransferControls();
        retryTransferOrFinish(new Error(payload.message || "SFTP 传输失败"));
      }
      return;
    }
    if (message.event === "sftp.remote-edit") {
      const name = payload.name || "远程文件";
      if (payload.state === "conflict_prompt") {
        showActionDialog({
          title: "远程文件修改覆盖确认",
          message: `检测到本地已保存对远程文件 "${name}" 的修改。\n\n当前已启用防覆盖保护，是否确认将修改上传并覆盖远程服务器上的对应文件？`,
          value: null,
          buttons: [
            { action: "cancel", label: "取消同步" },
            { action: "overwrite", label: "确认覆盖上传", primary: true }
          ]
        }).then(choice => {
          if (choice.action === "overwrite") {
            post("sftp.retryRemoteEdit", {
              index: Number(payload.index),
              remotePath: String(payload.remotePath || ""),
              conflict: "overwrite"
            }).catch(reportError);
          } else {
            showTransientStatus(`已取消 "${name}" 的覆盖上传。`);
          }
        });
        return;
      }
      const task = {
        type: "edit-sync", name, profileIndex: Number(payload.index),
        remotePath: String(payload.remotePath || ""), size: 0
      };
      const key = `${task.profileIndex}:${task.remotePath}`;
      if (payload.state === "syncing") {
        remoteEditTransfers.set(key, task);
        renderTransferPanel();
      } else if (payload.state === "synced") {
        remoteEditTransfers.delete(key);
        recordTransferResult("success", task, "自动同步完成");
      } else if (payload.state === "error" && remoteEditTransfers.has(key)) {
        remoteEditTransfers.delete(key);
        recordTransferResult("error", task, payload.message || "自动同步远程文件失败");
      }
      if (sftpProfile && Number(payload.index) === Number(sftpProfile.index)) {
        if (payload.state === "downloading") {
          const done = Number(payload.done || 0);
          const total = Number(payload.total || 0);
          const percent = total > 0 ? Math.floor(done * 100 / total) : 0;
          sftpStatusText.textContent =
            `正在下载编辑副本：${name}（${percent}%）`;
          sftpStatusText.title = payload.remotePath || "";
        } else if (payload.state === "opened")
          sftpStatusText.textContent = `已打开：${name}（保存后自动同步）`;
        else if (payload.state === "syncing")
          sftpStatusText.textContent = `正在同步修改：${name}`;
        else if (payload.state === "synced")
          sftpStatusText.textContent = `已自动同步：${name}`;
        else if (payload.state === "error")
          sftpStatusText.textContent = payload.message || `打开或同步失败：${name}`;
        sftpStatusText.title = payload.remotePath || "";
      }
      return;
    }
    if (message.event === "auth.required") {
      requestText("SSH 身份验证",
        `请输入 ${payload.name || payload.address} 的 SSH 密码`, "").then(password => {
        if (password !== null)
          post("session.connect", { index: payload.index, password }).catch(reportError);
      });
      return;
    }

    if (message.event === "auth.keyPathMissing") {
      const missingPath = String(payload.keyPath || "");
      showActionDialog({
        title: "私钥文件不存在",
        message:
          `连接 ${payload.name || payload.address} 需要私钥，但文件不存在：\n${missingPath}\n\n`
          + "该路径可能来自另一台电脑（云同步）。请选择本机的私钥文件，或取消后编辑连接改用密码。",
        value: null,
        buttons: [
          { action: "cancel", label: "取消" },
          { action: "pick", label: "选择本机私钥", primary: true }
        ]
      }).then(choice => {
        if (choice.action !== "pick") return;
        post("dialog.openFile", { initial: missingPath })
          .then(result => {
            if (result.cancelled) return;
            const path = String(result.path || "");
            if (!path) {
              showTransientStatus("无法获取所选文件路径，请使用“编辑连接”手动填写私钥路径");
              return;
            }
            post("session.connect", {
              index: Number(payload.index), keyPath: path
            }).catch(reportError);
          })
          .catch(reportError);
      });
      return;
    }

    if (message.event === "hostkey.mismatch") {
      const host = String(payload.address || message.sessionId || "未知主机");
      showActionDialog({
        title: "主机密钥已更改",
        message:
          `主机 ${host} 的 SSH 密钥与之前保存的记录不一致。\n\n`
          + `旧指纹：${payload.oldFingerprint || "（未知）"}\n`
          + `新指纹：${payload.newFingerprint || "（未知）"}\n\n`
          + "服务器重装或服务迁移会导致密钥变化；若您未预期此变化，"
          + "请取消连接并核对服务器信息。",
        monospace: true,
        buttons: [
          { action: "reject", label: "取消连接" },
          { action: "accept", label: "信任新指纹并继续", primary: true }
        ]
      }).then(choice => {
        post("session.hostKeyConfirm", {
          sessionId: message.sessionId,
          accept: choice.action === "accept"
        }).catch(reportError);
      });
      return;
    }

    if (message.event === "session.metrics") {
      const session = sessions.get(message.sessionId);
      if (!session) return;
      if (payload.state !== "available") {
        session.metrics = { ...session.metrics, state: "unavailable" };
        if (focusedSessionId === session.sessionId) renderServerMetrics();
        return;
      }
      const number = value => Number.isFinite(Number(value)) ? Number(value) : -1;
      const previous = session.metrics || {};
      const now = Date.now();
      const cpuTotal = number(payload.cpuTotal);
      const cpuIdle = number(payload.cpuIdle);
      const netRx = number(payload.netRx);
      const netTx = number(payload.netTx);
      let cpuPercent = NaN;
      if (cpuTotal > previous.cpuTotal && cpuIdle >= previous.cpuIdle) {
        const totalDelta = cpuTotal - previous.cpuTotal;
        cpuPercent = Math.max(0, Math.min(100,
          100 * (totalDelta - (cpuIdle - previous.cpuIdle)) / totalDelta));
      }
      let network = "";
      if (now > previous.receivedAt && netRx >= previous.netRx && netTx >= previous.netTx) {
        const seconds = Math.max(.1, (now - previous.receivedAt) / 1000);
        network = formatNetworkRates(
          (netTx - previous.netTx) / seconds,
          (netRx - previous.netRx) / seconds);
      }
      session.metrics = {
        state: "available", receivedAt: now, cpuTotal, cpuIdle, netRx, netTx,
        cpuPercent,
        cpuSampledAt: Number.isFinite(cpuPercent) &&
          now - (previous.cpuSampledAt || 0) >= 500 ? now : previous.cpuSampledAt || 0,
        cpuHistory: Number.isFinite(cpuPercent) &&
          now - (previous.cpuSampledAt || 0) >= 500
          ? [...(previous.cpuHistory || []), cpuPercent].slice(-30)
          : previous.cpuHistory || [],
        network,
        latency: previous.latency,
        isProxyJump: previous.isProxyJump,
        proxyHost: previous.proxyHost,
        memoryTotal: number(payload.memoryTotal),
        memoryAvailable: number(payload.memoryAvailable),
        uptime: number(payload.uptime),
        diskMount: String(payload.diskMount || ""),
        diskTotal: number(payload.diskTotal),
        diskUsed: number(payload.diskUsed),
        diskAvailable: number(payload.diskAvailable),
        diskPartitions: Array.isArray(payload.diskPartitions) ? payload.diskPartitions : []
      };
      if (focusedSessionId === session.sessionId) renderServerMetrics();
      return;
    }

    if (message.event === "session.latency") {
      const session = sessions.get(message.sessionId);
      if (!session) return;
      const latency = Number(payload.latency);
      session.metrics = {
        ...session.metrics,
        latency: Number.isFinite(latency) ? latency : -1,
        isProxyJump: Boolean(payload.isProxyJump),
        proxyHost: String(payload.proxyHost || "")
      };
      if (focusedSessionId === session.sessionId) renderServerMetrics();
      return;
    }

    if (message.event === "session.state") {
      const previousState = sessions.get(message.sessionId)?.state;
      if (payload.state === "connecting")
        openTerminal(message.sessionId, payload.name || "终端",
          payload.connectionType || (message.sessionId.startsWith("serial-") ? "serial" : "ssh"),
          Number.isInteger(payload.profileIndex) ? payload.profileIndex : -1);
      const session = sessions.get(message.sessionId);
      if (!session) return;
      let writeSshFailureOutput = false;
      if (payload.state === "error" && session.connectionType === "ssh"
          && previousState === "connected") {
        notify(
          "SSH 连接已断开",
          `${session.name}${payload.message ? `：${payload.message}` : ""}`
        );
      } else if (payload.state === "error"
          && session.connectionType === "ssh") {
        const reason = String(payload.message || "未知错误");
        showTransientStatus(`SSH 连接失败：${session.name} · ${reason}`);
        writeSshFailureOutput = true;
        notify("SSH 连接失败", `${session.name}：${reason}`);
      }
      if (payload.state === "connected") {
        session.reconnectAttempts = 0;
        if (session.connectionType !== "rdp" && rdpConnectedStatusSessionId)
          clearRdpConnectedStatus(rdpConnectedStatusSessionId);
      } else if (payload.state === "error"
          && previousState !== "connecting"
          && appSettings.autoReconnect
          && session.connectionType === "ssh"
          && session.reconnectAttempts < appSettings.reconnectAttempts) {
        session.reconnectAttempts += 1;
        const attempt = session.reconnectAttempts;
        const sessionId = message.sessionId;
        showTransientStatus(
          `连接已断开，${attempt}/${appSettings.reconnectAttempts} 秒后自动重连…`);
        setTimeout(() => {
          if (sessions.get(sessionId)
              && (sessions.get(sessionId).state === "error"
                  || sessions.get(sessionId).state === "closed"))
            reconnectSession(sessionId);
        }, 2000 * attempt);
      }
      updateSessionState(session, payload.state);
      if (focusedSessionId === session.sessionId && payload.state !== "connected")
        renderServerMetrics();
      if (payload.state === "connected") {
        showTransientConnectionStatus(`${session.name} 已连接`, message.sessionId);
        if (session.connectionType === "ssh") {
          const profile = profilesByIndex.get(session.profileIndex);
          if (profile) {
            if (focusedSessionId === message.sessionId) {
              openSftp(profile, message.sessionId);
              activateFunctionPanel("sftp");
            }
            const profileAddress = String(profile.address || "");
            const profileUser = String(profile.username
              || (profileAddress.includes("@") ? profileAddress.slice(0, profileAddress.lastIndexOf("@")) : ""));
            const profileHome = profileUser === "root" ? "/root"
              : (profileUser ? `/home/${profileUser}` : "/");
            session.completion.diagnostics = {
              state: "正在请求",
              sourcePath: `${profileHome === "/" ? "" : profileHome}/.zsh_history` || "/.zsh_history"
            };
            let historySettled = false;
            const settleHistory = () => {
              if (historySettled || !sessions.has(session.sessionId)) return;
              historySettled = true;
              session.completion.loaded = true;
              if (session.completion.visible) renderCompletion(session);
            };
            const historyTimeout = setTimeout(() => {
              if (session.completion.diagnostics.state === "正在请求")
                session.completion.diagnostics.state = "前端等待超过 4 秒";
              settleHistory();
            }, 4000);
            post("history.load", {
              index: session.profileIndex,
              limit: appSettings.historyLimit,
              days: appSettings.historyDays,
              readBash: appSettings.readBashHistory,
              readZsh: appSettings.readZshHistory,
              deduplicate: appSettings.deduplicateHistory
            })
              .then(result => {
                clearTimeout(historyTimeout);
                if (!sessions.has(session.sessionId)) return;
                const commands = Array.isArray(result) ? result : result?.commands;
                session.completion.diagnostics = {
                  state: "后端响应成功",
                  ...(result?.diagnostics || {})
                };
                session.commandHistory = Array.from(new Set([
                  ...session.commandHistory,
                  ...(Array.isArray(commands) ? commands : [])
                ])).slice(-appSettings.historyLimit || undefined);
                if (session.completion.visible) renderCompletion(session);
                settleHistory();
              })
              .catch(error => {
                clearTimeout(historyTimeout);
                session.completion.diagnostics = {
                  state: "后端响应失败",
                  error: error instanceof Error ? error.message : String(error)
                };
                settleHistory();
              });
          }
        }
      } else if (payload.state === "error" || payload.state === "closed") {
        closeSftpForSession(message.sessionId);
        const writeSessionStateOutput = () => {
          if (!sessions.has(message.sessionId)) return;
          if (writeSshFailureOutput) {
            const reason = String(payload.message || "未知错误");
            session.terminal?.write(
              `\r\n\x1b[31mSSH 连接失败：${reason}\x1b[0m\r\n`);
          }
          appendOutput(session,
            `\r\n[${payload.state}${payload.message ? `: ${payload.message}` : ""}]\r\n`);
        };
        const restore = session.connectionType === "ssh"
          ? session.restoreLocalAlternateScreen?.() : null;
        if (restore && typeof restore.then === "function")
          restore.then(writeSessionStateOutput);
        else
          writeSessionStateOutput();
      }
      return;
    }

    if (message.event !== "session.output") return;
    const session = sessions.get(message.sessionId);
    if (!session) return;
    const bytes = Uint8Array.from(atob(payload.data), character => character.charCodeAt(0));
    const text = session.decoder.decode(bytes, { stream: true });
    const hex = Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join(" ");
    session.lastRawBytes = `${session.lastRawBytes} ${hex}`.trim().slice(-8192);
    session.lastDecodedText = `${session.lastDecodedText}${text}`.slice(-4096);
    const plain = text.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "");
    const remoteAlternateEntered = /\x1b\[\?(?:47|1047|1049)h/.test(text);
    const remoteAlternateExited = /\x1b\[\?(?:47|1047|1049)l/.test(text);
    if (remoteAlternateEntered) session.remoteAlternateScreen = true;
    if (remoteAlternateExited) {
      session.remoteAlternateScreen = false;
      // xterm has already returned to the normal buffer while parsing the
      // remote exit sequence. Do not issue a second 1049l later: that would
      // restore the saved cursor from the fallback screen and move the shell
      // cursor back to the command line that launched OpenCode.
      session.alternateScreen = false;
      session.fullScreen = false;
      session.pendingExit = false;
      session.sawTopExit = false;
    }
    if (session.connectionType === "ssh" &&
        /(?:^|[\r\n])top - \d|%Cpu\(s\):|Tasks:\s+\d+\s+total/.test(plain))
      session.enterAlternateScreen();
    const topHasRestoredCursor = session.pendingExit &&
      (text.includes("\x1b[?25h") || text.includes("\x1b[?1l"));
    if (session.pendingExit) {
      const exitMarker = text.lastIndexOf("\x1b[?1l");
      if (exitMarker >= 0) session.sawTopExit = true;
    }
    session.terminal.write(colorizeTerminalOutput(text), () => {
      if (remoteAlternateExited) session.syncAfterRemoteAlternateExit?.();
      if (!topHasRestoredCursor && !session.sawTopExit) return;
      if (session.leaveTimer) clearTimeout(session.leaveTimer);
      session.leaveTimer = setTimeout(session.leaveAlternateScreen, 100);
    });
  };

  const profileSearchText = profile => {
    const address = String(profile?.address || "");
    const at = address.indexOf("@");
    const user = at >= 0 ? address.slice(0, at) : "";
    return [
      String(profile?.name || ""),
      address,
      user,
      String(profile?.workspace || "未分配"),
      String(profile?.tag || "")
    ].join(" ").toLowerCase();
  };
  const matchesServerSearch = profile => {
    if (!serverSearchQuery) return true;
    const haystack = profileSearchText(profile);
    return serverSearchQuery.split(/\s+/).filter(Boolean)
      .every(keyword => haystack.includes(keyword));
  };
  const visibleProfiles = profiles => profiles.filter(profile =>
    (activeWorkspace === "全部连接" || (profile.workspace || "未分配") === activeWorkspace)
    && matchesServerSearch(profile));

  const connectionTypeLabel = profile => profile?.connectionType === "rdp"
    ? "RDP" : profile?.connectionType === "serial" ? "串口" : "SSH";
  const profileDisplayTag = profile => profile?.tagMode === "custom"
    && String(profile?.tag || "").trim()
    ? String(profile.tag).trim() : connectionTypeLabel(profile);
  const profileTypeColor = profile => {
    return connectionTypeColor(profile?.connectionType);
  };
  const profileGroupKey = profile => {
    const mode = appSettings.serverGroupMode;
    if (mode === "workspace") return `workspace:${profile.workspace || "未分配"}`;
    if (mode === "workspace-type")
      return `workspace-type:${profile.workspace || "未分配"}:${profile.connectionType || "ssh"}`;
    if (mode === "type") return `type:${profile.connectionType || "ssh"}`;
    return "all";
  };
  const profileGroupLabel = key => {
    if (key === "all") return "";
    const [, first, second] = key.split(":");
    if (key.startsWith("type:")) return connectionTypeLabel({ connectionType: first });
    if (key.startsWith("workspace-type:")) return `${first} / ${connectionTypeLabel({ connectionType: second })}`;
    return first || "未分配";
  };
  const compareServerProfiles = (first, second) => {
    const mode = appSettings.serverSortMode;
    if (mode === "manual") return Number(first.index) - Number(second.index);
    if (mode === "recent") {
      const times = readRecentConnectionTimes();
      const firstTime = Number(times[recentConnectionKey(first)]) || 0;
      const secondTime = Number(times[recentConnectionKey(second)]) || 0;
      if (firstTime !== secondTime) return secondTime - firstTime;
    }
    const collator = new Intl.Collator("zh-CN", { numeric: true, sensitivity: "base" });
    const firstValue = mode === "address-asc" ? first.address : first.name || first.address;
    const secondValue = mode === "address-asc" ? second.address : second.name || second.address;
    const result = collator.compare(String(firstValue || ""), String(secondValue || ""));
    return (mode === "name-desc" ? -result : result)
      || Number(first.index) - Number(second.index);
  };
  const serverDisplayGroups = profiles => {
    const visible = visibleProfiles(profiles).slice().sort(compareServerProfiles);
    const groups = new Map();
    visible.forEach(profile => {
      const key = profileGroupKey(profile);
      if (!groups.has(key)) groups.set(key, { key, label: profileGroupLabel(key), profiles: [] });
      groups.get(key).profiles.push(profile);
    });
    const order = key => {
      const mode = appSettings.serverGroupMode;
      if (mode === "type") {
        const type = key.slice("type:".length);
        return ({ ssh: 0, rdp: 1, serial: 2 }[type] ?? 99);
      }
      if (mode === "workspace" || mode === "workspace-type") {
        const workspace = key.split(":")[1] || "未分配";
        const index = workspaceNames.indexOf(workspace);
        return index >= 0 ? index : 1000;
      }
      return 0;
    };
    const orderedGroups = [...groups.values()].sort((first, second) =>
      order(first.key) - order(second.key)
      || String(first.label).localeCompare(String(second.label), "zh-CN"));
    return { visible, groups: orderedGroups };
  };

  const groupConnectionType = group => {
    if (!group?.key) return "";
    if (group.key.startsWith("type:"))
      return group.key.slice("type:".length);
    if (group.key.startsWith("workspace-type:"))
      return group.key.slice(group.key.lastIndexOf(":") + 1);
    return "";
  };
  const groupWorkspace = group => {
    if (!group?.key) return "";
    if (group.key.startsWith("workspace:"))
      return group.key.slice("workspace:".length);
    if (group.key.startsWith("workspace-type:")) {
      const type = groupConnectionType(group);
      const prefix = "workspace-type:";
      return group.key.slice(prefix.length, -(type.length + 1));
    }
    return "";
  };
  const resolveProfileDrop = (source, target) => {
    if (!source || !target || source.index === target.index) return null;
    const mode = appSettings.serverGroupMode;
    if ((mode === "type" || mode === "workspace-type")
        && source.connectionType !== target.connectionType)
      return null;
    return {
      workspace: mode === "workspace" || mode === "workspace-type"
        ? (target.workspace || "未分配") : null
    };
  };
  const resolveGroupDrop = (source, group) => {
    if (!source || !group?.profiles?.length) return null;
    const mode = appSettings.serverGroupMode;
    const type = groupConnectionType(group);
    if ((mode === "type" || mode === "workspace-type")
        && type && source.connectionType !== type)
      return null;
    return {
      target: group.profiles[group.profiles.length - 1],
      before: false,
      workspace: mode === "workspace" || mode === "workspace-type"
        ? (groupWorkspace(group) || "未分配") : null
    };
  };

  const reorderProfiles = async (
    sourceIndex, targetIndex, before, workspace = null) => {
    if (sourceIndex === targetIndex || sourceIndex == null || targetIndex == null)
      return;
    const payload = {
      index: sourceIndex,
      targetIndex,
      before: Boolean(before)
    };
    if (workspace !== null && workspace !== undefined)
      payload.workspace = workspace;
    await post("server.reorder", payload);
    await refreshProfiles();
  };
  const enableManualServerSort = () => {
    if (appSettings.serverSortMode === "manual") return;
    appSettings = { ...appSettings, serverSortMode: "manual" };
    if (serverSortMode) serverSortMode.value = "manual";
    localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
    showTransientStatus("已切换为手动顺序，可继续调整连接位置。");
  };

  const render = profiles => {
    profilesCache = profiles;
    renderWorkspaceFilter();
    const root = document.querySelector("#servers");
    root.replaceChildren();
    profilesByIndex.clear();
    profiles.forEach(profile => profilesByIndex.set(profile.index, profile));
    for (const index of selectedProfileIndexes)
      if (!profilesByIndex.has(index)) selectedProfileIndexes.delete(index);
    const display = serverDisplayGroups(profiles);
    const visible = display.visible;
    displayedProfileOrder = visible.slice();
    const groupContainers = new Map();
    display.groups.forEach(group => {
      const wrapper = document.createElement("section");
      wrapper.className = "server-group";
      const collapsedKey = `${appSettings.serverGroupMode}:${group.key}`;
      const collapsed = collapsedServerGroups.has(collapsedKey);
      wrapper.classList.toggle("collapsed", collapsed);
      if (group.label) {
        const header = document.createElement("button");
        header.type = "button";
        header.className = "server-group-header";
        header.setAttribute("aria-expanded", String(!collapsed));
        const type = group.key.startsWith("type:")
          ? group.key.slice("type:".length)
          : group.key.startsWith("workspace-type:")
            ? group.key.split(":").pop() : "";
        header.style.setProperty("--group-color", profileTypeColor({ connectionType: type }));
        header.innerHTML = `<span class="group-chevron">${collapsed ? "▸" : "▾"}</span><span class="group-dot"></span><span></span><span class="server-group-count"></span>`;
        header.querySelector("span:nth-child(3)").textContent = group.label;
        header.querySelector(".server-group-count").textContent = String(group.profiles.length);
        header.addEventListener("click", () => {
          if (collapsed) collapsedServerGroups.delete(collapsedKey);
          else collapsedServerGroups.add(collapsedKey);
          render(profilesCache);
        });
        wrapper.appendChild(header);
      }
      const items = document.createElement("div");
      items.className = "server-group-items";
      wrapper.appendChild(items);
      root.appendChild(wrapper);
      group.profiles.forEach(profile => groupContainers.set(profile.index, items));
      wrapper.addEventListener("dragover", event => {
        if (event.target.closest?.(".server")) return;
        const source = profilesByIndex.get(draggingProfileIndex);
        const drop = resolveGroupDrop(source, group);
        wrapper.classList.toggle("drop-target", Boolean(drop));
        wrapper.classList.toggle("drop-invalid", !drop && Boolean(source));
        if (!drop) {
          if (event.dataTransfer) event.dataTransfer.dropEffect = "none";
          return;
        }
        event.preventDefault();
        if (event.dataTransfer) event.dataTransfer.dropEffect = "move";
      });
      wrapper.addEventListener("dragleave", event => {
        if (!wrapper.contains(event.relatedTarget))
          wrapper.classList.remove("drop-target", "drop-invalid");
      });
      wrapper.addEventListener("drop", event => {
        if (event.target.closest?.(".server")) return;
        const sourceIndex = draggingProfileIndex;
        const source = profilesByIndex.get(sourceIndex);
        const drop = resolveGroupDrop(source, group);
        wrapper.classList.remove("drop-target", "drop-invalid");
        draggingProfileIndex = null;
        if (!drop) return;
        event.preventDefault();
        suppressServerClick = true;
        setTimeout(() => { suppressServerClick = false; }, 0);
        enableManualServerSort();
        reorderProfiles(
          sourceIndex, drop.target.index, drop.before, drop.workspace)
          .catch(reportError);
      });
    });
    visible.forEach(profile => {
      const item = document.createElement("div");
      const serial = profile.connectionType === "serial";
      const rdp = profile.connectionType === "rdp";
      item.className = `server ${serial ? "serial-server" : rdp ? "rdp-server" : "ssh-server"}`;
      // Dragging is an explicit request for manual ordering.  Allow it from
      // any current sort mode and switch to manual after a valid drop.
      item.draggable = true;
      item.dataset.profileIndex = String(profile.index);
      item.classList.toggle("selected", selectedProfileIndexes.has(profile.index));
      item.style.setProperty("--server-accent", profile.color || "#8ab4f8");
      item.style.setProperty("--server-type-color", profileTypeColor(profile));
      const detail = serial
        ? `${profile.address} · ${profile.port} baud`
        : `${profile.address}:${profile.port}`;
      const iconLabels = { server: "▣", terminal: "⌁", cloud: "☁", database: "▤", serial: "⌘", rdp: "🖥" };
      item.innerHTML = `<div class="server-name"><span class="server-title"><b class="server-icon" aria-hidden="true"></b><span class="server-label"></span></span><small class="server-tag"><i class="server-tag-dot" aria-hidden="true"></i><span class="server-tag-text"></span></small></div><div class="address"></div>`;
      item.querySelector(".server-icon").textContent = iconLabels[profile.icon] || iconLabels.server;
      item.querySelector(".server-label").textContent = profile.name || profile.address;
      item.querySelector(".server-tag-text").textContent = profileDisplayTag(profile);
      item.querySelector(".address").textContent = detail;
      item.addEventListener("click", event => {
        if (suppressServerClick) {
          event.preventDefault();
          return;
        }
        const position = visible.findIndex(candidate => candidate.index === profile.index);
        if (event.shiftKey && position >= 0) {
          const selectedPositions = visible
            .map((candidate, index) => selectedProfileIndexes.has(candidate.index) ? index : -1)
            .filter(index => index >= 0);
          const anchor = selectedPositions.length ? Math.min(...selectedPositions) : position;
          selectedProfileIndexes.clear();
          visible.slice(Math.min(anchor, position), Math.max(anchor, position) + 1)
            .forEach(candidate => selectedProfileIndexes.add(candidate.index));
          render(profiles);
          return;
        }
        if (event.ctrlKey || event.metaKey) {
          if (selectedProfileIndexes.has(profile.index)) selectedProfileIndexes.delete(profile.index);
          else selectedProfileIndexes.add(profile.index);
          render(profiles);
          return;
        }
        selectedProfileIndexes.clear();
        selectedProfileIndexes.add(profile.index);
        render(profiles);
      });
      item.addEventListener("dblclick", () => connectProfile(profile));
      item.addEventListener("dragstart", event => {
        draggingProfileIndex = profile.index;
        event.dataTransfer?.setData("text/plain", String(profile.index));
        if (event.dataTransfer)
          event.dataTransfer.effectAllowed = "move";
        item.classList.add("dragging");
      });
      item.addEventListener("dragover", event => {
        if (draggingProfileIndex == null
            || draggingProfileIndex === profile.index)
          return;
        const source = profilesByIndex.get(draggingProfileIndex);
        const drop = resolveProfileDrop(source, profile);
        item.classList.toggle("drop-invalid", !drop && Boolean(source));
        if (!drop) {
          item.classList.remove("drop-before", "drop-after");
          if (event.dataTransfer) event.dataTransfer.dropEffect = "none";
          return;
        }
        event.preventDefault();
        if (event.dataTransfer)
          event.dataTransfer.dropEffect = "move";
        const bounds = item.getBoundingClientRect();
        const before = event.clientY < bounds.top + bounds.height / 2;
        item.classList.toggle("drop-before", before);
        item.classList.toggle("drop-after", !before);
      });
      item.addEventListener("dragleave", event => {
        if (!item.contains(event.relatedTarget))
          item.classList.remove("drop-before", "drop-after", "drop-invalid");
      });
      item.addEventListener("drop", event => {
        const sourceIndex = draggingProfileIndex;
        const source = profilesByIndex.get(sourceIndex);
        const drop = resolveProfileDrop(source, profile);
        item.classList.remove("drop-before", "drop-after", "drop-invalid");
        if (!drop) return;
        event.preventDefault();
        const bounds = item.getBoundingClientRect();
        const before = event.clientY < bounds.top + bounds.height / 2;
        draggingProfileIndex = null;
        suppressServerClick = true;
        setTimeout(() => { suppressServerClick = false; }, 0);
        if (sourceIndex == null || sourceIndex === profile.index)
          return;
        enableManualServerSort();
        reorderProfiles(sourceIndex, profile.index, before, drop.workspace)
          .catch(reportError);
      });
      item.addEventListener("dragend", () => {
        draggingProfileIndex = null;
        document.querySelectorAll(
          ".server.drop-before,.server.drop-after,.server.drop-invalid,"
            + ".server.dragging,.server-group.drop-target,.server-group.drop-invalid")
          .forEach(element => element.classList.remove(
            "drop-before", "drop-after", "drop-invalid", "dragging",
            "drop-target"));
      });
      item.addEventListener("contextmenu", event => openServerContextMenu(event, profile));
      groupContainers.get(profile.index)?.appendChild(item);
    });
    const visibleCount = root.querySelectorAll(".server").length;
    if (!visible.length)
      status.textContent = serverSearchQuery
        ? `没有匹配“${serverSearchQuery}”的连接`
        : "没有可显示的连接";
    else if (serverSearchQuery)
      status.textContent = activeWorkspace === "全部连接"
        ? `找到 ${visibleCount} 个匹配“${serverSearchQuery}”的连接`
        : `${activeWorkspace} · 找到 ${visibleCount} 个匹配`;
    else
      status.textContent = activeWorkspace === "全部连接"
        ? `${profiles.length} 个服务器配置`
        : `${activeWorkspace} · ${visibleCount} 个连接`;
  };

  document.querySelector(".server-list-body").addEventListener("click", event => {
    if (event.target.closest(".server")) return;
    if (!selectedProfileIndexes.size) return;
    selectedProfileIndexes.clear();
    render(profilesCache);
  });

  document.querySelector("#sftp-refresh").addEventListener(
    "click", () => loadSftpDirectory(sftpPath, false));
  document.querySelector("#sftp-new-folder").addEventListener(
    "click", () => createRemoteEntry(true));
  document.querySelector("#sftp-new-file").addEventListener(
    "click", () => createRemoteEntry(false));
  sftpUploadButton.addEventListener("click", chooseAndStartSftpUpload);
  sftpDownloadButton.addEventListener("click", () => {
    const selected = [...sftpSelectedPaths]
      .map(path => sftpEntriesByPath.get(path)).filter(Boolean);
    downloadSelectedRemoteEntries(selected);
  });
  sftpCancelButton?.addEventListener("click", cancelSftpTransfer);
  sftpFavoriteButton.addEventListener("click", event => {
    event.stopPropagation();
    if (sftpFavoritesMenu.hidden) showSftpFavoritesMenu();
    else hideSftpFavoritesMenu();
  });
  sftpFavoritesList.addEventListener("click", event => {
    const path = event.target.closest("button[data-path]")?.dataset.path;
    if (!path) return;
    hideSftpFavoritesMenu();
    loadSftpDirectory(path);
  });
  localFavoriteButton.addEventListener("click", event => {
    event.stopPropagation();
    if (localFavoritesMenu.hidden) showLocalFavoritesMenu();
    else hideLocalFavoritesMenu();
  });
  localFavoritesList.addEventListener("click", event => {
    const path = event.target.closest("button[data-path]")?.dataset.path;
    if (!path) return;
    hideLocalFavoritesMenu();
    loadLocalDirectory(path);
  });
  sftpTableWrap.addEventListener("contextmenu", event => {
    if (!event.target.closest("tbody tr"))
      showSftpContextMenu(event);
  });
  sftpContextMenu.addEventListener("click", event => {
    const button = event.target.closest("button[data-action]");
    const action = button?.dataset.action;
    const selected = [...sftpSelectedPaths]
      .map(path => sftpEntriesByPath.get(path)).filter(Boolean);
    const entry = selected.length === 1 ? selected[0] : null;
    hideSftpContextMenu();
    if (!action) return;
    if (action === "upload") {
      chooseAndStartSftpUpload();
    } else if (action === "cd-terminal") {
      const targetPath = (entry && entry.directory) ? entry.path : sftpPath;
      cdInActiveTerminal(targetPath);
    } else if (action === "new-folder") {
      createRemoteEntry(true);
    } else if (action === "new-file") {
      createRemoteEntry(false);
    } else if (action === "refresh") {
      loadSftpDirectory(sftpPath, false);
    } else if (action === "favorite") {
      toggleSftpFavorite(button.dataset.path || sftpPath);
    } else if (action === "open") {
      if (!entry) return;
      if (entry.directory) loadSftpDirectory(entry.path);
      else openRemoteFile(entry);
    } else if (action === "open-with") {
      if (entry) openRemoteFile(entry, "choose");
    } else if (action === "open-default") {
      if (entry) openRemoteFile(entry, "windows-dialog");
    } else if (action === "preview") {
      if (entry) openRemotePreview(entry);
    } else if (action === "download") {
      downloadSelectedRemoteEntries(selected);
    } else if (action === "copy-path") {
      if (!entry) return;
      post("clipboard.write", { text: entry.path })
        .then(() => { sftpStatusText.textContent = `已复制远程路径：${entry.path}`; })
        .catch(reportError);
    } else if (action === "copy-to") {
      copyRemoteEntry(entry);
    } else if (action === "move-to") {
      moveRemoteEntry(entry);
    } else if (action === "rename") {
      if (!entry) return;
      renameRemoteEntry(entry);
    } else if (action === "chmod") {
      chmodRemoteEntry(entry);
    } else if (action === "properties") {
      showRemoteProperties(entry);
    } else if (action === "delete") {
      removeSelectedRemoteEntries(selected);
    }
  });
  localTableWrap.addEventListener("contextmenu", event => {
    if (!event.target.closest("tbody tr"))
      showLocalContextMenu(event);
  });
  localContextMenu.addEventListener("click", event => {
    const action = event.target.closest("button[data-action]")?.dataset.action;
    const selected = [...localSelectedPaths]
      .map(path => localEntriesByPath.get(path)).filter(Boolean);
    const entry = selected.length === 1 ? selected[0] : null;
    hideLocalContextMenu();
    if (action === "open" && entry?.directory)
      loadLocalDirectory(entry.path);
    else if (action === "upload")
      uploadSelectedLocalEntries();
    else if (action === "sync")
      syncLocalToRemote();
    else if (action === "new-folder")
      createLocalEntry(true);
    else if (action === "new-file")
      createLocalEntry(false);
    else if (action === "refresh")
      loadLocalDirectory(localPath, false);
    else if (action === "favorite")
      toggleLocalFavorite(event.target.closest("button")?.dataset.path || localPath);
    else if (action === "rename")
      renameLocalEntry(entry);
    else if (action === "delete")
      removeSelectedLocalEntries();
  });
  // WebView2 browser UI is disabled by the native host as well. Keep this
  // capture-phase fallback so no document region can expose Chromium's menu.
  document.addEventListener("contextmenu", event => event.preventDefault(), true);
  window.addEventListener("keydown", event => {
    if ((event.ctrlKey || event.metaKey) && (event.key.toLowerCase() === "p" || event.key.toLowerCase() === "k") && !event.shiftKey && !event.altKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      toggleQuickSwitcher();
      return;
    }
    if ((event.ctrlKey || event.metaKey) && (event.key === "/" || event.key === "?") && !event.altKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      toggleShortcutsDialog();
      return;
    }
    if (event.key === "F1" && !event.ctrlKey && !event.altKey && !event.metaKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      toggleShortcutsDialog();
      return;
    }
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "w" && !event.shiftKey && !event.altKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      closeCurrentTab();
      return;
    }
    if ((event.ctrlKey || event.metaKey) && (event.key === "," || event.code === "Comma") && !event.shiftKey && !event.altKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      openSettingsDialog();
      return;
    }
    if (event.ctrlKey && event.key === "Tab") {
      event.preventDefault();
      event.stopImmediatePropagation();
      cycleTab(!event.shiftKey);
      return;
    }
    if (event.altKey && !event.ctrlKey && !event.shiftKey && !event.metaKey
        && event.key >= "1" && event.key <= "9") {
      event.preventDefault();
      event.stopImmediatePropagation();
      switchToTabByIndex(parseInt(event.key, 10));
      return;
    }
    if (event.altKey && (event.key.toLowerCase() === "m" || event.key.toLowerCase() === "z") && !event.ctrlKey && !event.metaKey) {
      if (splitMode) {
        event.preventDefault();
        event.stopImmediatePropagation();
        toggleSplitZoom();
        return;
      }
    }
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "f" && !event.shiftKey && !event.altKey) {
      if (activeSession && activeSession.connectionType !== "rdp") {
        event.preventDefault();
        event.stopImmediatePropagation();
        openTerminalSearch(activeSession);
        return;
      }
    }
    if (event.altKey && event.key.toLowerCase() === "b" && !event.ctrlKey && !event.metaKey) {
      event.preventDefault();
      event.stopImmediatePropagation();
      toggleBroadcastInput();
      return;
    }
    if (event.code === "Space" && event.ctrlKey && !event.shiftKey && !event.altKey
        && !event.metaKey && activeSession?.connectionType === "ssh") {
      event.preventDefault();
      event.stopImmediatePropagation();
      if (activeSession.commandCursor === activeSession.commandLine.length)
        showCompletion(activeSession);
      return;
    }
    if (event.key === "F5"
        || ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "r")) {
      event.preventDefault();
      event.stopPropagation();
    }
  }, true);
  document.addEventListener("keydown", event => {
    if (event.defaultPrevented || actionDialog.open
        || event.ctrlKey || event.altKey || event.metaKey)
      return;
    const target = event.target;
    if (target instanceof HTMLElement
        && (target.matches("input,textarea,select,[contenteditable='true']")
            || target.closest(".terminal-card")))
      return;
    const sftpPanel = document.querySelector(
      ".function-panel[data-panel='sftp']");
    if (!sftpPanel?.classList.contains("active"))
      return;

    if (activeFilePane === "remote") {
      if (event.key.length === 1 && !event.ctrlKey && !event.altKey && !event.metaKey) {
        event.preventDefault();
        handleSftpTypeToJump(event.key);
        return;
      }
      if (event.key === "Backspace" && sftpJumpBuffer) {
        event.preventDefault();
        sftpJumpBuffer = sftpJumpBuffer.slice(0, -1);
        if (sftpJumpIndicator) {
          if (sftpJumpBuffer) {
            sftpJumpIndicator.textContent = `跳转: ${sftpJumpBuffer}`;
          } else {
            sftpJumpIndicator.hidden = true;
          }
        }
        return;
      }
      if (event.key === "Escape" && sftpJumpBuffer) {
        event.preventDefault();
        sftpJumpBuffer = "";
        if (sftpJumpIndicator) sftpJumpIndicator.hidden = true;
        return;
      }
    }

    if (event.key !== "Delete" && event.key !== "F2")
      return;

    if (activeFilePane === "local" && localPanelVisible) {
      const selected = [...localSelectedPaths]
        .map(path => localEntriesByPath.get(path)).filter(Boolean);
      if (!selected.length) return;
      event.preventDefault();
      if (event.key === "Delete")
        removeSelectedLocalEntries();
      else if (selected.length === 1)
        renameLocalEntry(selected[0]);
      return;
    }

    const selected = [...sftpSelectedPaths]
      .map(path => sftpEntriesByPath.get(path)).filter(Boolean);
    if (!selected.length) return;
    event.preventDefault();
    if (event.key === "Delete")
      removeSelectedRemoteEntries(selected);
    else if (selected.length === 1)
      renameRemoteEntry(selected[0]);
  });
  document.addEventListener("pointerdown", event => {
    if (!sftpContextMenu.hidden && !sftpContextMenu.contains(event.target))
      hideSftpContextMenu();
    if (!sftpFavoritesMenu.hidden
        && !sftpFavoritesMenu.contains(event.target)
        && !sftpFavoriteButton.contains(event.target))
      hideSftpFavoritesMenu();
    if (!localContextMenu.hidden && !localContextMenu.contains(event.target))
      hideLocalContextMenu();
    if (!localFavoritesMenu.hidden
        && !localFavoritesMenu.contains(event.target)
        && !localFavoriteButton.contains(event.target))
      hideLocalFavoritesMenu();
  });
  document.addEventListener("scroll", hideSftpContextMenu, true);
  document.addEventListener("scroll", hideSftpFavoritesMenu, true);
  document.addEventListener("scroll", hideLocalContextMenu, true);
  document.addEventListener("scroll", hideLocalFavoritesMenu, true);
  window.addEventListener("blur", hideSftpContextMenu);
  window.addEventListener("blur", hideSftpFavoritesMenu);
  window.addEventListener("blur", hideLocalContextMenu);
  window.addEventListener("blur", hideLocalFavoritesMenu);
  window.addEventListener("resize", hideSftpContextMenu);
  window.addEventListener("resize", hideSftpFavoritesMenu);
  window.addEventListener("resize", hideLocalContextMenu);
  window.addEventListener("resize", hideLocalFavoritesMenu);
  document.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      hideSftpContextMenu();
      hideSftpFavoritesMenu();
      hideLocalContextMenu();
      hideLocalFavoritesMenu();
    }
  });
  document.querySelector("#sftp-up").addEventListener(
    "click", () => loadSftpDirectory(remoteParent(sftpPath)));
  sftpBackButton.addEventListener("click", () => navigateSftpHistory(-1));
  sftpForwardButton.addEventListener("click", () => navigateSftpHistory(1));
  document.addEventListener("mousedown", event => {
    if ((event.button !== 3 && event.button !== 4)
        || !event.target.closest(".sftp-card")) return;
    event.preventDefault();
    if (localPanelVisible && event.target.closest(".local-pane"))
      navigateLocalHistory(event.button === 3 ? -1 : 1);
    else
      navigateSftpHistory(event.button === 3 ? -1 : 1);
  }, true);
  document.addEventListener("auxclick", event => {
    if ((event.button === 3 || event.button === 4)
        && event.target.closest(".sftp-card"))
      event.preventDefault();
  }, true);
  sftpPathInput.addEventListener("keydown", event => {
    if (event.key !== "Enter") return;
    event.preventDefault();
    loadSftpDirectory(sftpPathInput.value);
  });
  sftpLocalToggle.addEventListener(
    "click", () => setLocalPanelVisible(!localPanelVisible));
  localBackButton.addEventListener("click", () => navigateLocalHistory(-1));
  document.querySelector("#local-up").addEventListener("click", () => {
    if (localParentPath) loadLocalDirectory(localParentPath);
  });
  document.querySelector("#local-refresh").addEventListener(
    "click", () => loadLocalDirectory(localPath, false));
  localUploadButton.addEventListener("click", () => {
    uploadSelectedLocalEntries();
  });
  localPathInput.addEventListener("keydown", event => {
    if (event.key !== "Enter") return;
    event.preventDefault();
    loadLocalDirectory(localPathInput.value);
  });
  localOpenFolderButton.addEventListener("click", async () => {
    try {
      const result = await post("local.chooseDirectory", { path: localPath || "" });
      if (!result.cancelled && result.path) loadLocalDirectory(result.path);
    } catch (error) {
      localStatus.textContent = error.message;
      reportError(error);
    }
  });
  installTableColumnResizers(document.querySelector(".sftp-table"),
    "masterterm.sftpColumnWidths", [38, 12, 19, 12, 19]);
  installTableColumnResizers(document.querySelector(".local-table"),
    "masterterm.localColumnWidths", [52, 18, 30]);
  const installDirectoryTableControls = (table, getSort, setSort,
                                          renderEntries, getEntries) => {
    table.querySelector("thead").addEventListener("click", event => {
      const header = event.target.closest("th[data-sort]");
      if (!header) return;
      const current = getSort();
      setSort({
        key: header.dataset.sort,
        direction: current.key === header.dataset.sort ? -current.direction : 1
      });
      renderEntries(getEntries());
    });
  };
  installDirectoryTableControls(
    document.querySelector(".sftp-table"),
    () => sftpSort, value => { sftpSort = value; },
    renderSftpEntries, () => sftpDirectoryEntries);
  installDirectoryTableControls(
    document.querySelector(".local-table"),
    () => localSort, value => { localSort = value; },
    renderLocalEntries, () => localDirectoryEntries);
  installMarqueeSelection(sftpTableWrap, sftpFiles, sftpSelectedPaths,
    () => sftpEntriesByPath, updateSftpSelection);
  installMarqueeSelection(localTableWrap, localFiles, localSelectedPaths,
    () => localEntriesByPath, updateLocalSelection);
  document.querySelector("#action-dialog-close").addEventListener(
    "click", () => finishActionDialog("cancel"));
  actionDialog.addEventListener("cancel", event => {
    event.preventDefault();
    finishActionDialog("cancel");
  });
  document.querySelector("#action-dialog-form").addEventListener("submit", event => {
    event.preventDefault();
    const primary = actionDialogActions.querySelector("button.primary");
    if (primary) primary.click();
  });
  updateSftpHistoryButtons();
  updateTransferControls();
  installCardLayout();
  const createWorkspace = async () => {
    const name = await requestText("新建工作区", "请输入工作区名称：", "");
    if (name === null || !name.trim()) return;
    try {
      await post("workspace.create", { name: name.trim() });
      await refreshWorkspaces();
      render(profilesCache);
    } catch (error) {
      reportError(error);
    }
  };
  const exportConfig = async () => {
    const result = await post("config.export");
    if (result.cancelled) return;
    showTransientStatus(`配置已导出：${result.path}`);
  };
  const importConfig = async () => {
    const preview = await post("config.import.preview");
    if (preview.cancelled) return;
    const confirmed = await requestConfirmation(
      "导入连接配置",
      `文件：${preview.path}\n有效连接：${preview.servers}（无效项目：${preview.invalidServers}）\n工作区：${preview.workspaces}\n导入会合并现有连接，是否继续？`,
      "导入");
    if (!confirmed) return;
    const result = await post("config.import.apply", { token: preview.token });
    showTransientStatus(`配置已导入：新增 ${result.added}，更新 ${result.updated}`);
    await refreshWorkspaces();
    await refreshProfiles();
  };
  const restoreConfig = async () => {
    const result = await post("config.backup.restore");
    if (result.cancelled) return;
    showTransientStatus(`配置备份已恢复：${result.path}`);
    await refreshWorkspaces();
    await refreshProfiles();
  };
  workspaceFilterSelect.addEventListener("change", () => {
    activeWorkspace = workspaceFilterSelect.value;
    render(profilesCache);
  });
  if (serverSearchInput)
    serverSearchInput.addEventListener("input", () => {
      serverSearchQuery = serverSearchInput.value.trim().toLowerCase();
      render(profilesCache);
    });
  const openWorkspaceMenu = (event = null) => {
    contextMenuWorkspace = activeWorkspace;
    const editable = activeWorkspace !== "全部连接" && activeWorkspace !== "未分配";
    workspaceContextMenu.querySelector('[data-action="rename"]').disabled = !editable;
    workspaceContextMenu.querySelector('[data-action="delete"]').disabled = !editable;
    workspaceContextMenu.hidden = false;
    const anchor = workspaceManageButton.getBoundingClientRect();
    const bounds = workspaceContextMenu.getBoundingClientRect();
    const preferredLeft = event ? event.clientX : anchor.right - bounds.width;
    const preferredTop = event ? event.clientY : anchor.bottom + 4;
    workspaceContextMenu.style.left =
      `${Math.max(6, Math.min(preferredLeft, window.innerWidth - bounds.width - 6))}px`;
    workspaceContextMenu.style.top =
      `${Math.max(6, Math.min(preferredTop, window.innerHeight - bounds.height - 6))}px`;
  };
  workspaceManageButton.addEventListener("click", event => {
    event.stopPropagation();
    if (!workspaceContextMenu.hidden) {
      workspaceContextMenu.hidden = true;
      contextMenuWorkspace = "";
      clearRdpOverlay();
      return;
    }
    openWorkspaceMenu();
  });
  document.querySelector(".server-list-body").addEventListener("contextmenu", event => {
    if (event.target.closest(".server") || event.target.closest(".server-toolbar")) return;
    event.preventDefault();
    event.stopPropagation();
    openWorkspaceMenu(event);
  });
  workspaceContextMenu.addEventListener("click", async event => {
    const action = event.target.closest("button[data-action]")?.dataset.action;
    const name = contextMenuWorkspace;
    workspaceContextMenu.hidden = true;
    contextMenuWorkspace = "";
    clearRdpOverlay();
    if (!action) return;
    try {
      if (action === "import-config") {
        await importConfig();
        return;
      } else if (action === "export-config") {
        await exportConfig();
        return;
      } else if (action === "restore-config") {
        await restoreConfig();
        return;
      } else if (action === "create-ssh") {
        openServerEditor(null, "ssh");
        return;
      } else if (action === "create-rdp") {
        openServerEditor(null, "rdp");
        return;
      } else if (action === "create-serial") {
        openServerEditor(null, "serial");
        return;
      } else if (action === "create") {
        await createWorkspace();
        return;
      } else if (action === "rename" && name) {
        const newName = await requestText("重命名工作区", "请输入新名称：", name);
        if (newName === null || newName.trim() === name) return;
        await post("workspace.rename", { name, newName: newName.trim() });
        if (activeWorkspace === name) activeWorkspace = newName.trim();
      } else if (action === "delete" && name) {
        if (!await requestDeleteConfirmation(
            "删除工作区",
            `删除“${name}”后，其中的连接将移动到“未分配”。是否继续？`, "删除"))
          return;
        await post("workspace.delete", { name });
        workspaceNames = workspaceNames.filter(workspace => workspace !== name);
        if (activeWorkspace === name) activeWorkspace = "全部连接";
        renderWorkspaceFilter();
        render(profilesCache);
      }
      await refreshWorkspaces();
      await refreshProfiles();
    } catch (error) {
      reportError(error);
    }
  });
  document.querySelector("#server-create").addEventListener(
    "click", () => openServerEditor(null, "ssh"));
  installTerminalCreateGroup(terminalCreateGroup);
  document.addEventListener("pointerdown", event => {
    document.querySelectorAll(".terminal-create-group").forEach(group => {
      if (!event.target.closest(".terminal-create-group"))
        closeTerminalCreateDropdown(group);
    });
  });
  document.querySelector("#server-dialog-close").addEventListener("click", () => serverDialog.close());
  document.querySelector("#server-cancel").addEventListener("click", () => serverDialog.close());
  serverContextMenu.addEventListener("click", async event => {
    const action = event.target.closest("button")?.dataset.action;
    const profile = contextMenuProfile;
    if (!action || !profile) return;
    closeServerContextMenu();
    try {
      if (action === "connect")
        connectProfile(profile);
      else if (action === "edit")
        openServerEditor(profile);
      else if (action === "copy")
        await post("server.copy", { index: profile.index });
      else if (action === "move-up" || action === "move-down") {
        enableManualServerSort();
        const display = serverDisplayGroups(profilesCache);
        const group = display.groups.find(candidate =>
          candidate.profiles.some(item => item.index === profile.index));
        const visible = group?.profiles || displayedProfileOrder;
        const position = visible.findIndex(item => item.index === profile.index);
        const target = visible[position + (action === "move-up" ? -1 : 1)];
        const drop = resolveProfileDrop(profile, target);
        if (target && drop)
          await reorderProfiles(
            profile.index, target.index, action === "move-up", drop.workspace);
      }
      else if (action === "delete")
        await deleteProfile(profile);
      else if (action === "batch-workspace")
        await batchWorkspace();
      else if (action === "batch-color")
        await batchColor();
      else if (action === "batch-icon")
        await batchIcon();
      else if (action === "batch-delete")
        await batchDeleteProfiles();
      if (["copy", "delete", "batch-workspace", "batch-color", "batch-icon"].includes(action))
        await refreshProfiles();
    } catch (error) {
      reportError(error);
    }
  });
  terminalContextMenu.addEventListener("click", async event => {
    const action = event.target.closest("button")?.dataset.action;
    const sessionId = contextMenuSessionId;
    if (!action || !sessionId) return;
    const targetSession = sessions.get(sessionId);
    closeTerminalContextMenu();
    if (action === "reconnect")
      reconnectSession(sessionId);
    else if (action === "duplicate") {
      if (targetSession) {
        if (targetSession.connectionType === "local") {
          connectLocalTerminal(targetSession.shellType || null);
        } else if (targetSession.profileIndex !== undefined && targetSession.profileIndex >= 0) {
          const profile = profilesCache.find(p => p.index === targetSession.profileIndex);
          if (profile) openExistingConnection(profile);
        } else if (targetSession.connectionType === "ssh") {
          const profile = profilesCache.find(p => p.name === targetSession.name);
          if (profile) openExistingConnection(profile);
          else connectLocalTerminal();
        }
      }
    } else if (action === "rename-tab") {
      if (targetSession) {
        const newName = await requestText("重命名标签页", "请输入新的标签页名称：", targetSession.name || "");
        if (newName && newName.trim()) {
          targetSession.name = newName.trim();
          targetSession.displayName = targetSession.name;
          const tabLabel = targetSession.tab?.querySelector(".terminal-tab-label");
          if (tabLabel) tabLabel.textContent = targetSession.name;
          showTransientStatus(`标签页已更名为：${targetSession.name}`);
        }
      }
    } else if (action === "fullscreen" && targetSession?.connectionType === "rdp")
      requestRdpFullscreen(!rdpFullscreen).catch(reportError);
    else if (action === "diagnostics")
      showTerminalDiagnostics(sessionId);
    else if (action === "close")
      closeSession(sessionId);
    else if (action === "close-others")
      Promise.all(Array.from(sessions.keys())
        .filter(id => id !== sessionId).map(closeSession));
    else if (action === "close-all")
      Promise.all(Array.from(sessions.keys()).map(closeSession));
    else if (action === "move-left")
      moveTerminalTab(sessionId, -1);
    else if (action === "move-right")
      moveTerminalTab(sessionId, 1);
    else if (action === "split-vertical" || action === "split-horizontal") {
      const candidates = [...sessions.values()].filter(
        candidate => candidate.sessionId !== sessionId
          && candidate.connectionType !== "rdp"
          && candidate.state !== "closed" && candidate.state !== "error");
      if (!candidates.length) {
        showTransientStatus("没有其他可用终端会话用于分屏。");
        return;
      }
      const choice = await requestChoice(
        action === "split-vertical" ? "垂直分屏（左右）" : "水平分屏（上下）",
        "选择要并排显示的会话：",
        candidates.map(candidate => ({
          value: candidate.sessionId,
          label: `${candidate.name}（${candidate.sessionId}）`
        }))
      );
      if (!choice) return;
      setSplit(action === "split-vertical" ? "vertical" : "horizontal", choice);
      showTransientStatus("已分屏，按 Alt+Z 可放大当前子窗口，右键标签可关闭分屏。");
    } else if (action === "split-close") {
      setSplit(null, null);
      showTransientStatus("已关闭分屏。");
    } else if (action === "batch-command")
      openBatchCommand();
  });
  const openRdpFullscreenMenu = menu => {
    if (!rdpFullscreen) return;
    rdpFullscreenMenu = rdpFullscreenMenu === menu ? "" : menu;
    rdpFullscreenBarVisible = true;
    syncRdpFullscreenUi();
    if (!rdpFullscreenBarPinned)
      scheduleRdpFullscreenBarHide();
  };
  rdpFullscreenPin?.addEventListener("click", () => {
    if (!rdpFullscreen) return;
    rdpFullscreenBarPinned = !rdpFullscreenBarPinned;
    rdpFullscreenBarVisible = true;
    if (rdpFullscreenBarPinned) {
      if (rdpFullscreenBarTimer) {
        clearTimeout(rdpFullscreenBarTimer);
        rdpFullscreenBarTimer = 0;
      }
    } else {
      scheduleRdpFullscreenBarHide();
    }
    syncRdpFullscreenUi();
    notifyRdpLayout(false);
  });
  rdpFullscreenDisplay?.addEventListener(
    "click", () => openRdpFullscreenMenu("display"));
  rdpFullscreenConnection?.addEventListener(
    "click", () => openRdpFullscreenMenu("connection"));
  rdpFullscreenKeyboard?.addEventListener(
    "click", () => openRdpFullscreenMenu("keyboard"));
  rdpFullscreenQuality?.addEventListener("click", showRdpQualityPanel);
  rdpQualityClose?.addEventListener("click", hideRdpQualityPanel);
  rdpFullscreenMenuElement?.addEventListener("click", event => {
    const action = event.target.closest("[data-rdp-action]")?.dataset.rdpAction;
    if (!action) return;
    rdpFullscreenMenu = "";
    syncRdpFullscreenUi();
    post("session.rdpFullscreenAction", {
      sessionId: activeRdpSessionId(), action
    }).catch(reportError);
  });
  rdpFullscreenBar?.addEventListener("pointerdown", event => {
    event.stopPropagation();
  });
  rdpFullscreenBar?.addEventListener("pointerenter", () => {
    if (rdpFullscreenBarTimer) {
      clearTimeout(rdpFullscreenBarTimer);
      rdpFullscreenBarTimer = 0;
    }
  });
  rdpFullscreenBar?.addEventListener("pointerleave", scheduleRdpFullscreenBarHide);
  rdpFullscreenHotZone?.addEventListener("pointerenter", showRdpFullscreenBar);
  rdpFullscreenHotZone?.addEventListener("pointermove", showRdpFullscreenBar);
  rdpFullscreenClose?.addEventListener("click", () => {
    const session = activeSession?.connectionType === "rdp" ? activeSession : null;
    if (session) closeSession(session.sessionId);
  });
  const toggleRdpKeyMenu = () => {
    if (!rdpKeyToolboxBtn) return;
    if (activeRdpVisible()) {
      closeRdpKeyMenu();
      const rect = rdpKeyToolboxBtn.getBoundingClientRect();
      post("app.rdpKeyMenu", {
        x: Math.round(rect.left),
        y: Math.round(rect.bottom + 6)
      }).catch(reportError);
      return;
    }
    if (!rdpKeyMenu) return;
    if (!rdpKeyMenu.hidden) {
      closeRdpKeyMenu();
      return;
    }
    const rect = rdpKeyToolboxBtn.getBoundingClientRect();
    placeContextMenu(rdpKeyMenu, { clientX: rect.left, clientY: rect.bottom + 4 });
  };
  rdpKeyToolboxBtn?.addEventListener("click", event => {
    event.stopPropagation();
    toggleRdpKeyMenu();
  });
  rdpKeyMenu?.addEventListener("click", event => {
    const action = event.target.closest("[data-rdp-action]")?.dataset.rdpAction;
    if (!action) return;
    closeRdpKeyMenu();
    const sessionId = activeRdpSessionId() || (activeSession?.connectionType === "rdp" ? activeSession.sessionId : "");
    if (sessionId) {
      post("session.rdpAction", { sessionId, action }).catch(reportError);
    }
  });
  document.addEventListener("pointerdown", event => {
    if (rdpFullscreenMenu && !rdpFullscreenBar?.contains(event.target)) {
      rdpFullscreenMenu = "";
      syncRdpFullscreenUi();
    }
    if (rdpKeyMenu && !rdpKeyMenu.hidden && !rdpKeyMenu.contains(event.target) && event.target !== rdpKeyToolboxBtn) {
      closeRdpKeyMenu();
    }
    if (rdpQualityPanel && !rdpQualityPanel.hidden
        && !rdpQualityPanel.contains(event.target)
        && event.target !== rdpFullscreenQuality)
      hideRdpQualityPanel();
  });
  const terminalTabsContextMenu = document.querySelector("#terminal-tabs-context-menu");
  const closeTerminalTabsContextMenu = () => {
    const wasVisible = !terminalTabsContextMenu.hidden;
    terminalTabsContextMenu.hidden = true;
    terminalTabsContextGroup = null;
    if (wasVisible) clearRdpOverlay();
  };
  handleTerminalTabsAction = async (action, group = null) => {
    if (action === "close-all") {
      Promise.all(Array.from(sessions.keys()).map(closeSession));
    } else if (action === "split-close") {
      setSplit(null, null);
      showTransientStatus("已关闭分屏。");
    } else if (action === "batch-command") {
      openBatchCommand();
    } else if (action === "new-local") {
      connectLocalTerminal(null, group);
    } else if (action === "new-local-pwsh") {
      connectLocalTerminal("pwsh", group);
    } else if (action === "new-local-powershell") {
      connectLocalTerminal("powershell", group);
    } else if (action === "new-local-cmd") {
      connectLocalTerminal("cmd", group);
    } else if (action === "new-local-wsl") {
      connectLocalTerminal("wsl", group);
    } else if (action === "new-local-gitbash") {
      connectLocalTerminal("gitbash", group);
    } else if (action === "new-ssh") {
      const profiles = profilesCache.filter(
        profile => profile.connectionType === "ssh");
      if (!profiles.length) {
        showTransientStatus("没有可用的 SSH 连接。");
        return;
      }
      const choice = await requestChoice(
        "新建 SSH 终端", "选择服务器：",
        profiles.map(profile => ({
          value: String(profile.index), label: profile.name
        })));
      if (choice === null) {
        pendingNewTerminalGroup = null;
        return;
      }
      pendingNewTerminalGroup = group?.isConnected && splitMode ? group : null;
      post("session.connect", { index: Number(choice) }).catch(error => {
        pendingNewTerminalGroup = null;
        reportError(error);
      });
    } else if (action === "new-rdp") {
      const profiles = profilesCache.filter(
        profile => profile.connectionType === "rdp");
      if (!profiles.length) {
        showTransientStatus("没有可用的远程桌面 (RDP) 连接。");
        return;
      }
      const choice = await requestChoice(
        "打开远程桌面 (RDP)", "选择远程桌面配置：",
        profiles.map(profile => ({
          value: String(profile.index), label: profile.name || profile.address
        })));
      if (choice === null) return;
      post("session.connect", { index: Number(choice) }).catch(reportError);
    } else if (action === "sort-name") {
      const host = group || terminalTabs;
      const tabs = [...host.querySelectorAll(".terminal-tab")];
      tabs.sort((a, b) => {
        const firstSession = sessions.get(a.dataset.sessionId);
        const secondSession = sessions.get(b.dataset.sessionId);
        const first = firstSession?.displayName || firstSession?.name || "";
        const second = secondSession?.displayName || secondSession?.name || "";
        return first.localeCompare(second, "zh-CN", { numeric: true })
          || String(firstSession?.sessionId || "")
            .localeCompare(String(secondSession?.sessionId || ""));
      });
      appendTerminalTabs(host, tabs);
      showTransientStatus(`已按名称排序（${tabs.length} 个标签）`);
    } else if (action === "sort-type") {
      const host = group || terminalTabs;
      const order = { ssh: 0, serial: 1, local: 2, rdp: 3 };
      const tabs = [...host.querySelectorAll(".terminal-tab")];
      tabs.sort((a, b) => {
        const first = order[sessions.get(a.dataset.sessionId)?.connectionType] ?? 9;
        const second = order[sessions.get(b.dataset.sessionId)?.connectionType] ?? 9;
        return first - second;
      });
      appendTerminalTabs(host, tabs);
      if (splitMode) renderSplit();
      showTransientStatus(`已按连接类型排序（${tabs.length} 个标签）`);
    }
  };

  const showTerminalTabsContextMenu = (event, group = null) => {
    event.preventDefault();
    event.stopPropagation();
    terminalTabsContextGroup = group;
    // 当活动会话为 RDP 时，因原生 Win32 子窗口存在空域拦截，HTML 弹窗无法在外部点击时正常关闭且可能被遮挡。
    // 此时委托给底层的 Win32 TrackPopupMenuEx 原生菜单，点击外部瞬时关闭且绝无遮挡。
    if (activeRdpSessionId()) {
      post("session.rdpTabsContextMenu", {
        x: Math.round(event.clientX),
        y: Math.round(event.clientY),
        canCloseSplit: Boolean(splitMode)
      }).catch(reportError);
      return;
    }
    terminalTabsContextMenu.querySelector('[data-action="split-close"]').disabled = !splitMode;
    placeContextMenu(terminalTabsContextMenu, event);
    notifyRdpLayout(false, false);
  };
  terminalTabs.addEventListener("contextmenu", event => {
    if (event.target.closest(".terminal-tab")) return;
    closeTerminalContextMenu();
    showTerminalTabsContextMenu(event);
  });
  // Split panes own their tab groups outside #terminal-tabs.  Capture the
  // event so a blank area in either dynamically-created group has the same
  // management menu as the normal tab strip.
  document.addEventListener("contextmenu", event => {
    const group = event.target.closest(".tab-group");
    if (!group || event.target.closest(".terminal-tab")) return;
    closeTerminalContextMenu();
    showTerminalTabsContextMenu(event, group);
  }, true);
  terminalTabsContextMenu.addEventListener("click", async event => {
    const action = event.target.closest("button")?.dataset.action;
    const group = terminalTabsContextGroup;
    closeTerminalTabsContextMenu();
    if (action) {
      await handleTerminalTabsAction(action, group);
    }
  });
  document.addEventListener("pointerdown", event => {
    if (!terminalTabsContextMenu.hidden
        && !terminalTabsContextMenu.contains(event.target))
      closeTerminalTabsContextMenu();
  });
  document.addEventListener("contextmenu", event => {
    if (!event.target.closest("#terminal-tabs"))
      closeTerminalTabsContextMenu();
  });
  const splitNavContextMenu = document.querySelector("#split-nav-context-menu");
  const closeSplitNavContextMenu = () => {
    if (splitNavContextMenu) splitNavContextMenu.hidden = true;
  };
  const showSplitNavContextMenu = event => {
    if (!splitNavContextMenu) return;
    placeContextMenu(splitNavContextMenu, event);
  };
  splitNavContextMenu?.addEventListener("click", async event => {
    const action = event.target.closest("button")?.dataset.action;
    closeSplitNavContextMenu();
    if (action === "split-activate") {
      switchToSplitView();
    } else if (action === "split-vertical") {
      if (splitMode !== "vertical") {
        terminalPanels.classList.remove("split-horizontal");
        terminalPanels.classList.add("split-vertical");
        splitMode = "vertical";
        renderSplit();
        showTransientStatus("已切换为垂直分屏（左右）。");
      }
    } else if (action === "split-horizontal") {
      if (splitMode !== "horizontal") {
        terminalPanels.classList.remove("split-vertical");
        terminalPanels.classList.add("split-horizontal");
        splitMode = "horizontal";
        renderSplit();
        showTransientStatus("已切换为水平分屏（上下）。");
      }
    } else if (action === "split-close") {
      exitSplit();
      showTransientStatus("已关闭分屏。");
    } else if (action === "batch-command") {
      openBatchCommand();
    } else if (action === "close-all") {
      Promise.all(Array.from(sessions.keys()).map(closeSession));
    }
  });
  terminalOutputContextMenu.addEventListener("click", event => {
    const action = event.target.closest("button")?.dataset.action;
    const session = sessions.get(terminalOutputContextSessionId);
    closeTerminalOutputContextMenu();
    if (action === "copy") copyTerminalSelection(session);
    else if (action === "paste") pasteIntoTerminal(session);
  });
  themeButton.addEventListener("click", event => {
    event.stopPropagation();
    if (!themeMenu.hidden) {
      closeThemeMenu();
      return;
    }
    if (activeRdpVisible()) {
      // The HTML menu is behind the native RDP child. Ask the native host to
      // open a top-level themed popup at the same anchor position instead.
      closeThemeMenu();
      const anchor = themeButton.getBoundingClientRect();
      post("app.themeMenu", {
        x: Math.round(anchor.left),
        y: Math.round(anchor.bottom + 6),
        theme: activeTheme,
        preset: appSettings.terminalThemePreset || "custom"
      }).catch(reportError);
      return;
    }
    updateThemeMenuChecks();
    themeMenu.hidden = false;
    const anchor = themeButton.getBoundingClientRect();
    const menu = themeMenu.getBoundingClientRect();
    const needOpenLeft = (anchor.left + menu.width + 200 > window.innerWidth);
    themeMenu.classList.toggle("open-left", needOpenLeft);
    const left = Math.max(8, Math.min(
      anchor.left, window.innerWidth - menu.width - 8));
    const top = Math.max(8, Math.min(
      anchor.bottom + 6, window.innerHeight - menu.height - 8));
    themeMenu.style.left = `${left}px`;
    themeMenu.style.top = `${top}px`;
  });
  themeMenu.addEventListener("click", event => {
    const presetBtn = event.target.closest(".theme-preset-btn");
    if (presetBtn) {
      const theme = presetBtn.dataset.theme;
      const preset = presetBtn.dataset.preset;
      if (theme && themes[theme]) {
        applyTheme(theme);
      }
      if (preset === "custom") {
        appSettings.terminalThemePreset = "custom";
        appSettings.terminalBackground = "";
        appSettings.terminalForeground = "";
        applyTerminalSettings();
        saveSettings();
        updateThemeMenuChecks();
      } else if (preset && terminalThemePresets[preset]) {
        appSettings.terminalThemePreset = preset;
        appSettings.terminalBackground = terminalThemePresets[preset].background;
        appSettings.terminalForeground = terminalThemePresets[preset].foreground;
        applyTerminalSettings();
        saveSettings();
        updateThemeMenuChecks();
      }
      closeThemeMenu();
      return;
    }
    const catBtn = event.target.closest(".theme-menu-cat-btn");
    if (catBtn) {
      const wrap = catBtn.closest(".theme-menu-item-wrap");
      if (wrap) {
        const wasOpen = wrap.classList.contains("open");
        themeMenu.querySelectorAll(".theme-menu-item-wrap").forEach(w => w.classList.remove("open"));
        if (!wasOpen) wrap.classList.add("open");
      }
      return;
    }
  });
  themeMenu.querySelectorAll(".theme-menu-item-wrap").forEach(wrap => {
    wrap.addEventListener("mouseenter", () => {
      themeMenu.querySelectorAll(".theme-menu-item-wrap").forEach(w => {
        if (w !== wrap) w.classList.remove("open");
      });
    });
  });
  const openSettingsDialog = () => {
    if (settingsDialog.open) {
      settingsDialog.close();
      return;
    }
    populateSettingsForm();
    settingsDialog.showModal();
    loadTerminalFontFamilies();
    post("app.closeBehavior")
      .then(result => {
        const behavior = result && result.behavior;
        if (behavior === "ask" || behavior === "tray" || behavior === "exit") {
          document.querySelector("#setting-close-behavior").value = behavior;
        }
      })
      .catch(() => {});
  };

  const closeCurrentTab = () => {
    const targetSessionId = activeSession?.sessionId || focusedSessionId;
    if (targetSessionId && sessions.has(targetSessionId)) {
      closeSession(targetSessionId);
    }
  };

  settingsButton.addEventListener("click", openSettingsDialog);
  document.querySelector("#settings-dialog-close").addEventListener("click", () => settingsDialog.close());
  document.querySelector("#manage-known-hosts").addEventListener("click", manageKnownHosts);
  document.querySelector("#settings-cancel").addEventListener("click", () => settingsDialog.close());

  // ---- 端口转发 ----
  const tunnelDialog = document.querySelector("#tunnel-dialog");
  const tunnelSessionSelect = document.querySelector("#tunnel-session");
  const tunnelModeSelect = document.querySelector("#tunnel-mode");
  const tunnelListenPort = document.querySelector("#tunnel-listen-port");
  const tunnelTargetHost = document.querySelector("#tunnel-target-host");
  const tunnelTargetPort = document.querySelector("#tunnel-target-port");
  const tunnelCreateButton = document.querySelector("#tunnel-create");
  const tunnelFormStatus = document.querySelector("#tunnel-form-status");
  const tunnelSaveConfig = document.querySelector("#tunnel-save-config");
  const tunnelList = document.querySelector("#tunnel-list");

  const populateTunnelSessions = () => {
    tunnelSessionSelect.replaceChildren();
    let added = 0;
    for (const session of sessions.values()) {
      if (session.connectionType !== "ssh" || session.state !== "connected") continue;
      tunnelSessionSelect.append(new Option(
        `${session.name}（${session.sessionId}）`, session.sessionId));
      added++;
    }
    tunnelSessionSelect.disabled = added === 0;
    return added;
  };

  const renderTunnelList = tunnels => {
    tunnelList.replaceChildren();
    if (!tunnels.length) {
      const empty = document.createElement("div");
      empty.className = "tunnel-empty";
      empty.textContent = "暂无端口转发。选择已连接的 SSH 会话并填写上方表单创建。";
      tunnelList.append(empty);
      return;
    }
    const stateText = {
      listening: "监听中", starting: "创建中", failed: "失败", stopped: "已停止"
    };
    for (const tunnel of tunnels) {
      const item = document.createElement("div");
      item.className = "tunnel-item";
      const header = document.createElement("div");
      header.className = "tunnel-item-header";
      const mode = tunnel.mode === "local" ? "本地" : "远程";
      const description = document.createElement("div");
      description.textContent = tunnel.mode === "local"
        ? `${mode}转发 127.0.0.1:${tunnel.listenPort} → ${tunnel.targetHost}:${tunnel.targetPort}`
        : `${mode}转发 服务器:${tunnel.listenPort} → 本地 ${tunnel.targetHost}:${tunnel.targetPort}`;
      const badge = document.createElement("span");
      badge.className = "tunnel-state " + (tunnel.state || "starting");
      badge.textContent = stateText[tunnel.state] || tunnel.state;
      header.append(description, badge);
      if (tunnel.automatic) {
        const auto = document.createElement("span");
        auto.className = "tunnel-auto-badge";
        auto.textContent = "自动";
        auto.title = "登录该连接后自动建立";
        header.append(auto);
      }
      item.append(header);
      if (tunnel.error) {
        const error = document.createElement("div");
        error.className = "tunnel-error";
        error.textContent = tunnel.error;
        item.append(error);
      }
      const actions = document.createElement("div");
      const stopButton = document.createElement("button");
      stopButton.type = "button";
      stopButton.textContent = "停止";
      stopButton.addEventListener("click", async () => {
        try {
          await post("tunnel.stop", {
            sessionId: tunnel.sessionId, tunnelId: tunnel.tunnelId });
          await refreshTunnelList();
        } catch (error) {
          reportError(error);
        }
      });
      actions.append(stopButton);
      if (tunnel.automatic) {
        const forgetButton = document.createElement("button");
        forgetButton.type = "button";
        forgetButton.textContent = "取消自动";
        forgetButton.addEventListener("click", async () => {
          const session = sessions.get(tunnel.sessionId);
          const profileIndex = session?.profileIndex ?? -1;
          if (profileIndex < 0) return;
          try {
            await post("tunnel.removeConfig", {
              profileIndex,
              mode: tunnel.mode,
              listenHost: tunnel.mode === "local" ? "127.0.0.1" : "localhost",
              listenPort: tunnel.listenPort,
              targetHost: tunnel.targetHost,
              targetPort: tunnel.targetPort
            });
            await refreshTunnelList();
          } catch (error) {
            reportError(error);
          }
        });
        actions.append(forgetButton);
      }
      item.append(actions);
      tunnelList.append(item);
    }
  };

  const refreshTunnelList = async () => {
    try {
      renderTunnelList(await post("tunnel.list"));
    } catch (error) {
      reportError(error);
    }
  };

  const openTunnelManager = async () => {
    tunnelFormStatus.textContent = "";
    tunnelListenPort.value = "";
    tunnelTargetHost.value = "";
    tunnelTargetPort.value = "";
    tunnelSaveConfig.checked = false;
    populateTunnelSessions();
    tunnelDialog.showModal();
    await refreshTunnelList();
  };

  tunnelModeSelect.addEventListener("change", () => {
    const remote = tunnelModeSelect.value === "remote";
    tunnelTargetHost.placeholder = remote
      ? "例如 127.0.0.1（本地可达的目标）"
      : "例如 127.0.0.1 或 db.example.com";
  });

  tunnelCreateButton.addEventListener("click", async () => {
    const sessionId = tunnelSessionSelect.value;
    if (!sessionId) {
      tunnelFormStatus.textContent = "请先连接一个 SSH 会话。";
      return;
    }
    const listenPort = Number(tunnelListenPort.value || 0);
    const targetPort = Number(tunnelTargetPort.value);
    const targetHost = tunnelTargetHost.value.trim();
    if (!Number.isInteger(listenPort) || listenPort < 0 || listenPort > 65535) {
      tunnelFormStatus.textContent = "监听端口无效。";
      return;
    }
    if (!Number.isInteger(targetPort) || targetPort < 1
        || targetPort > 65535 || !targetHost) {
      tunnelFormStatus.textContent = "目标主机或端口无效。";
      return;
    }
    tunnelCreateButton.disabled = true;
    tunnelFormStatus.textContent = "正在创建…";
    try {
      const params = {
        sessionId,
        mode: tunnelModeSelect.value,
        listenPort,
        targetHost,
        targetPort
      };
      if (tunnelSaveConfig.checked) {
        const session = sessions.get(sessionId);
        const profileIndex = session?.profileIndex ?? -1;
        if (profileIndex >= 0) {
          params.saveConfig = true;
          params.profileIndex = profileIndex;
        }
      }
      await post("tunnel.create", params);
      tunnelFormStatus.textContent = "已创建。";
      tunnelListenPort.value = "";
      tunnelTargetHost.value = "";
      tunnelTargetPort.value = "";
      tunnelSaveConfig.checked = false;
      await refreshTunnelList();
    } catch (error) {
      tunnelFormStatus.textContent = error instanceof Error ? error.message : String(error);
    } finally {
      tunnelCreateButton.disabled = false;
    }
  });

  document.querySelector("#tunnel-button").addEventListener("click", openTunnelManager);
  document.querySelector("#tunnel-dialog-close").addEventListener("click", () => tunnelDialog.close());
  document.querySelector("#tunnel-dialog-close-action").addEventListener("click", () => tunnelDialog.close());
  document.querySelector("#remote-preview-close").addEventListener("click", () => remotePreviewDialog.close());
  document.querySelector("#remote-preview-close-action").addEventListener("click", () => remotePreviewDialog.close());
  document.querySelector("#tunnel-refresh").addEventListener("click", refreshTunnelList);

  // ---- 侧边栏功能面板扩展：常用命令、端口转发、云备份 ----

  // --- 1. 侧边栏常用运维命令面板 (Snippets Panel) ---
  const customSnippetsStorageKey = "masterterm.customSnippets";
  const readCustomSnippets = () => {
    try {
      const val = JSON.parse(localStorage.getItem(customSnippetsStorageKey) || "[]");
      return Array.isArray(val) ? val : [];
    } catch {
      return [];
    }
  };
  const saveCustomSnippets = snippets => {
    localStorage.setItem(customSnippetsStorageKey, JSON.stringify(snippets));
  };

  const getSnippetCategory = item => {
    if (item.custom) return "custom";
    const cmd = (item.cmd || "").toLowerCase();
    if (["tar", "zip", "unzip", "gzip", "bzip2"].includes(cmd)) return "package";
    if (cmd === "find") return "find";
    if (["grep", "awk", "sed"].includes(cmd)) return "search";
    if (["top", "htop", "vmstat", "iostat", "free", "df", "ps", "uptime"].includes(cmd)) return "system";
    if (["netstat", "ss", "lsof", "curl", "tcpdump", "ping", "traceroute", "dig", "nc"].includes(cmd)) return "network";
    if (["systemctl", "journalctl", "service", "kill", "killall", "nohup"].includes(cmd)) return "service";
    if (cmd.startsWith("docker")) return "docker";
    return "system";
  };

  const getCategoryLabel = cat => {
    const map = {
      package: "打包解压",
      find: "查找定位",
      search: "文本检索",
      system: "系统监控",
      network: "网络端口",
      service: "服务进程",
      docker: "Docker容器",
      custom: "我的自定义"
    };
    return map[cat] || "常用运维";
  };

  const sendCommandToActiveTerminal = (cmd, execute = true) => {
    let targetSession = sessions.get(focusedSessionId);
    if (!targetSession || targetSession.state !== "connected") {
      targetSession = Array.from(sessions.values()).find(s => s.state === "connected");
    }
    if (!targetSession) {
      showTransientStatus("当前没有已连接的终端会话，请先连接或打开终端。");
      return;
    }
    focusSession(targetSession.sessionId);
    const data = execute ? (cmd.endsWith("\r") || cmd.endsWith("\n") ? cmd : cmd + "\r") : cmd;
    post("session.input", { sessionId: targetSession.sessionId, data }).catch(reportError);
    targetSession.terminal?.focus();
    showTransientStatus(execute ? `已在终端中执行命令` : `已插入指令到终端，可按需修改后回车`);
  };

  renderSnippetsPanel = () => {
    const listEl = document.querySelector("#snippets-list");
    const emptyEl = document.querySelector("#snippets-empty");
    const searchInput = document.querySelector("#snippet-search-input");
    const categorySelect = document.querySelector("#snippet-category-select");
    if (!listEl) return;

    const query = (searchInput?.value || "").trim().toLowerCase();
    const categoryFilter = categorySelect?.value || "all";

    const customSnippets = readCustomSnippets();
    const allSnippets = [
      ...customSnippets.map((s, idx) => ({ ...s, custom: true, customIndex: idx })),
      ...(typeof BUILTIN_COMMAND_TEMPLATES !== "undefined" ? BUILTIN_COMMAND_TEMPLATES : [])
    ];

    const filtered = allSnippets.filter(item => {
      const cat = getSnippetCategory(item);
      if (categoryFilter !== "all" && cat !== categoryFilter) return false;
      if (!query) return true;
      const text = `${item.cmd || ""} ${item.desc || ""} ${item.value || ""}`.toLowerCase();
      return text.includes(query);
    });

    listEl.replaceChildren();
    if (emptyEl) emptyEl.hidden = filtered.length > 0;

    filtered.forEach(item => {
      const card = document.createElement("div");
      card.className = "snippet-item";

      const header = document.createElement("div");
      header.className = "snippet-header";

      const desc = document.createElement("span");
      desc.className = "snippet-desc";
      desc.textContent = item.desc || item.cmd || "运维指令";
      desc.title = item.desc || "";

      const catBadge = document.createElement("span");
      catBadge.className = "snippet-category-badge";
      catBadge.textContent = getCategoryLabel(getSnippetCategory(item));

      header.append(desc, catBadge);

      const codeBox = document.createElement("div");
      codeBox.className = "snippet-code";
      codeBox.textContent = item.value || "";

      const actions = document.createElement("div");
      actions.className = "snippet-actions";

      const runBtn = document.createElement("button");
      runBtn.type = "button";
      runBtn.className = "snippet-action-btn run-btn";
      runBtn.textContent = "🚀 执行";
      runBtn.title = "直接发送到当前终端并回车执行";
      runBtn.addEventListener("click", () => sendCommandToActiveTerminal(item.value, true));

      const insertBtn = document.createElement("button");
      insertBtn.type = "button";
      insertBtn.className = "snippet-action-btn";
      insertBtn.textContent = "📝 插入";
      insertBtn.title = "插入到终端输入区，方便修改参数后再回车";
      insertBtn.addEventListener("click", () => sendCommandToActiveTerminal(item.value, false));

      const copyBtn = document.createElement("button");
      copyBtn.type = "button";
      copyBtn.className = "snippet-action-btn";
      copyBtn.textContent = "📋 复制";
      copyBtn.title = "复制命令到剪贴板";
      copyBtn.addEventListener("click", () => {
        navigator.clipboard.writeText(item.value).then(() => {
          copyBtn.textContent = "✓ 已复制";
          setTimeout(() => { copyBtn.textContent = "📋 复制"; }, 1500);
        }).catch(() => {
          showTransientStatus("复制失败，请手动选择复制。");
        });
      });

      actions.append(runBtn, insertBtn, copyBtn);

      if (item.custom) {
        const delBtn = document.createElement("button");
        delBtn.type = "button";
        delBtn.className = "snippet-action-btn";
        delBtn.textContent = "🗑️ 删除";
        delBtn.title = "删除此自定义命令";
        delBtn.addEventListener("click", () => {
          const list = readCustomSnippets().filter((_, i) => i !== item.customIndex);
          saveCustomSnippets(list);
          renderSnippetsPanel();
          showTransientStatus("已删除自定义命令");
        });
        actions.append(delBtn);
      }

      card.append(header, codeBox, actions);
      listEl.appendChild(card);
    });
  };

  const initSnippetsPanel = () => {
    const searchInput = document.querySelector("#snippet-search-input");
    const categorySelect = document.querySelector("#snippet-category-select");
    const addBtn = document.querySelector("#snippet-add-btn");
    const formEl = document.querySelector("#snippet-custom-form");
    const saveBtn = document.querySelector("#snippet-form-save");
    const cancelBtn = document.querySelector("#snippet-form-cancel");
    const descInput = document.querySelector("#snippet-form-desc");
    const cmdInput = document.querySelector("#snippet-form-cmd");

    searchInput?.addEventListener("input", renderSnippetsPanel);
    categorySelect?.addEventListener("change", renderSnippetsPanel);

    addBtn?.addEventListener("click", () => {
      if (!formEl) return;
      formEl.hidden = !formEl.hidden;
      if (!formEl.hidden) descInput?.focus();
    });

    cancelBtn?.addEventListener("click", () => {
      if (!formEl) return;
      formEl.hidden = true;
      if (descInput) descInput.value = "";
      if (cmdInput) cmdInput.value = "";
    });

    saveBtn?.addEventListener("click", () => {
      const desc = (descInput?.value || "").trim();
      const cmd = (cmdInput?.value || "").trim();
      if (!cmd) {
        showTransientStatus("命令内容不能为空");
        cmdInput?.focus();
        return;
      }
      const list = readCustomSnippets();
      list.unshift({ desc: desc || cmd, value: cmd, cmd: cmd.split(/\s+/)[0] || "custom", custom: true });
      saveCustomSnippets(list);
      if (formEl) formEl.hidden = true;
      if (descInput) descInput.value = "";
      if (cmdInput) cmdInput.value = "";
      renderSnippetsPanel();
      showTransientStatus("自定义命令已保存");
    });

    renderSnippetsPanel();
  };

  // --- 2. 侧边栏端口转发面板 (Tunnels Panel) ---
  const savedTunnelsStorageKey = "masterterm.savedTunnels";
  const readSavedTunnels = () => {
    try {
      const val = JSON.parse(localStorage.getItem(savedTunnelsStorageKey) || "[]");
      return Array.isArray(val) ? val : [];
    } catch {
      return [];
    }
  };
  const saveSavedTunnels = list => {
    try {
      localStorage.setItem(savedTunnelsStorageKey, JSON.stringify(list));
    } catch (err) {
      console.warn("saveSavedTunnels error:", err);
    }
  };
  let sidebarTunnelsCache = [];

  refreshSidebarTunnelSessions = () => {
    const tunnelSessionSelect = document.querySelector("#sidebar-tunnel-session");
    if (!tunnelSessionSelect) return;
    const available = connectedSftpSessions();
    const prev = tunnelSessionSelect.value;
    tunnelSessionSelect.replaceChildren();
    if (!available.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "没有已连接的 SSH 会话";
      tunnelSessionSelect.appendChild(option);
      tunnelSessionSelect.disabled = true;
    } else {
      available.forEach(session => {
        const profile = profilesByIndex.get(session.profileIndex);
        const option = document.createElement("option");
        option.value = session.sessionId;
        option.textContent = profile?.name || session.name
          || profile?.address || "SSH 会话";
        tunnelSessionSelect.appendChild(option);
      });
      tunnelSessionSelect.disabled = false;
      const match = available.some(s => s.sessionId === prev);
      if (match) {
        tunnelSessionSelect.value = prev;
      } else {
        const preferred = available.some(s => s.sessionId === focusedSessionId)
          ? focusedSessionId : available[0].sessionId;
        tunnelSessionSelect.value = preferred;
      }
    }
  };

  refreshTunnelsPanel = async () => {
    refreshSidebarTunnelSessions();
    const listEl = document.querySelector("#sidebar-tunnels-list");
    const emptyEl = document.querySelector("#sidebar-tunnels-empty");
    const badgeEl = document.querySelector("#sidebar-tunnel-badge");
    try {
      const tunnels = await post("tunnel.list");
      const runningTunnels = Array.isArray(tunnels) ? tunnels : [];
      let savedList = readSavedTunnels();

      // 将后端正在运行但不在已保存列表中的通道同步记录
      runningTunnels.forEach(rt => {
        const existing = savedList.find(s =>
          (s.activeTunnelId && (s.activeTunnelId === rt.id || s.activeTunnelId === rt.tunnelId)) ||
          (s.sessionId === rt.sessionId && s.mode === rt.mode && s.listenPort === rt.listenPort && s.targetPort === rt.targetPort)
        );
        if (existing) {
          existing.activeTunnelId = rt.id || rt.tunnelId;
          existing.status = "listening";
        } else {
          const session = sessions.get(rt.sessionId);
          const profile = session && profilesByIndex.get(session.profileIndex);
          savedList.push({
            id: "tunnel_" + Date.now() + "_" + Math.random().toString(36).slice(2, 7),
            sessionId: rt.sessionId,
            sessionName: profile?.name || session?.name || rt.sessionId || "SSH",
            mode: rt.mode || "local",
            listenPort: rt.listenPort,
            targetPort: rt.targetPort,
            targetHost: rt.targetHost || "127.0.0.1",
            listenHost: rt.listenHost || "127.0.0.1",
            activeTunnelId: rt.id || rt.tunnelId,
            status: "listening"
          });
        }
      });

      // 根据后端运行列表校准每个通道状态
      savedList.forEach(rule => {
        const isRunning = runningTunnels.some(rt =>
          (rule.activeTunnelId && (rt.id === rule.activeTunnelId || rt.tunnelId === rule.activeTunnelId)) ||
          (rt.sessionId === rule.sessionId && rt.mode === rule.mode && rt.listenPort === rule.listenPort && rt.targetPort === rule.targetPort)
        );
        if (!isRunning) {
          rule.activeTunnelId = null;
          rule.status = "stopped";
        }
      });
      saveSavedTunnels(savedList);

      const activeCount = savedList.filter(t => t.status === "listening" || t.status === "active").length;
      if (badgeEl) {
        badgeEl.textContent = String(activeCount);
        badgeEl.hidden = activeCount === 0;
      }
      if (!listEl) return;
      listEl.replaceChildren();
      if (emptyEl) emptyEl.hidden = savedList.length > 0;

      savedList.forEach((tunnel, ruleIndex) => {
        const card = document.createElement("div");
        card.className = "tunnel-card";

        const header = document.createElement("div");
        header.className = "tunnel-card-header";

        const route = document.createElement("span");
        route.className = "tunnel-route";
        const isLocal = tunnel.mode === "local";
        route.textContent = isLocal
          ? `[本地] :${tunnel.listenPort} ➔ ${tunnel.targetHost}:${tunnel.targetPort}`
          : `[远端] :${tunnel.listenPort} ➔ ${tunnel.targetHost}:${tunnel.targetPort}`;

        const isRunning = tunnel.status === "listening" || tunnel.status === "active";
        const badge = document.createElement("span");
        badge.className = `tunnel-badge ${isRunning ? "listening" : "stopped"}`;
        badge.textContent = isRunning ? "监听中" : "已停止";

        header.append(route, badge);

        const meta = document.createElement("div");
        meta.className = "tunnel-meta";

        let sessionName = tunnel.sessionName || "SSH";
        if (tunnel.sessionId && sessions.has(tunnel.sessionId)) {
          const s = sessions.get(tunnel.sessionId);
          const p = s && profilesByIndex.get(s.profileIndex);
          sessionName = p?.name || s?.name || sessionName;
        }
        const metaText = document.createElement("span");
        metaText.textContent = `会话: ${sessionName}`;

        const actions = document.createElement("div");
        actions.className = "tunnel-actions";

        if (isRunning) {
          const stopBtn = document.createElement("button");
          stopBtn.type = "button";
          stopBtn.className = "tunnel-stop-btn";
          stopBtn.textContent = "停止";
          stopBtn.addEventListener("click", async () => {
            stopBtn.disabled = true;
            try {
              if (tunnel.activeTunnelId && tunnel.sessionId) {
                await post("tunnel.stop", { sessionId: tunnel.sessionId, tunnelId: tunnel.activeTunnelId });
              }
              const currentList = readSavedTunnels();
              const target = currentList.find(s => s.id === tunnel.id) || currentList[ruleIndex];
              if (target) {
                target.activeTunnelId = null;
                target.status = "stopped";
                saveSavedTunnels(currentList);
              }
              showTransientStatus("端口转发已停止");
              await refreshTunnelsPanel();
            } catch (err) {
              reportError(err);
            } finally {
              stopBtn.disabled = false;
            }
          });
          actions.appendChild(stopBtn);
        } else {
          const startBtn = document.createElement("button");
          startBtn.type = "button";
          startBtn.className = "tunnel-start-btn";
          startBtn.textContent = "启动";
          startBtn.addEventListener("click", async () => {
            startBtn.disabled = true;
            try {
              let targetSessionId = tunnel.sessionId;
              if (!sessions.has(targetSessionId) || sessions.get(targetSessionId)?.state !== "connected") {
                const connected = connectedSftpSessions();
                const matched = connected.find(s => {
                  const p = profilesByIndex.get(s.profileIndex);
                  return (p?.name && p.name === tunnel.sessionName) || (s.name === tunnel.sessionName);
                }) || connected[0];
                if (!matched) {
                  showTransientStatus("未找到可用的已连接 SSH 会话，请先连接 SSH");
                  return;
                }
                targetSessionId = matched.sessionId;
              }
              const result = await post("tunnel.create", {
                sessionId: targetSessionId,
                mode: tunnel.mode,
                listenPort: tunnel.listenPort,
                targetPort: tunnel.targetPort,
                targetHost: tunnel.targetHost,
                listenHost: tunnel.listenHost
              });
              const currentList = readSavedTunnels();
              const target = currentList.find(s => s.id === tunnel.id) || currentList[ruleIndex];
              if (target) {
                target.sessionId = targetSessionId;
                target.activeTunnelId = result?.id || result?.tunnelId || null;
                target.status = "listening";
                saveSavedTunnels(currentList);
              }
              showTransientStatus(`端口转发已建立：${tunnel.listenPort} ➔ ${tunnel.targetHost}:${tunnel.targetPort}`);
              await refreshTunnelsPanel();
            } catch (err) {
              showTransientStatus(err instanceof Error ? err.message : String(err));
            } finally {
              startBtn.disabled = false;
            }
          });

          const delBtn = document.createElement("button");
          delBtn.type = "button";
          delBtn.className = "tunnel-delete-btn";
          delBtn.textContent = "删除";
          delBtn.addEventListener("click", async () => {
            delBtn.disabled = true;
            try {
              const currentList = readSavedTunnels().filter(s => s.id !== tunnel.id);
              saveSavedTunnels(currentList);
              showTransientStatus("端口转发规则已删除");
              await refreshTunnelsPanel();
            } catch (err) {
              reportError(err);
            }
          });

          actions.append(startBtn, delBtn);
        }

        meta.append(metaText, actions);
        card.append(header, meta);
        listEl.appendChild(card);
      });
    } catch (err) {
      console.warn("refreshTunnelsPanel error:", err);
    }
  };

  const initTunnelsPanel = () => {
    const toggleBtn = document.querySelector("#sidebar-tunnel-create-toggle");
    const refreshBtn = document.querySelector("#sidebar-tunnel-refresh-btn");
    const formEl = document.querySelector("#sidebar-tunnel-create-form");
    const cancelBtn = document.querySelector("#sidebar-tunnel-cancel-btn");
    const submitBtn = document.querySelector("#sidebar-tunnel-submit-btn");
    const modeSelect = document.querySelector("#sidebar-tunnel-mode");
    const listenPortInput = document.querySelector("#sidebar-tunnel-listen-port");
    const targetPortInput = document.querySelector("#sidebar-tunnel-target-port");
    const targetHostInput = document.querySelector("#sidebar-tunnel-target-host");
    const listenHostInput = document.querySelector("#sidebar-tunnel-listen-host");
    const listenPortLabel = document.querySelector("#sidebar-tunnel-listen-port-label");
    const targetPortLabel = document.querySelector("#sidebar-tunnel-target-port-label");

    toggleBtn?.addEventListener("click", () => {
      if (!formEl) return;
      formEl.hidden = !formEl.hidden;
      if (!formEl.hidden) {
        refreshSidebarTunnelSessions();
        listenPortInput?.focus();
      }
    });

    cancelBtn?.addEventListener("click", () => {
      if (formEl) formEl.hidden = true;
    });

    refreshBtn?.addEventListener("click", async () => {
      refreshBtn.classList.add("refreshing");
      try {
        await refreshTunnelsPanel();
        showTransientStatus("已刷新端口转发通道");
      } finally {
        setTimeout(() => refreshBtn.classList.remove("refreshing"), 600);
      }
    });

    modeSelect?.addEventListener("change", () => {
      const isRemote = modeSelect.value === "remote";
      if (listenPortLabel) listenPortLabel.textContent = isRemote ? "远程监听端口" : "本地监听端口";
      if (targetPortLabel) targetPortLabel.textContent = isRemote ? "本地目标端口" : "远程目标端口";
    });

    submitBtn?.addEventListener("click", async () => {
      const sessionSelect = document.querySelector("#sidebar-tunnel-session");
      const sessionId = sessionSelect?.value;
      if (!sessionId) {
        showTransientStatus("请先选择一个已连接的 SSH 会话");
        return;
      }
      const mode = modeSelect?.value || "local";
      const listenPort = Number(listenPortInput?.value || 0);
      const targetPort = Number(targetPortInput?.value || 0);
      const targetHost = (targetHostInput?.value || "127.0.0.1").trim();
      const listenHost = (listenHostInput?.value || "127.0.0.1").trim();

      if (!Number.isInteger(listenPort) || listenPort < 1 || listenPort > 65535) {
        showTransientStatus("监听端口无效，请输入 1-65535 之间的端口号");
        listenPortInput?.focus();
        return;
      }
      if (!Number.isInteger(targetPort) || targetPort < 1 || targetPort > 65535) {
        showTransientStatus("目标端口无效，请输入 1-65535 之间的端口号");
        targetPortInput?.focus();
        return;
      }

      submitBtn.disabled = true;
      try {
        const result = await post("tunnel.create", {
          sessionId,
          mode,
          listenPort,
          targetPort,
          targetHost,
          listenHost
        });
        const session = sessions.get(sessionId);
        const profile = session && profilesByIndex.get(session.profileIndex);
        const sessionName = profile?.name || session?.name || "SSH";
        const currentList = readSavedTunnels();
        currentList.unshift({
          id: "tunnel_" + Date.now() + "_" + Math.random().toString(36).slice(2, 7),
          sessionId,
          sessionName,
          mode,
          listenPort,
          targetPort,
          targetHost,
          listenHost,
          activeTunnelId: result?.id || result?.tunnelId || null,
          status: "listening"
        });
        saveSavedTunnels(currentList);

        showTransientStatus(`端口转发已建立：${listenPort} ➔ ${targetHost}:${targetPort}`);
        if (formEl) formEl.hidden = true;
        if (listenPortInput) listenPortInput.value = "";
        if (targetPortInput) targetPortInput.value = "";
        await refreshTunnelsPanel();
      } catch (err) {
        showTransientStatus(err instanceof Error ? err.message : String(err));
      } finally {
        submitBtn.disabled = false;
      }
    });

    refreshTunnelsPanel();
  };

  // --- 3. 侧边栏云备份面板 (Cloud Sidebar Panel) ---
  const loadSidebarCloudHistory = async () => {
    const listEl = document.querySelector("#cloud-sidebar-history-list");
    const emptyEl = document.querySelector("#cloud-sidebar-history-empty");
    if (!listEl || !cloudState.token) return;
    try {
      const url = cloudState.url || cloudSyncDefaultUrl;
      const result = await post("cloud.history", { url, token: cloudState.token });
      const snapshots = Array.isArray(result) ? result : [];
      listEl.replaceChildren();
      if (emptyEl) emptyEl.hidden = snapshots.length > 0;

      snapshots.slice(0, 5).forEach(snapshot => {
        const item = document.createElement("div");
        item.className = "cloud-history-item";

        const meta = document.createElement("div");
        const title = document.createElement("div");
        title.className = "cloud-history-title";
        title.textContent = snapshot.name || `快照版本 #${snapshot.seq}`;

        const date = document.createElement("div");
        date.className = "cloud-history-date";
        date.textContent = snapshot.updatedAt || "";

        meta.append(title, date);

        const restoreBtn = document.createElement("button");
        restoreBtn.type = "button";
        restoreBtn.className = "cloud-btn-link";
        restoreBtn.textContent = "恢复";
        restoreBtn.addEventListener("click", async () => {
          if (!await requestConfirmation("确认恢复快照", `确定恢复快照版本 #${snapshot.seq} 吗？`)) return;
          try {
            await post("cloud.restore", { url, token: cloudState.token, seq: snapshot.seq });
            showTransientStatus("云端版本恢复成功，正在拉取最新数据…");
            cloudPull();
          } catch (err) {
            reportError(err);
          }
        });

        item.append(meta, restoreBtn);
        listEl.appendChild(item);
      });
    } catch {
      if (emptyEl) emptyEl.hidden = false;
    }
  };

  updateSidebarCloudUi = () => {
    const loggedIn = Boolean(cloudState.token);
    const loggedInCard = document.querySelector("#cloud-sidebar-logged-in");
    const guestCard = document.querySelector("#cloud-sidebar-guest");
    const usernameEl = document.querySelector("#cloud-sidebar-username");
    const syncTimeEl = document.querySelector("#cloud-sidebar-sync-time");

    if (loggedInCard) loggedInCard.hidden = !loggedIn;
    if (guestCard) guestCard.hidden = loggedIn;

    if (loggedIn) {
      if (usernameEl) usernameEl.textContent = cloudState.username || "已登录用户";
      if (syncTimeEl) {
        syncTimeEl.textContent = cloudState.updatedAt
          ? new Date(cloudState.updatedAt).toLocaleString()
          : "尚未同步";
      }
      loadSidebarCloudHistory();
    }
  };

  const initCloudSidebarPanel = () => {
    const loginBtn = document.querySelector("#cloud-sidebar-login-btn");
    const registerBtn = document.querySelector("#cloud-sidebar-register-btn");
    const logoutBtn = document.querySelector("#cloud-sidebar-logout");
    const pushBtn = document.querySelector("#cloud-sidebar-push");
    const pullBtn = document.querySelector("#cloud-sidebar-pull");
    const historyRefreshBtn = document.querySelector("#cloud-sidebar-history-refresh");
    const exportLocalBtn = document.querySelector("#cloud-sidebar-export-local");
    const importLocalBtn = document.querySelector("#cloud-sidebar-import-local");
    const userInput = document.querySelector("#cloud-sidebar-input-user");
    const pwdInput = document.querySelector("#cloud-sidebar-input-pwd");
    const guestStatus = document.querySelector("#cloud-sidebar-guest-status");

    loginBtn?.addEventListener("click", async () => {
      const username = (userInput?.value || "").trim();
      const password = pwdInput?.value || "";
      if (!username || !password) {
        if (guestStatus) guestStatus.textContent = "请输入用户名和密码";
        return;
      }
      if (guestStatus) guestStatus.textContent = "正在登录…";
      const url = cloudState.url || cloudSyncDefaultUrl;
      try {
        const result = await post("cloud.login", { url, username, password });
        cloudState = { url, username, token: result.token, updatedAt: "", passwordLength: password.length };
        saveCloudState();
        updateSidebarCloudUi();
        if (guestStatus) guestStatus.textContent = "";
        if (pwdInput) pwdInput.value = "";
        showTransientStatus("云端登录成功");
      } catch (err) {
        if (guestStatus) guestStatus.textContent = err instanceof Error ? err.message : String(err);
      }
    });

    registerBtn?.addEventListener("click", async () => {
      const username = (userInput?.value || "").trim();
      const password = pwdInput?.value || "";
      if (!username || !password) {
        if (guestStatus) guestStatus.textContent = "请输入用户名和密码";
        return;
      }
      if (guestStatus) guestStatus.textContent = "正在注册…";
      const url = cloudState.url || cloudSyncDefaultUrl;
      try {
        const result = await post("cloud.register", { url, username, password });
        cloudState = { url, username, token: result.token, updatedAt: "", passwordLength: password.length };
        saveCloudState();
        updateSidebarCloudUi();
        if (guestStatus) guestStatus.textContent = "";
        if (pwdInput) pwdInput.value = "";
        showTransientStatus("云端注册并登录成功");
      } catch (err) {
        if (guestStatus) guestStatus.textContent = err instanceof Error ? err.message : String(err);
      }
    });

    logoutBtn?.addEventListener("click", () => {
      cloudState = { url: cloudState.url, username: "", token: "", updatedAt: "" };
      saveCloudState();
      updateSidebarCloudUi();
      showTransientStatus("已退出云端账号");
    });

    pushBtn?.addEventListener("click", cloudPush);
    pullBtn?.addEventListener("click", cloudPull);
    historyRefreshBtn?.addEventListener("click", loadSidebarCloudHistory);

    exportLocalBtn?.addEventListener("click", async () => {
      try {
        const exported = await post("cloud.exportServers");
        const payload = {
          version: 2,
          savedAt: new Date().toISOString(),
          servers: exported,
          workspaces: workspaceNames || []
        };
        const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json;charset=utf-8" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = `masterterm-backup-${new Date().toISOString().slice(0, 10)}.json`;
        a.click();
        URL.revokeObjectURL(url);
        showTransientStatus("本地备份文件已导出");
      } catch (err) {
        reportError(err);
      }
    });

    importLocalBtn?.addEventListener("click", () => {
      const fileInput = document.createElement("input");
      fileInput.type = "file";
      fileInput.accept = ".json,application/json";
      fileInput.addEventListener("change", async () => {
        const file = fileInput.files?.[0];
        if (!file) return;
        try {
          const text = await file.text();
          const parsed = JSON.parse(text);
          const servers = Array.isArray(parsed) ? parsed : (parsed.servers || []);
          const workspaces = parsed.workspaces || [];
          if (!servers.length) {
            showTransientStatus("备份文件中未找到有效的连接配置");
            return;
          }
          await post("cloud.importServers", { servers, workspaces });
          const refreshed = await post("server.list");
          render(refreshed);
          showTransientStatus(`已从本地备份导入 ${servers.length} 个连接配置！`);
        } catch (err) {
          reportError(err);
        }
      });
      fileInput.click();
    });

    updateSidebarCloudUi();
  };
  document.querySelector("#settings-reset").addEventListener("click", () => {
    const currentSettings = appSettings;
    appSettings = { ...defaultSettings };
    populateSettingsForm();
    appSettings = currentSettings;
    settingsStatus.textContent = "已恢复默认值，保存后生效。";
  });
  ["#setting-font-size", "#setting-font-family", "#setting-terminal-background",
    "#setting-terminal-foreground"].forEach(selector => {
    const input = document.querySelector(selector);
    input.addEventListener("input", updateTerminalSettingsPreview);
    input.addEventListener("change", updateTerminalSettingsPreview);
  });
  settingsForm.addEventListener("submit", event => {
    event.preventDefault();
    saveSettingsFromForm();
    settingsDialog.close();
  });
  document.querySelector("#settings-reset-connection-colors")?.addEventListener("click", () => {
    document.querySelector("#setting-connection-color-ssh").value = defaultSettings.connectionTypeColorSsh;
    document.querySelector("#setting-connection-color-rdp").value = defaultSettings.connectionTypeColorRdp;
    document.querySelector("#setting-connection-color-serial").value = defaultSettings.connectionTypeColorSerial;
    settingsStatus.textContent = "已恢复连接类型默认颜色，保存后生效。";
  });
  const closeServerViewMenu = () => {
    if (!serverViewMenu?.hidden) serverViewMenu.hidden = true;
    serverViewOptions?.setAttribute("aria-expanded", "false");
  };
  const persistServerViewSettings = () => {
    localStorage.setItem("masterterm.settings", JSON.stringify(appSettings));
    render(profilesCache);
  };
  serverViewOptions?.addEventListener("click", event => {
    event.stopPropagation();
    if (!serverViewMenu) return;
    serverViewMenu.hidden = !serverViewMenu.hidden;
    serverViewOptions.setAttribute("aria-expanded", String(!serverViewMenu.hidden));
    if (!serverViewMenu.hidden) {
      serverSortMode.value = appSettings.serverSortMode;
      serverGroupMode.value = appSettings.serverGroupMode;
      const bounds = serverViewOptions.getBoundingClientRect();
      serverViewMenu.style.left = `${bounds.left}px`;
      serverViewMenu.style.top = `${bounds.bottom + 6}px`;
      const menuBounds = serverViewMenu.getBoundingClientRect();
      serverViewMenu.style.left = `${Math.max(6, Math.min(bounds.left, window.innerWidth - menuBounds.width - 6))}px`;
      serverViewMenu.style.top = `${Math.max(6, Math.min(bounds.bottom + 6, window.innerHeight - menuBounds.height - 6))}px`;
    }
  });
  serverSortMode?.addEventListener("change", () => {
    appSettings = { ...appSettings, serverSortMode: serverSortMode.value };
    persistServerViewSettings();
  });
  serverGroupMode?.addEventListener("change", () => {
    appSettings = { ...appSettings, serverGroupMode: serverGroupMode.value };
    persistServerViewSettings();
  });
  document.addEventListener("pointerdown", event => {
    if (!serverViewMenu?.hidden && !serverViewMenu.contains(event.target)
        && event.target !== serverViewOptions)
      closeServerViewMenu();
    if (!serverContextMenu.hidden && !serverContextMenu.contains(event.target)) {
      closeServerContextMenu();
    }
    if (!workspaceContextMenu.hidden && !workspaceContextMenu.contains(event.target)) {
      workspaceContextMenu.hidden = true;
      contextMenuWorkspace = "";
    }
    if (!terminalContextMenu.hidden && !terminalContextMenu.contains(event.target))
      closeTerminalContextMenu();
    if (!splitNavContextMenu.hidden && !splitNavContextMenu.contains(event.target))
      closeSplitNavContextMenu();
    if (!terminalOutputContextMenu.hidden && !terminalOutputContextMenu.contains(event.target))
      closeTerminalOutputContextMenu();
    if (!themeMenu.hidden && !themeMenu.contains(event.target) && event.target !== themeButton)
      closeThemeMenu();
  });
  document.addEventListener("contextmenu", event => {
    if (!event.target.closest("#server-view-menu") && event.target !== serverViewOptions)
      closeServerViewMenu();
    if (!event.target.closest(".server"))
      closeServerContextMenu();
    if (!event.target.closest(".terminal-tab")) {
      closeTerminalContextMenu();
      closeSplitNavContextMenu();
    }
    if (!event.target.closest(".terminal-output"))
      closeTerminalOutputContextMenu();
    if (!event.target.closest("#theme-menu") && event.target !== themeButton)
      closeThemeMenu();
  });
  document.addEventListener("scroll", closeServerContextMenu, true);
  document.addEventListener("scroll", closeServerViewMenu, true);
  document.addEventListener("scroll", closeTerminalContextMenu, true);
  document.addEventListener("scroll", closeSplitNavContextMenu, true);
  document.addEventListener("scroll", closeTerminalOutputContextMenu, true);
  document.addEventListener("scroll", closeThemeMenu, true);
  window.addEventListener("blur", closeServerContextMenu);
  window.addEventListener("blur", closeServerViewMenu);
  window.addEventListener("blur", closeTerminalContextMenu);
  window.addEventListener("blur", closeSplitNavContextMenu);
  window.addEventListener("blur", closeTerminalOutputContextMenu);
  window.addEventListener("blur", closeThemeMenu);
  window.addEventListener("blur", hideRdpTabHover);
  window.addEventListener("resize", closeServerContextMenu);
  window.addEventListener("resize", closeServerViewMenu);
  window.addEventListener("resize", closeTerminalContextMenu);
  window.addEventListener("resize", closeSplitNavContextMenu);
  window.addEventListener("resize", closeTerminalOutputContextMenu);
  window.addEventListener("resize", closeThemeMenu);
  window.addEventListener("resize", hideRdpTabHover);
  document.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      if (rdpFullscreen && !document.querySelector("dialog[open]"))
        requestRdpFullscreen(false).catch(reportError);
      closeServerContextMenu();
      closeTerminalContextMenu();
      closeSplitNavContextMenu();
      closeTerminalOutputContextMenu();
      closeThemeMenu();
      hideRdpTabHover();
    }
  });
  serverType.addEventListener("change", () => {
    const port = document.querySelector("#server-port");
    port.value = serverType.value === "serial" ? "115200" : "22";
    document.querySelector("#server-tag-mode").value = "auto";
    updateServerTagMode();
    updateServerFormType();
    if (serverType.value === "serial") refreshSerialPorts();
  });
  document.querySelector("#server-tag-mode")?.addEventListener("change", updateServerTagMode);
  const applyRdpQualityPreset = preset => {
    if (preset === "high") {
      if (rdpOptionFields.colorDepth) rdpOptionFields.colorDepth.value = "32";
      if (rdpOptionFields.networkConnectionType) rdpOptionFields.networkConnectionType.value = "6";
      if (rdpOptionFields.performanceFlags) rdpOptionFields.performanceFlags.value = "0";
      if (rdpOptionFields.desktopBackground) rdpOptionFields.desktopBackground.checked = true;
      if (rdpOptionFields.fontSmoothing) rdpOptionFields.fontSmoothing.checked = true;
      if (rdpOptionFields.desktopComposition) rdpOptionFields.desktopComposition.checked = true;
      if (rdpOptionFields.fullWindowDrag) rdpOptionFields.fullWindowDrag.checked = true;
      if (rdpOptionFields.menuAnimations) rdpOptionFields.menuAnimations.checked = true;
      if (rdpOptionFields.visualStyles) rdpOptionFields.visualStyles.checked = true;
      if (rdpOptionFields.cursorShadow) rdpOptionFields.cursorShadow.checked = true;
      if (rdpOptionFields.cursorSettings) rdpOptionFields.cursorSettings.checked = true;
    } else if (preset === "balanced") {
      if (rdpOptionFields.colorDepth) rdpOptionFields.colorDepth.value = "24";
      if (rdpOptionFields.networkConnectionType) rdpOptionFields.networkConnectionType.value = "4";
      if (rdpOptionFields.performanceFlags) rdpOptionFields.performanceFlags.value = "0";
      if (rdpOptionFields.desktopBackground) rdpOptionFields.desktopBackground.checked = false;
      if (rdpOptionFields.fontSmoothing) rdpOptionFields.fontSmoothing.checked = true;
      if (rdpOptionFields.desktopComposition) rdpOptionFields.desktopComposition.checked = true;
      if (rdpOptionFields.fullWindowDrag) rdpOptionFields.fullWindowDrag.checked = false;
      if (rdpOptionFields.menuAnimations) rdpOptionFields.menuAnimations.checked = false;
      if (rdpOptionFields.visualStyles) rdpOptionFields.visualStyles.checked = true;
      if (rdpOptionFields.cursorShadow) rdpOptionFields.cursorShadow.checked = false;
      if (rdpOptionFields.cursorSettings) rdpOptionFields.cursorSettings.checked = true;
    } else if (preset === "speed") {
      if (rdpOptionFields.colorDepth) rdpOptionFields.colorDepth.value = "16";
      if (rdpOptionFields.networkConnectionType) rdpOptionFields.networkConnectionType.value = "2";
      if (rdpOptionFields.performanceFlags) rdpOptionFields.performanceFlags.value = "15";
      if (rdpOptionFields.desktopBackground) rdpOptionFields.desktopBackground.checked = false;
      if (rdpOptionFields.fontSmoothing) rdpOptionFields.fontSmoothing.checked = false;
      if (rdpOptionFields.desktopComposition) rdpOptionFields.desktopComposition.checked = false;
      if (rdpOptionFields.fullWindowDrag) rdpOptionFields.fullWindowDrag.checked = false;
      if (rdpOptionFields.menuAnimations) rdpOptionFields.menuAnimations.checked = false;
      if (rdpOptionFields.visualStyles) rdpOptionFields.visualStyles.checked = false;
      if (rdpOptionFields.cursorShadow) rdpOptionFields.cursorShadow.checked = false;
      if (rdpOptionFields.cursorSettings) rdpOptionFields.cursorSettings.checked = false;
    }
  };
  rdpQualityPresetSelect?.addEventListener("change", () => {
    if (rdpQualityPresetSelect.value !== "custom") {
      applyRdpQualityPreset(rdpQualityPresetSelect.value);
    }
  });
  [
    rdpOptionFields.colorDepth, rdpOptionFields.networkConnectionType,
    rdpOptionFields.performanceFlags, rdpOptionFields.desktopBackground,
    rdpOptionFields.fontSmoothing, rdpOptionFields.desktopComposition,
    rdpOptionFields.fullWindowDrag, rdpOptionFields.menuAnimations,
    rdpOptionFields.visualStyles, rdpOptionFields.cursorShadow
  ].forEach(field => {
    field?.addEventListener("change", () => {
      if (rdpQualityPresetSelect) rdpQualityPresetSelect.value = "custom";
    });
  });

  const resetRdpAdvancedOptions = () => {
    Object.entries(rdpOptionFields).forEach(([key, field]) => {
      if (!field) return;
      if (rdpCheckboxKeys.has(key))
        field.checked = true;
      else
        field.value = String(defaultRdpOptionValue(key));
    });
    if (rdpQualityPresetSelect) rdpQualityPresetSelect.value = "balanced";
    serverFormStatus.textContent = "RDP 高级设置已恢复为默认";
  };
  rdpResetDefaultsButton?.addEventListener("click", resetRdpAdvancedOptions);
  serialPortRefreshButton.addEventListener("click", () => refreshSerialPorts(serialPortSelect.value));
  const browsePrivateKey = inputId => {
    const input = document.querySelector(inputId);
    post("local.chooseFile", { path: input.value })
      .then(result => {
        if (!result?.cancelled && result?.path)
          input.value = result.path;
      })
      .catch(reportError);
  };
  document.querySelector("#server-key-browse").addEventListener(
    "click", () => browsePrivateKey("#server-key-path"));
  document.querySelector("#server-proxy-key-browse").addEventListener(
    "click", () => browsePrivateKey("#server-proxy-key-path"));
  document.querySelector("#server-proxy-enabled")?.addEventListener("change", () => {
    const enabled = document.querySelector("#server-proxy-enabled")?.checked ?? false;
    document.querySelector("#jump-server-body")?.classList.toggle("disabled", !enabled);
  });
  serverForm.addEventListener("submit", event => {
    event.preventDefault();
    serverFormStatus.textContent = "正在保存…";
    const payload = profileFormPayload();
    const method = payload.index >= 0 ? "server.update" : "server.create";
    post(method, payload)
      .then(() => refreshProfiles())
      .then(() => serverDialog.close())
      .catch(error => {
        serverFormStatus.textContent = error instanceof Error ? error.message : String(error);
      });
  });

  sftpTransferTabs.forEach(tab => tab.addEventListener("click", () => {
    activeTransferTab = tab.dataset.transferTab;
    selectedTransferTarget = null;
    renderTransferPanel();
  }));
  sftpTransferResultsList.addEventListener("contextmenu", event => {
    const row = event.target.closest(".sftp-transfer-result");
    event.preventDefault();
    if (row) {
      const key = `${row.dataset.transferKind}:${row.dataset.transferIndex}`;
      selectedTransferTarget = {
        key,
        kind: row.dataset.transferKind,
        index: row.dataset.transferIndex
      };
    } else {
      selectedTransferTarget = null;
    }
    transferContextTarget = {
      kind: row?.dataset.transferKind || activeTransferTab,
      index: row ? row.dataset.transferIndex : -1
    };
    const rowTask = row && transferContextTarget.kind === "queue"
      ? (String(transferContextTarget.index).startsWith("edit:")
        ? remoteEditTransfers.get(String(transferContextTarget.index).slice(5))
        : transferQueue[Number(transferContextTarget.index)])
      : row ? transferResults[Number(transferContextTarget.index)] : null;
    const remoteEdit = rowTask?.type === "edit-sync";
    sftpTransferContextMenu.querySelectorAll("[data-transfer-action]").forEach(button => {
      const action = button.dataset.transferAction;
      const globalAction = ["pause-all", "resume-all", "retry-failed-all", "delete-all"].includes(action);
      button.hidden = row
        ? globalAction || (transferContextTarget.kind === "queue"
          ? (remoteEdit ? !["sync", "open-folder", "stop-tracking", "cancel"].includes(action)
            : !["up", "down", "pause", "cancel"].includes(action))
          : (remoteEdit ? !["sync", "open-folder", "stop-tracking", "retry"].includes(action)
            : !["retry", "details"].includes(action)))
        : !globalAction;
      if (action === "pause" && row) {
        const task = transferQueue[Number(row.dataset.transferIndex)];
        button.textContent = task?.paused ? "继续任务" : "暂停任务";
        button.hidden = transferContextTarget.kind !== "queue" || !task;
      }
      if (action === "pause-all") button.disabled = transferQueuePaused;
      if (action === "resume-all") button.disabled = !transferQueuePaused;
      if (action === "retry-failed-all") button.disabled = !transferResults.some(isFailedTransfer);
      if (action === "delete-all") button.disabled = transferContextTarget.kind === "queue"
        ? !transferQueue.length : !transferResults.some(result =>
          result.state === transferContextTarget.kind
          || (transferContextTarget.kind === "failed" && result.state === "cancelled"));
    });
    const separator = sftpTransferContextMenu.querySelector(".context-menu-separator");
    if (separator) {
      const children = [...sftpTransferContextMenu.children];
      const separatorIndex = children.indexOf(separator);
      const visibleBefore = children.slice(0, separatorIndex)
        .some(element => !element.hidden);
      const visibleAfter = children.slice(separatorIndex + 1)
        .some(element => !element.hidden);
      separator.hidden = !(visibleBefore && visibleAfter);
    }
    sftpTransferContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - 210)}px`;
    sftpTransferContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - 220)}px`;
    sftpTransferContextMenu.hidden = false;
  });
  sftpTransferContextMenu.addEventListener("click", event => {
    const button = event.target.closest("[data-transfer-action]");
    if (button) executeTransferContextAction(button.dataset.transferAction);
  });
  sftpTransferFilter?.addEventListener("input", event => {
    transferFilterText = event.target.value || "";
    renderTransferPanel();
  });
  sftpTransferRetryFailedButton?.addEventListener("click", () => {
    retryAllFailedTransfers();
  });
  document.addEventListener("pointerdown", event => {
    if (!sftpTransferContextMenu.hidden && !sftpTransferContextMenu.contains(event.target))
      closeTransferContextMenu();
  });
  document.addEventListener("contextmenu", event => {
    if (!event.target.closest("#sftp-transfer-results-list"))
      closeTransferContextMenu();
  });
  document.addEventListener("keydown", event => {
    if (event.key === "Escape") closeTransferContextMenu();
  });
  window.addEventListener("blur", closeTransferContextMenu);
  window.addEventListener("resize", closeTransferContextMenu);
  const savedTransferHeight = Number(localStorage.getItem("masterterm.transferPanelHeight"));
  if (Number.isFinite(savedTransferHeight))
    sftpStatus.style.setProperty("--sftp-status-height",
      `${Math.min(360, Math.max(110, savedTransferHeight))}px`);
  sftpStatusResizer.addEventListener("pointerdown", event => {
    event.preventDefault();
    sftpStatusResizer.classList.add("resizing");
    transferResizeStart = {
      y: event.clientY,
      height: sftpStatus.getBoundingClientRect().height
    };
    sftpStatusResizer.setPointerCapture?.(event.pointerId);
  });
  sftpStatusResizer.addEventListener("pointermove", event => {
    if (!transferResizeStart) return;
    const height = Math.min(360, Math.max(110,
      transferResizeStart.height + transferResizeStart.y - event.clientY));
    sftpStatus.style.setProperty("--sftp-status-height", `${height}px`);
  });
  const finishTransferResize = () => {
    if (!transferResizeStart) return;
    transferResizeStart = null;
    sftpStatusResizer.classList.remove("resizing");
    localStorage.setItem("masterterm.transferPanelHeight",
      String(Math.round(sftpStatus.getBoundingClientRect().height)));
  };
  sftpStatusResizer.addEventListener("pointerup", finishTransferResize);
  sftpStatusResizer.addEventListener("pointercancel", finishTransferResize);

  try {
    const savedColumns = JSON.parse(localStorage.getItem("masterterm.transferColumns") || "null");
    if (Array.isArray(savedColumns) && savedColumns.length === defaultTransferColumnWidths.length)
      transferColumnWidths = savedColumns.map((width, index) =>
        Math.min(420, Math.max(index === 0 ? 24 : 48, Number(width) || defaultTransferColumnWidths[index])));
  } catch { /* use defaults */ }
  const applyTransferColumnWidths = () => {
    applyTransferGridColumns();
  };
  sftpTransferResultsHeader.querySelectorAll("[data-transfer-column]").forEach((column, index) => {
    const handle = document.createElement("span");
    handle.className = "transfer-column-resizer";
    handle.title = "拖动调整列宽";
    column.appendChild(handle);
    handle.addEventListener("pointerdown", event => {
      event.preventDefault();
      transferColumnResizeStart = { index, x: event.clientX, width: transferColumnWidths[index] };
      handle.classList.add("resizing");
      handle.setPointerCapture?.(event.pointerId);
    });
    handle.addEventListener("pointermove", event => {
      if (!transferColumnResizeStart) return;
      transferColumnWidths[index] = Math.min(420, Math.max(index === 0 ? 24 : 48,
        transferColumnResizeStart.width + event.clientX - transferColumnResizeStart.x));
      applyTransferColumnWidths();
    });
    const finishColumnResize = () => {
      if (!transferColumnResizeStart) return;
      transferColumnResizeStart = null;
      handle.classList.remove("resizing");
      localStorage.setItem("masterterm.transferColumns", JSON.stringify(transferColumnWidths));
    };
    handle.addEventListener("pointerup", finishColumnResize);
    handle.addEventListener("pointercancel", finishColumnResize);
  });
  applyTransferColumnWidths();

  // SFTP cd into Terminal
  const cdInActiveTerminal = targetPath => {
    if (!targetPath) return;
    let targetSession = null;
    if (activeSftpSessionId && sessions.has(activeSftpSessionId)) {
      const s = sessions.get(activeSftpSessionId);
      if (s.connectionType === "ssh" && s.state === "connected") {
        targetSession = s;
      }
    }
    if (!targetSession && sftpProfile) {
      targetSession = Array.from(sessions.values()).find(s =>
        s.connectionType === "ssh" && s.profileIndex === sftpProfile.index && s.state === "connected"
      );
    }
    if (!targetSession) {
      const focused = sessions.get(focusedSessionId);
      if (focused && focused.connectionType === "ssh" && focused.state === "connected") {
        targetSession = focused;
      }
    }
    if (!targetSession) {
      showTransientStatus("未找到该 SFTP 关联的活动 SSH 终端，无法执行远程 cd。");
      return;
    }
    focusSession(targetSession.sessionId);
    const sanitizedPath = targetPath.replace(/["$`]/g, "");
    const cdCmd = `cd "${sanitizedPath}"\r`;
    post("session.input", { sessionId: targetSession.sessionId, data: cdCmd }).catch(reportError);
    targetSession.terminal?.focus();
    const sessionName = targetSession.displayName || targetSession.name || targetSession.title || targetSession.sessionId;
    showTransientStatus(`已在 SSH 终端 [${sessionName}] 中执行：cd "${sanitizedPath}"`);
  };
  document.querySelector("#sftp-open-in-terminal")?.addEventListener("click", () => {
    cdInActiveTerminal(sftpPath);
  });

  // Broadcast Input
  const toggleBroadcastInput = () => {
    broadcastInputActive = !broadcastInputActive;
    const button = document.querySelector("#broadcast-input-button");
    if (button) {
      button.classList.toggle("active", broadcastInputActive);
      button.setAttribute("aria-pressed", String(broadcastInputActive));
      button.title = broadcastInputActive ? "广播输入：开启中（按键将同步到所有终端）" : "广播输入到所有终端";
      document.querySelector("#tools-broadcast-indicator")?.toggleAttribute("hidden", !broadcastInputActive);
      document.querySelector("#broadcast-menu-check")?.toggleAttribute("hidden", !broadcastInputActive);
    }
    document.querySelectorAll(".terminal-tab").forEach(tab => {
      tab.classList.toggle("broadcast-active", broadcastInputActive);
    });
    showTransientStatus(broadcastInputActive
      ? "已开启终端按键广播：所有键盘输入将同步发送到所有终端！"
      : "已关闭终端广播输入。");
  };
  document.querySelector("#broadcast-input-button")?.addEventListener("click", toggleBroadcastInput);

  // Terminal Theme Preset change & color picker synchronization
  document.querySelector("#setting-terminal-theme-preset")?.addEventListener("change", event => {
    const preset = terminalThemePresets[event.target.value];
    if (preset) {
      document.querySelector("#setting-terminal-background").value = preset.background;
      document.querySelector("#setting-terminal-foreground").value = preset.foreground;
    }
    updateTerminalSettingsPreview();
  });
  ["#setting-terminal-background", "#setting-terminal-foreground"].forEach(sel => {
    document.querySelector(sel)?.addEventListener("input", () => {
      const presetSelect = document.querySelector("#setting-terminal-theme-preset");
      if (presetSelect) presetSelect.value = "custom";
    });
  });

  // Quick Switcher implementation
  let quickSwitcherSelectedIndex = 0;
  let quickSwitcherFilteredItems = [];
  let quickSwitcherKeyboardNav = false;

  const getQuickSwitcherItems = () => {
    const items = [];
    sessions.forEach(session => {
      if (session.state !== "closed") {
        let typeBadge = "SSH";
        if (session.connectionType === "local") typeBadge = "本地终端";
        else if (session.connectionType === "rdp") typeBadge = "RDP";
        else if (session.connectionType === "serial") typeBadge = "串口";

        items.push({
          type: "session",
          icon: "⚡",
          title: session.displayName || session.name || "未命名终端",
          sub: `会话 ID: ${session.sessionId} · ${typeBadge}`,
          badge: "已连接",
          badgeColor: "#10b981",
          keywords: `${session.displayName || ""} ${session.name || ""} ${session.sessionId} ${typeBadge} session`,
          action: () => focusSession(session.sessionId)
        });
      }
    });

    (profilesCache || []).forEach(profile => {
      let badge = "SSH";
      if (profile.connectionType === "rdp") badge = "RDP";
      else if (profile.connectionType === "serial") badge = "串口";

      const sub = profile.connectionType === "serial"
        ? (profile.serialPort || "串口")
        : (profile.host ? `${profile.username || "root"}@${profile.host}:${profile.port || 22}` : "");

      items.push({
        type: "profile",
        icon: profile.connectionType === "rdp" ? "🖥️" : (profile.connectionType === "serial" ? "🔌" : "🌐"),
        title: profile.name,
        sub,
        badge,
        badgeColor: "#3b82f6",
        keywords: `${profile.name} ${profile.host || ""} ${profile.username || ""} ${badge}`,
        action: () => openExistingConnection(profile)
      });
    });

    const shells = [
      { id: "pwsh", name: "新建终端: PowerShell 7", desc: "pwsh" },
      { id: "powershell", name: "新建终端: Windows PowerShell", desc: "powershell" },
      { id: "cmd", name: "新建终端: 命令提示符 (CMD)", desc: "cmd" },
      { id: "wsl", name: "新建终端: WSL (Linux)", desc: "wsl" },
      { id: "gitbash", name: "新建终端: Git Bash", desc: "bash" }
    ];
    shells.forEach(s => {
      const isInstalled = (s.id === "cmd") ? true : (detectedShells ? detectedShells.get(s.id) !== false : true);
      items.push({
        type: "shell",
        icon: "💻",
        title: s.name + (!isInstalled && detectedShells ? " (未安装)" : ""),
        sub: s.desc,
        badge: (!isInstalled && detectedShells) ? "未安装" : "Shell",
        badgeColor: (!isInstalled && detectedShells) ? "#64748b" : "#8b5cf6",
        keywords: `${s.name} ${s.desc} shell local 终端 本地`,
        action: () => {
          if (!isInstalled && detectedShells) {
            showTransientStatus(`本地未检测到 ${s.desc}，请先安装该终端。`);
            return;
          }
          connectLocalTerminal(s.id);
        }
      });
    });

    items.push({
      type: "action",
      icon: "📢",
      title: "广播输入: 切换所有终端按键同步",
      sub: "实时将键盘输入同步发送到所有已连接的终端",
      badge: "命令",
      badgeColor: "#ec4899",
      keywords: "broadcast 广播 广播输入 同步输入 全局输入",
      action: () => toggleBroadcastInput()
    });
    items.push({
      type: "action",
      icon: "⚙️",
      title: "设置: 打开偏好设置",
      sub: "外观、字体、颜色、启动选项与保活配置",
      badge: "系统",
      badgeColor: "#64748b",
      keywords: "settings 设置 偏好设置 配置 theme 配色 字体",
      action: () => {
        populateSettingsForm();
        settingsDialog.showModal();
      }
    });
    items.push({
      type: "action",
      icon: "🎨",
      title: "主题: 切换深色主题",
      sub: "Dark Theme",
      badge: "主题",
      badgeColor: "#64748b",
      keywords: "theme dark 深色主题 暗色 黑",
      action: () => applyTheme("dark")
    });
    items.push({
      type: "action",
      icon: "☀️",
      title: "主题: 切换浅色主题",
      sub: "Light Theme",
      badge: "主题",
      badgeColor: "#64748b",
      keywords: "theme light 浅色主题 亮色 白",
      action: () => applyTheme("light")
    });
    items.push({
      type: "action",
      icon: "🌊",
      title: "主题: 切换蓝色主题",
      sub: "Blue Ocean Theme",
      badge: "主题",
      badgeColor: "#64748b",
      keywords: "theme blue 蓝色主题 经典蓝",
      action: () => applyTheme("blue")
    });
    items.push({
      type: "action",
      icon: "⚡",
      title: "命令: 批量执行命令",
      sub: "在多个已连接的会话中同时执行指令",
      badge: "批量",
      badgeColor: "#ec4899",
      keywords: "batch 批量 批量命令 batch-command 批量执行",
      action: () => openBatchCommand()
    });
    items.push({
      type: "action",
      icon: "📁",
      title: "SFTP: 切换本地文件浏览器面板",
      sub: "在 SFTP 面板中显示或隐藏本地双列文件视图",
      badge: "SFTP",
      badgeColor: "#10b981",
      keywords: "sftp local 本地文件 双列 浏览",
      action: () => setLocalPanelVisible(!localPanelVisible)
    });
    items.push({
      type: "action",
      icon: "🔲",
      title: "分屏: 关闭当前分屏",
      sub: "恢复单标签视图",
      badge: "分屏",
      badgeColor: "#f59e0b",
      keywords: "split 关闭分屏 close split",
      action: () => {
        setSplit(null, null);
        showTransientStatus("已关闭分屏。");
      }
    });
    items.push({
      type: "action",
      icon: "❓",
      title: "帮助: 快捷键速查中心",
      sub: "查看全部键盘操作映射与快捷指引 (Ctrl+/)",
      badge: "帮助",
      badgeColor: "#3b82f6",
      keywords: "shortcuts help 快捷键 帮助 速查 快捷键指南 ?",
      action: () => openShortcutsDialog()
    });

    (workspaceNames || []).forEach(ws => {
      const count = (profilesCache || []).filter(p => (p.workspace || "未分配") === ws).length;
      items.push({
        type: "workspace",
        icon: "📂",
        title: `工作区: ${ws}`,
        sub: `切换视图至「${ws}」（共 ${count} 个连接）`,
        badge: "工作区",
        badgeColor: "#8b5cf6",
        keywords: `workspace 工作区 ${ws}`,
        action: () => {
          activeWorkspace = ws;
          if (workspaceFilterSelect) workspaceFilterSelect.value = ws;
          render(profilesCache);
          showTransientStatus(`已切换至工作区「${ws}」`);
        }
      });
    });

    return items;
  };

  const renderQuickSwitcherResults = () => {
    const list = document.querySelector("#quick-switcher-results");
    if (!list) return;
    list.innerHTML = "";

    if (quickSwitcherFilteredItems.length === 0) {
      const empty = document.createElement("div");
      empty.className = "quick-switcher-empty";
      empty.textContent = "未找到匹配项";
      list.appendChild(empty);
      return;
    }

    quickSwitcherFilteredItems.forEach((item, index) => {
      const el = document.createElement("div");
      el.className = "quick-switcher-item" + (index === quickSwitcherSelectedIndex ? " selected" : "");
      
      const badge = document.createElement("span");
      badge.className = "quick-switcher-item-badge";
      badge.textContent = item.badge;
      if (item.badgeColor) {
        badge.style.borderColor = item.badgeColor;
        badge.style.color = item.badgeColor;
      }

      const icon = document.createElement("span");
      icon.className = "quick-switcher-item-icon";
      icon.textContent = item.icon || "•";
      
      const title = document.createElement("span");
      title.className = "quick-switcher-item-title";
      title.textContent = item.title;
      
      el.appendChild(badge);
      el.appendChild(icon);
      el.appendChild(title);

      if (item.sub) {
        const sub = document.createElement("span");
        sub.className = "quick-switcher-item-sub";
        sub.textContent = item.sub;
        el.appendChild(sub);
      }

      el.addEventListener("click", () => {
        closeQuickSwitcher();
        item.action();
      });
      el.addEventListener("mouseenter", () => {
        if (quickSwitcherKeyboardNav) return;
        quickSwitcherSelectedIndex = index;
        updateQuickSwitcherSelection();
      });

      list.appendChild(el);
    });

    scrollQuickSwitcherSelectionIntoView();
  };

  const updateQuickSwitcherSelection = () => {
    const items = document.querySelectorAll(".quick-switcher-item");
    items.forEach((item, index) => {
      item.classList.toggle("selected", index === quickSwitcherSelectedIndex);
    });
  };

  const scrollQuickSwitcherSelectionIntoView = () => {
    const selected = document.querySelector(".quick-switcher-item.selected");
    if (selected) {
      selected.scrollIntoView({ block: "nearest", behavior: "auto" });
    }
  };

  const filterQuickSwitcherItems = query => {
    const raw = (query || "").trim().toLowerCase();
    const all = getQuickSwitcherItems();
    if (!raw) {
      quickSwitcherFilteredItems = all;
    } else {
      const parts = raw.split(/\s+/).filter(Boolean);
      quickSwitcherFilteredItems = all.filter(item => {
        const haystack = `${item.title} ${item.sub || ""} ${item.badge || ""} ${item.keywords || ""}`.toLowerCase();
        return parts.every(part => haystack.includes(part));
      });
    }
    quickSwitcherSelectedIndex = 0;
    renderQuickSwitcherResults();
  };

  const openQuickSwitcher = () => {
    const dialog = document.querySelector("#quick-switcher-dialog");
    const input = document.querySelector("#quick-switcher-input");
    if (!dialog || !input) return;
    quickSwitcherKeyboardNav = false;
    input.value = "";
    filterQuickSwitcherItems("");
    dialog.showModal();
    input.focus();
  };

  const closeQuickSwitcher = () => {
    const dialog = document.querySelector("#quick-switcher-dialog");
    if (dialog?.open) dialog.close();
  };

  const toggleQuickSwitcher = () => {
    const dialog = document.querySelector("#quick-switcher-dialog");
    if (dialog?.open) closeQuickSwitcher();
    else openQuickSwitcher();
  };

  const initQuickSwitcher = () => {
    const dialog = document.querySelector("#quick-switcher-dialog");
    const input = document.querySelector("#quick-switcher-input");
    const resultsContainer = document.querySelector("#quick-switcher-results");
    if (resultsContainer) {
      resultsContainer.addEventListener("mousemove", () => {
        quickSwitcherKeyboardNav = false;
      });
    }
    if (dialog) {
      dialog.addEventListener("click", event => {
        const rect = dialog.getBoundingClientRect();
        const isInDialog = (
          rect.top <= event.clientY &&
          event.clientY <= rect.top + rect.height &&
          rect.left <= event.clientX &&
          event.clientX <= rect.left + rect.width
        );
        if (!isInDialog) {
          closeQuickSwitcher();
        }
      });
      document.addEventListener("mousedown", event => {
        if (dialog.open && !dialog.contains(event.target)) {
          closeQuickSwitcher();
        }
      });
      window.addEventListener("blur", () => {
        if (dialog.open) {
          closeQuickSwitcher();
        }
      });
    }
    if (input) {
      input.addEventListener("input", () => filterQuickSwitcherItems(input.value));
      input.addEventListener("keydown", event => {
        if (event.key === "ArrowDown") {
          quickSwitcherKeyboardNav = true;
          if (quickSwitcherFilteredItems.length > 0) {
            quickSwitcherSelectedIndex = (quickSwitcherSelectedIndex + 1) % quickSwitcherFilteredItems.length;
            updateQuickSwitcherSelection();
            scrollQuickSwitcherSelectionIntoView();
          }
          event.preventDefault();
        } else if (event.key === "ArrowUp") {
          quickSwitcherKeyboardNav = true;
          if (quickSwitcherFilteredItems.length > 0) {
            quickSwitcherSelectedIndex = (quickSwitcherSelectedIndex - 1 + quickSwitcherFilteredItems.length) % quickSwitcherFilteredItems.length;
            updateQuickSwitcherSelection();
            scrollQuickSwitcherSelectionIntoView();
          }
          event.preventDefault();
        } else if (event.key === "Enter") {
          if (quickSwitcherFilteredItems[quickSwitcherSelectedIndex]) {
            const item = quickSwitcherFilteredItems[quickSwitcherSelectedIndex];
            closeQuickSwitcher();
            item.action();
          }
          event.preventDefault();
        } else if (event.key === "Escape") {
          closeQuickSwitcher();
          event.preventDefault();
        }
      });
    }
    document.querySelector("#quick-switcher-button")?.addEventListener("click", toggleQuickSwitcher);
    document.querySelector(".quick-switcher-esc-badge")?.addEventListener("click", closeQuickSwitcher);
  };
  initQuickSwitcher();

  const openShortcutsDialog = () => {
    const dialog = document.querySelector("#shortcuts-dialog");
    if (dialog && !dialog.open) {
      dialog.showModal();
    }
  };

  const closeShortcutsDialog = () => {
    const dialog = document.querySelector("#shortcuts-dialog");
    if (dialog?.open) {
      dialog.close();
    }
  };

  const toggleShortcutsDialog = () => {
    const dialog = document.querySelector("#shortcuts-dialog");
    if (dialog?.open) closeShortcutsDialog();
    else openShortcutsDialog();
  };

  const initShortcutsDialog = () => {
    const dialog = document.querySelector("#shortcuts-dialog");
    if (!dialog) return;
    document.querySelector("#shortcuts-help-button")?.addEventListener("click", toggleShortcutsDialog);
    document.querySelector("#shortcuts-close-badge")?.addEventListener("click", closeShortcutsDialog);
  };
  initShortcutsDialog();

  // 顶部菜单栏下拉交互（工具 ▾ 与 帮助 ▾）
  const initHeaderMenus = () => {
    const toolsBtn = document.querySelector("#tools-menu-button");
    const toolsMenu = document.querySelector("#tools-menu");
    const helpBtn = document.querySelector("#help-menu-button");
    const helpMenu = document.querySelector("#help-menu");

    const closeAllHeaderDropdowns = () => {
      if (toolsMenu) toolsMenu.hidden = true;
      if (helpMenu) helpMenu.hidden = true;
    };

    if (toolsBtn && toolsMenu) {
      toolsBtn.addEventListener("click", event => {
        event.stopPropagation();
        const opening = toolsMenu.hidden;
        closeThemeMenu();
        closeAllHeaderDropdowns();
        if (opening) {
          if (activeRdpVisible()) {
            const rect = toolsBtn.getBoundingClientRect();
            post("app.toolsMenu", {
              x: Math.round(rect.left),
              y: Math.round(rect.bottom + 6),
              broadcastActive: Boolean(broadcastInputActive)
            }).catch(reportError);
            return;
          }
          toolsMenu.hidden = false;
          const rect = toolsBtn.getBoundingClientRect();
          toolsMenu.style.left = `${Math.max(8, Math.min(rect.left, window.innerWidth - 220))}px`;
          toolsMenu.style.top = `${rect.bottom + 4}px`;
        }
      });
    }

    if (helpBtn && helpMenu) {
      helpBtn.addEventListener("click", event => {
        event.stopPropagation();
        const opening = helpMenu.hidden;
        closeThemeMenu();
        closeAllHeaderDropdowns();
        if (opening) {
          if (activeRdpVisible()) {
            const rect = helpBtn.getBoundingClientRect();
            post("app.helpMenu", {
              x: Math.round(rect.left),
              y: Math.round(rect.bottom + 6)
            }).catch(reportError);
            return;
          }
          helpMenu.hidden = false;
          const rect = helpBtn.getBoundingClientRect();
          helpMenu.style.left = `${Math.max(8, Math.min(rect.left, window.innerWidth - 200))}px`;
          helpMenu.style.top = `${rect.bottom + 4}px`;
        }
      });
    }

    // 点击页面其他区域关闭下拉菜单
    document.addEventListener("click", event => {
      if (!event.target.closest("#tools-menu-button") && !event.target.closest("#tools-menu")) {
        if (toolsMenu) toolsMenu.hidden = true;
      }
      if (!event.target.closest("#help-menu-button") && !event.target.closest("#help-menu")) {
        if (helpMenu) helpMenu.hidden = true;
      }
    });

    // 工具菜单内项目事件
    document.querySelector("#header-batch-cmd-btn")?.addEventListener("click", () => {
      closeAllHeaderDropdowns();
      renderBatchCommandSessions();
      document.querySelector("#batch-command-dialog")?.showModal();
    });

    document.querySelector("#header-cloud-sync-btn")?.addEventListener("click", () => {
      closeAllHeaderDropdowns();
      openSettingsDialog();
      setTimeout(() => {
        document.querySelector("#cloud-sync-settings")?.scrollIntoView({ behavior: "smooth" });
      }, 100);
    });

    document.querySelector("#header-diagnostics-btn")?.addEventListener("click", () => {
      closeAllHeaderDropdowns();
      document.querySelector("#terminal-diagnostics-dialog")?.showModal();
    });

    // 帮助菜单内项目事件
    document.querySelector("#header-check-updates-btn")?.addEventListener("click", () => {
      closeAllHeaderDropdowns();
      openUpdateDialog();
    });

    document.querySelector("#header-about-btn")?.addEventListener("click", () => {
      closeAllHeaderDropdowns();
      openAboutDialog();
    });
  };
  initHeaderMenus();

  // 关于 MasterTerm 弹窗
  const openAboutDialog = () => {
    const dialog = document.querySelector("#about-dialog");
    if (!dialog) return;
    const versionBadge = document.querySelector("#about-dialog-version-badge");
    const currentVer = document.querySelector("#current-version-label")?.textContent
      || appSettings.version || "0.1.128";
    if (versionBadge) {
      versionBadge.textContent = currentVer.startsWith("v") ? currentVer : `v${currentVer}`;
    }
    if (!dialog.open) {
      dialog.showModal();
    }
  };
  window.openAboutDialog = openAboutDialog;

  const closeAboutDialog = () => {
    const dialog = document.querySelector("#about-dialog");
    if (dialog?.open) dialog.close();
  };
  window.closeAboutDialog = closeAboutDialog;

  const initAboutDialog = () => {
    const dialog = document.querySelector("#about-dialog");
    if (!dialog) return;
    document.querySelector("#about-dialog-close")?.addEventListener("click", closeAboutDialog);
    document.querySelector("#about-dialog-ok-btn")?.addEventListener("click", closeAboutDialog);
    const openPortal = event => {
      event?.preventDefault();
      post("shell.openUrl", { url: "https://master.dapang.wang" }).catch(reportError);
    };
    document.querySelector("#about-portal-url")?.addEventListener("click", openPortal);
    document.querySelector("#about-open-portal-btn")?.addEventListener("click", openPortal);
  };
  initAboutDialog();

  const formatVersionTag = v => {
    if (!v) return "—";
    const s = String(v).trim();
    return s.startsWith("v") ? s : `v${s}`;
  };

  const limitChangelogTo5 = raw => {
    if (!raw) return "（发布方未提供详细变更说明）";
    const text = String(raw).trim();
    if (!text) return "（发布方未提供详细变更说明）";
    const lines = text.split("\n");
    const resultLines = [];
    let versionCount = 0;
    let foundVersionHeader = false;
    for (const line of lines) {
      if (/^###\s+v?\d+\.\d+/.test(line.trim())) {
        foundVersionHeader = true;
        versionCount++;
        if (versionCount > 5) break;
      }
      resultLines.push(line);
    }
    if (foundVersionHeader) {
      return resultLines.join("\n").trim();
    }
    const commitBullets = lines.filter(l => /^\s*[-*]\s+/.test(l));
    if (commitBullets.length > 5) {
      let count = 0;
      const filtered = [];
      for (const line of lines) {
        if (/^\s*[-*]\s+/.test(line)) {
          count++;
          if (count <= 5) filtered.push(line);
        } else if (count <= 5) {
          filtered.push(line);
        }
      }
      return filtered.join("\n").trim();
    }
    return text;
  };

  // 独立检查更新弹窗
  const openUpdateDialog = () => {
    const dialog = document.querySelector("#update-dialog");
    if (!dialog) return;
    const currentVer = document.querySelector("#current-version-label")?.textContent
      || appSettings.version || "0.1.128";
    const currentVerEl = document.querySelector("#update-dialog-current-ver");
    if (currentVerEl) {
      currentVerEl.textContent = formatVersionTag(currentVer);
    }
    const latestWrap = document.querySelector("#update-dialog-latest-wrap");
    if (latestWrap) latestWrap.hidden = true;
    const changelogWrap = document.querySelector("#update-dialog-changelog-wrap");
    if (changelogWrap) changelogWrap.hidden = true;
    const progressWrap = document.querySelector("#update-dialog-progress-wrap");
    if (progressWrap) progressWrap.hidden = true;
    const actionBtn = document.querySelector("#update-dialog-action-btn");
    if (actionBtn) actionBtn.hidden = true;

    if (!dialog.open) {
      dialog.showModal();
    }
    runStandaloneUpdateCheck();
  };
  window.openUpdateDialog = openUpdateDialog;

  const closeUpdateDialog = () => {
    const dialog = document.querySelector("#update-dialog");
    if (dialog?.open) dialog.close();
  };
  window.closeUpdateDialog = closeUpdateDialog;

  const runStandaloneUpdateCheck = () => {
    const statusTitle = document.querySelector("#update-dialog-status-title");
    const statusIcon = document.querySelector("#update-dialog-status-icon");
    const changelogWrap = document.querySelector("#update-dialog-changelog-wrap");
    const changelogPre = document.querySelector("#update-dialog-changelog");
    const latestWrap = document.querySelector("#update-dialog-latest-wrap");
    const latestVerEl = document.querySelector("#update-dialog-latest-ver");
    const actionBtn = document.querySelector("#update-dialog-action-btn");
    const progressWrap = document.querySelector("#update-dialog-progress-wrap");

    if (statusTitle) statusTitle.textContent = "正在连接更新服务器检查…";
    if (statusIcon) statusIcon.textContent = "🔍";
    if (changelogWrap) changelogWrap.hidden = true;
    if (latestWrap) latestWrap.hidden = true;
    if (progressWrap) progressWrap.hidden = true;
    if (actionBtn) {
      actionBtn.hidden = true;
      actionBtn.disabled = false;
    }

    const currentVer = document.querySelector("#current-version-label")?.textContent
      || appSettings.version || "0.1.128";

    postWithTimeout("update.check", { url: appSettings.updateServerUrl || updateServerDefaultUrl })
      .then(result => {
        if (result && result.hasUpdate) {
          if (statusTitle) statusTitle.textContent = `发现新版本 ${formatVersionTag(result.latestVersion)}！`;
          if (statusIcon) statusIcon.textContent = "🚀";
          if (latestWrap && latestVerEl) {
            latestWrap.hidden = false;
            latestVerEl.textContent = formatVersionTag(result.latestVersion);
          }
          if (changelogWrap && changelogPre) {
            changelogWrap.hidden = false;
            const titleEl = changelogWrap.querySelector(".update-changelog-title");
            if (titleEl) titleEl.textContent = "版本更新说明：";
            changelogPre.textContent = limitChangelogTo5(result.body);
          }
          if (actionBtn) {
            actionBtn.hidden = false;
            actionBtn.disabled = false;
            actionBtn.textContent = (result.url && result.sha256) ? "立即下载并更新" : "前往发布页面";
            actionBtn.onclick = () => {
              if (result.url && result.sha256) {
                startStandaloneDownload(result.url, result.sha256);
              } else if (result.url) {
                post("shell.openUrl", { url: result.url }).catch(reportError);
              }
            };
          }
        } else if (result && result.status > 0) {
          if (statusTitle) statusTitle.textContent = "当前已是最新版本";
          if (statusIcon) statusIcon.textContent = "✅";
          if (latestWrap && latestVerEl) {
            latestWrap.hidden = false;
            latestVerEl.textContent = formatVersionTag(result.latestVersion || result.currentVersion || currentVer);
          }
          if (changelogWrap && changelogPre) {
            changelogWrap.hidden = false;
            const titleEl = changelogWrap.querySelector(".update-changelog-title");
            if (titleEl) titleEl.textContent = "检查状态：";
            changelogPre.textContent = `当前安装的 MasterTerm 已是最新版本，已包含全部安全修复与功能更新。\n检查时间：${new Date().toLocaleTimeString()}`;
          }
        } else {
          if (statusTitle) statusTitle.textContent = "检查失败（无法连接更新源）";
          if (statusIcon) statusIcon.textContent = "⚠️";
          if (changelogWrap && changelogPre) {
            changelogWrap.hidden = false;
            const titleEl = changelogWrap.querySelector(".update-changelog-title");
            if (titleEl) titleEl.textContent = "提示信息：";
            changelogPre.textContent = "无法连接至版本分发服务器，请检查网络连接或稍后重试。";
          }
        }
      })
      .catch(err => {
        if (statusTitle) statusTitle.textContent = "检查更新出错";
        if (statusIcon) statusIcon.textContent = "⚠️";
        if (changelogWrap && changelogPre) {
          changelogWrap.hidden = false;
          changelogPre.textContent = `检查更新异常：${err instanceof Error ? err.message : String(err)}`;
        }
      });
  };

  const startStandaloneDownload = (url, sha256) => {
    const progressWrap = document.querySelector("#update-dialog-progress-wrap");
    const progressFill = document.querySelector("#update-dialog-progress-fill");
    const progressText = document.querySelector("#update-dialog-progress-text");
    const statusTitle = document.querySelector("#update-dialog-status-title");
    const actionBtn = document.querySelector("#update-dialog-action-btn");

    if (progressWrap) progressWrap.hidden = false;
    if (progressFill) progressFill.style.width = "0%";
    if (progressText) progressText.textContent = "正在下载更新包 0%…";
    if (statusTitle) statusTitle.textContent = "正在下载新版本…";
    if (actionBtn) {
      actionBtn.disabled = true;
      actionBtn.textContent = "下载中…";
    }

    let downloadPath = "";
    updateProgressHandler = payload => {
      const done = Number(payload.done || 0);
      const total = Number(payload.total || 0);
      const percent = total > 0 ? Math.min(100, Math.round((done / total) * 100)) : 0;
      if (progressFill) progressFill.style.width = `${percent}%`;
      if (progressText) {
        progressText.textContent = total > 0
          ? `正在下载更新包 ${percent}% (${formatSize(done)} / ${formatSize(total)})`
          : "正在下载更新包…";
      }
    };

    post("update.download", { url, sha256 })
      .then(result => {
        downloadPath = String(result.path || "");
        if (progressFill) progressFill.style.width = "100%";
        if (progressText) progressText.textContent = "更新包下载校验完成，准备安装…";
        if (statusTitle) statusTitle.textContent = "下载完成";
        if (actionBtn) {
          actionBtn.disabled = false;
          actionBtn.textContent = "立即重启安装";
          actionBtn.onclick = () => {
            if (!downloadPath) return;
            post("update.install", { path: downloadPath })
              .then(() => post("app.closeDecision", { decision: "exit" }))
              .catch(reportError);
          };
        }
      })
      .catch(error => {
        if (progressText) progressText.textContent = `下载失败：${error instanceof Error ? error.message : String(error)}`;
        if (statusTitle) statusTitle.textContent = "更新失败";
        if (actionBtn) {
          actionBtn.disabled = false;
          actionBtn.textContent = "重试下载";
          actionBtn.onclick = () => startStandaloneDownload(url, sha256);
        }
      });
  };

  const initUpdateDialog = () => {
    const dialog = document.querySelector("#update-dialog");
    if (!dialog) return;
    document.querySelector("#update-dialog-close")?.addEventListener("click", closeUpdateDialog);
    document.querySelector("#update-dialog-close-btn")?.addEventListener("click", closeUpdateDialog);
    document.querySelector("#update-dialog-recheck-btn")?.addEventListener("click", runStandaloneUpdateCheck);

    dialog.addEventListener("click", event => {
      const rect = dialog.getBoundingClientRect();
      const inBox = (
        rect.top <= event.clientY &&
        event.clientY <= rect.top + rect.height &&
        rect.left <= event.clientX &&
        event.clientX <= rect.left + rect.width
      );
      if (!inBox) {
        closeUpdateDialog();
      }
    });
    document.addEventListener("mousedown", event => {
      if (dialog.open && !dialog.contains(event.target)) {
        closeUpdateDialog();
      }
    });
    window.addEventListener("blur", () => {
      if (dialog.open) {
        closeUpdateDialog();
      }
    });
  };
  initUpdateDialog();


  const hasActiveModal = () => {
    return Boolean(document.querySelector("dialog[open]:not(.rdp-editor-fallback-freeze)"))
      || Boolean(rdpKeyMenu && !rdpKeyMenu.hidden)
      || Boolean(terminalSearchBar && !terminalSearchBar.hidden)
      || Boolean(document.querySelector("#tools-menu:not([hidden])"))
      || Boolean(document.querySelector("#help-menu:not([hidden])"))
      || Boolean(document.querySelector("#theme-menu:not([hidden])"));
  };
  window.hasActiveModal = hasActiveModal;

  const dismissActiveModal = (options = {}) => {
    const lightDismissOnly = Boolean(options && options.lightDismissOnly);

    // 0. Dropdown menus
    const tm = document.querySelector("#tools-menu");
    const hm = document.querySelector("#help-menu");
    const thm = document.querySelector("#theme-menu");
    let closedMenu = false;
    if (tm && !tm.hidden) { tm.hidden = true; closedMenu = true; }
    if (hm && !hm.hidden) { hm.hidden = true; closedMenu = true; }
    if (thm && !thm.hidden) { thm.hidden = true; closedMenu = true; }
    if (closedMenu) return true;

    // 1. Quick switcher has top priority (light-dismissable)
    const qs = document.querySelector("#quick-switcher-dialog");
    if (qs?.open) {
      closeQuickSwitcher();
      return true;
    }
    // 2. Update dialog (light-dismissable)
    const ud = document.querySelector("#update-dialog");
    if (ud?.open) {
      closeUpdateDialog();
      return true;
    }
    // RDP key menu & Terminal search bar
    if (rdpKeyMenu && !rdpKeyMenu.hidden) {
      closeRdpKeyMenu();
      return true;
    }
    if (terminalSearchBar && !terminalSearchBar.hidden) {
      closeTerminalSearch();
      return true;
    }

    // When lightDismissOnly is requested (from backdrop click, outside click, or dim overlay click),
    // strictly protect all other dialogs: they require explicit user action (close button, cancel, or Esc)
    if (lightDismissOnly) {
      return false;
    }

    // 3. Shortcuts dialog
    const sc = document.querySelector("#shortcuts-dialog");
    if (sc?.open) {
      closeShortcutsDialog();
      return true;
    }
    // 4. About dialog
    const ab = document.querySelector("#about-dialog");
    if (ab?.open) {
      closeAboutDialog();
      return true;
    }
    // 5. Diagnostics
    const td = document.querySelector("#terminal-diagnostics-dialog");
    if (td?.open) {
      td.close();
      return true;
    }
    // 6. Remote preview
    const rp = document.querySelector("#remote-preview-dialog");
    if (rp?.open) {
      rp.close();
      return true;
    }
    // 7. Cloud history / diff
    const ch = document.querySelector("#cloud-history-dialog");
    if (ch?.open) {
      ch.close();
      return true;
    }
    const cd = document.querySelector("#cloud-diff-dialog");
    if (cd?.open) {
      document.querySelector("#cloud-diff-cancel")?.click();
      if (cd.open) cd.close();
      return true;
    }
    // 8. Command favorites / Batch command / Command macro
    const cmdMacro = document.querySelector("#command-macro-dialog");
    if (cmdMacro?.open) {
      document.querySelector("#command-macro-cancel")?.click();
      if (cmdMacro.open) cmdMacro.close();
      return true;
    }
    const cf = document.querySelector("#command-favorites-dialog");
    if (cf?.open) {
      cf.close();
      return true;
    }
    const bc = document.querySelector("#batch-command-dialog");
    if (bc?.open) {
      document.querySelector("#batch-command-cancel")?.click();
      if (bc.open) bc.close();
      return true;
    }
    // 9. Action dialog
    const ad = document.querySelector("#action-dialog");
    if (ad?.open) {
      document.querySelector("#action-dialog-close")?.click();
      if (ad.open) ad.close();
      return true;
    }
    // 10. Tunnel dialog
    const tl = document.querySelector("#tunnel-dialog");
    if (tl?.open) {
      tl.close();
      return true;
    }
    // 11. Settings dialog
    const sd = document.querySelector("#settings-dialog");
    if (sd?.open) {
      document.querySelector("#settings-cancel")?.click();
      if (sd.open) sd.close();
      return true;
    }
    // 12. Server dialog
    const srv = document.querySelector("#server-dialog");
    if (srv?.open) {
      document.querySelector("#server-cancel")?.click();
      if (srv.open) srv.close();
      return true;
    }
    // 13. Any remaining open dialog
    const anyDialog = document.querySelector("dialog[open]:not(.rdp-editor-fallback-freeze)");
    if (anyDialog) {
      anyDialog.close();
      return true;
    }
    return false;
  };
  window.dismissActiveModal = dismissActiveModal;

  // Window capture listener on Escape to ensure open modals can always be dismissed
  window.addEventListener("keydown", event => {
    if (event.key === "Escape" && hasActiveModal()) {
      event.preventDefault();
      event.stopPropagation();
      dismissActiveModal();
    }
  }, true);

  // Global backdrop click dismissal: only light-dismissable dialogs (quick switcher, update dialog)
  // close on outside/backdrop click; all other dialogs require active user closure.
  document.addEventListener("click", event => {
    if (event.target instanceof HTMLDialogElement && event.target.open) {
      const rect = event.target.getBoundingClientRect();
      const inBox = (
        rect.top <= event.clientY && event.clientY <= rect.top + rect.height &&
        rect.left <= event.clientX && event.clientX <= rect.left + rect.width
      );
      if (!inBox) {
        dismissActiveModal({ lightDismissOnly: true });
      }
    }
  });

  try { renderTransferPanel(); } catch (err) { console.error("renderTransferPanel failed:", err); }
  try { applyTheme(activeTheme); } catch (err) { console.error("applyTheme failed:", err); }
  try { setLocalPanelVisible(appSettings.localPanelOnOpen); } catch (err) { console.error("setLocalPanelVisible failed:", err); }
  try { initSnippetsPanel(); } catch (err) { console.error("initSnippetsPanel failed:", err); }
  try { initTunnelsPanel(); } catch (err) { console.error("initTunnelsPanel failed:", err); }
  try { initCloudSidebarPanel(); } catch (err) { console.error("initCloudSidebarPanel failed:", err); }
  post("logs.configure", { enabled: appSettings.sessionLogging }).catch(() => {});
  post("update.check", { url: appSettings.updateServerUrl || updateServerDefaultUrl })
    .then(result => showUpdateDialog(result && typeof result === "object" ? result : null))
    .catch(() => {});

  // Startup fallback timer: release splash screen if initial backend calls take longer than 3.5s
  const startupFallbackTimer = setTimeout(() => {
    if (!frontendReady) {
      console.warn("Startup fallback timer triggered: releasing startup splash.");
      notifyFrontendReady();
    }
  }, 3500);

  Promise.all([post("app.getInfo"), post("workspace.list"), post("server.list")])
    .then(([info, names, profiles]) => {
      clearTimeout(startupFallbackTimer);
      const versionLabel = document.querySelector("#current-version-label");
      if (versionLabel && info && info.version)
        versionLabel.textContent = `v${info.version}`;
      workspaceNames = names;
      if (!workspaceNames.includes("未分配"))
        workspaceNames.unshift("未分配");
      renderWorkspaceFilter();
      render(profiles);
      notifyFrontendReady();
      return restoreSavedSessions(profiles)
        .then(() => recoverPersistedTransfers().catch(reportError));
    })
    .catch(error => {
      clearTimeout(startupFallbackTimer);
      notifyFrontendReady();
      reportError(error);
    });

  const warmTerminalLibraries = () => ensureTerminalLibraries().catch(reportError);
  if ("requestIdleCallback" in window)
    window.requestIdleCallback(warmTerminalLibraries, { timeout: 750 });
  else
    setTimeout(warmTerminalLibraries, 0);
})();
