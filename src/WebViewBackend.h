#pragma once

#include "RdpConnectionOptions.h"
#include "NativeJsonDom.h"
#include "NativeString.h"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct NativeStringHash {
    std::size_t operator()(const NativeString &value) const noexcept
    {
        return std::hash<std::string>{}(value.toStdString());
    }
};

class NativeProcess;
class LocalShellSession;
class SerialSession;
class SshSession;
class SshTunnel;
struct RdpQualitySnapshot;

// UI-independent JSON message boundary for the WebView2 host.
// The backend deliberately exposes no passwords or private-key contents.
class WebViewBackend final
{
public:
    WebViewBackend();
    ~WebViewBackend();

    void setSendHandler(std::function<void(const std::string &)> handler)
    {
        m_sendHandler = std::move(handler);
    }

    void setUiReadyHandler(std::function<void()> handler)
    {
        m_uiReadyHandler = std::move(handler);
    }

    void setNotifyHandler(std::function<void(
        const std::string &title, const std::string &message)> handler)
    {
        m_notifyHandler = std::move(handler);
    }

    void setThemeHandler(std::function<void(const std::string &theme)> handler)
    {
        m_themeHandler = std::move(handler);
    }
    using ThemeMenuHandler = std::function<void(
        int x, int y,
        const std::string &currentTheme,
        const std::string &currentPreset)>;
    void setThemeMenuHandler(ThemeMenuHandler handler)
    {
        m_themeMenuHandler = std::move(handler);
    }
    using ToolsMenuHandler = std::function<void(int x, int y, bool broadcastActive)>;
    void setToolsMenuHandler(ToolsMenuHandler handler)
    {
        m_toolsMenuHandler = std::move(handler);
    }
    using HelpMenuHandler = std::function<void(int x, int y)>;
    void setHelpMenuHandler(HelpMenuHandler handler)
    {
        m_helpMenuHandler = std::move(handler);
    }
    using RdpKeyMenuHandler = std::function<void(int x, int y)>;
    void setRdpKeyMenuHandler(RdpKeyMenuHandler handler)
    {
        m_rdpKeyMenuHandler = std::move(handler);
    }
    using ServerContextMenuHandler = std::function<void(
        int profileIndex, int x, int y, bool canMoveUp, bool canMoveDown)>;
    void setServerContextMenuHandler(ServerContextMenuHandler handler)
    {
        m_serverContextMenuHandler = std::move(handler);
    }
    using ExistingConnectionsMenuHandler = std::function<void(
        int x, int y, const std::vector<std::pair<int, std::string>> &items)>;
    void setExistingConnectionsMenuHandler(
        ExistingConnectionsMenuHandler handler)
    {
        m_existingConnectionsMenuHandler = std::move(handler);
    }

    void receiveMessage(const std::string &message);
    void poll();
    // Enters the shutdown state: stops accepting new requests and stops
    // polling.  Safe to call multiple times; poll() becomes a no-op.
    void beginShutdown();
    // Tears the backend down in a fixed order (tunnels, SSH sessions, serial
    // sessions, ConPTY sessions, then SFTP worker processes) so no child
    // process or thread is left behind.  Idempotent.
    void shutdown();
    bool requestCloseConfirmation();
    void setCloseDecisionHandler(std::function<void(const std::string &decision)> handler)
    {
        m_closeDecisionHandler = std::move(handler);
    }
    // Requests to open/close an embedded RDP session are forwarded to the
    // native window, which owns the RDP control and the WebView2 visibility.
    using RdpOpenHandler = std::function<void(
        const std::string &sessionId, int profileIndex,
        const std::string &host, int port,
        const std::string &user, const std::string &password,
        const RdpConnectionOptions &options)>;
    using RdpCloseHandler = std::function<void(
        const std::string &sessionId)>;
    using RdpLayoutHandler = std::function<void(
        const std::string &sessionId, bool visible, bool refresh,
        bool clearOcclusion, bool reflowAll)>;
    using RdpAdvancedEditorTransitionHandler = std::function<void(
        const std::string &sessionId, bool expanded,
        std::function<void(bool)> completion)>;
    using RdpContextMenuHandler = std::function<void(
        const std::string &sessionId, int x, int y)>;
    using RdpTabsContextMenuHandler = std::function<void(
        int x, int y, bool canCloseSplit)>;
    using RdpFullscreenHandler = std::function<void(
        const std::string &sessionId, bool enabled)>;
    using RdpFullscreenActionHandler = std::function<void(
        const std::string &sessionId, const std::string &action)>;
    void setRdpHandlers(RdpOpenHandler open, RdpCloseHandler close)
    {
        m_rdpOpenHandler = std::move(open);
        m_rdpCloseHandler = std::move(close);
    }
    void setRdpLayoutHandler(RdpLayoutHandler handler)
    {
        m_rdpLayoutHandler = std::move(handler);
    }
    void setRdpAdvancedEditorTransitionHandler(
        RdpAdvancedEditorTransitionHandler handler)
    {
        m_rdpAdvancedEditorTransitionHandler = std::move(handler);
    }
    void setRdpContextMenuHandler(RdpContextMenuHandler handler)
    {
        m_rdpContextMenuHandler = std::move(handler);
    }
    void setRdpTabsContextMenuHandler(RdpTabsContextMenuHandler handler)
    {
        m_rdpTabsContextMenuHandler = std::move(handler);
    }
    void setRdpFullscreenHandler(RdpFullscreenHandler handler)
    {
        m_rdpFullscreenHandler = std::move(handler);
    }
    void setRdpFullscreenActionHandler(RdpFullscreenActionHandler handler)
    {
        m_rdpFullscreenActionHandler = std::move(handler);
    }
    std::string closeBehavior() const;
    bool setCloseBehavior(const std::string &behavior);
    void startUpdateCheck();
    // Hidden diagnostic mode (T1-1 unified exit lifecycle): drives one real
    // SSH connect + data round-trip + SFTP worker list + ConPTY local session
    // cycle through the production backend and reports the result.  Only
    // used when MasterTerm starts with --exit-self-test, driven by
    // tools/connect-exit-stress.ps1.  Exit codes: 0 pass, 10 skipped (no
    // usable SSH profile), 11 failed.
    void startExitSelfTest(std::function<void(int exitCode)> completion);
    void notifyRdpState(
        const std::string &sessionId, const std::string &state);
    void notifyRdpQuality(
        const std::string &sessionId, const RdpQualitySnapshot &snapshot);
    void notifyRdpFullscreen(
        const std::string &sessionId, bool enabled);
    void notifyRdpFullscreenBar(
        const std::string &sessionId, bool visible);
    void notifyRdpFullscreenNativeBar(
        const std::string &sessionId, bool visible, bool pinned);
    void notifyRdpContextAction(
        const std::string &sessionId, const std::string &action);

private:
    void sendNativeResult(
        const NativeString &requestId, const NativeJsonDom::Value &result);
    void sendError(const NativeString &requestId, const NativeString &message);
    void sendNativeEvent(
        const std::string &event, const std::string &sessionId,
        const NativeJsonDom::Object &payload);
    void emitSessionOutput(
        const NativeString &sessionId, const std::string &output);
    void openSessionLog(
        const NativeString &sessionId, const std::string &name);
    void closeSessionLog(const NativeString &sessionId);
    void collectSensitiveValues();
    std::string redactSensitiveText(const std::string &text) const;
    void startSelfUpdateCheck(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    void startUpdateDownload(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    bool installUpdate(
        const NativeString &zipPath, NativeString &error) const;
    void startCloudAuthRequest(
        const NativeString &requestId, const NativeJsonDom::Object &params,
        bool registerMode);
    void startCloudSyncRequest(
        const NativeString &requestId, const NativeJsonDom::Object &params,
        bool push);
    void startCloudHistoryRequest(
        const NativeString &requestId, const NativeJsonDom::Object &params,
        const NativeString &operation);
    NativeJsonDom::Array exportCloudServers() const;
    NativeJsonDom::Value importCloudServers(
        const NativeJsonDom::Object &params, NativeString &error);
    bool replaceLocalCommandHistory(
        const NativeJsonDom::Object &params, NativeString &error) const;
    SshSession *sshSession(const NativeString &sessionId) const;
    SerialSession *serialSession(const NativeString &sessionId) const;
    LocalShellSession *localSession(const NativeString &sessionId) const;
    bool removeSession(const NativeString &sessionId);
    void pollExitSelfTest();
    void finishExitSelfTest(int exitCode);
    NativeJsonDom::Value createTunnel(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Value listTunnels(
        const NativeJsonDom::Object &params, NativeString &error) const;
    bool stopTunnel(
        const NativeJsonDom::Object &params, NativeString &error);
    bool saveTunnelConfig(
        const NativeJsonDom::Object &params, NativeString &error);
    bool removeTunnelConfig(
        const NativeJsonDom::Object &params, NativeString &error);
    void startConfiguredTunnels(int profileIndex, const std::string &sessionId);
    SshTunnel *findTunnel(
        const std::string &sessionId, const std::string &tunnelId) const;
    NativeJsonDom::Object tunnelToJson(
        const SshTunnel &tunnel, const std::string &sessionId) const;
    void startSftpList(const NativeString &requestId, int profileIndex, const NativeString &path);
    void finishSftpList(NativeProcess *process, const NativeString &requestId,
                        int profileIndex, const NativeString &path, int exitCode);
    void failSftpList(NativeProcess *process, const NativeString &requestId,
                      const NativeString &message);
    void startSftpHistory(const NativeString &requestId, int profileIndex,
                          const NativeJsonDom::Object &options);
    void finishSftpHistory(NativeProcess *process, const NativeString &requestId,
                           int profileIndex, int exitCode);
    void startSftpPreview(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    void finishSftpPreview(NativeProcess *process, int exitCode);
    void openRemoteFile(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    void finishRemoteEditDownload(NativeProcess *process, int exitCode);
    void finishRemoteEditUpload(NativeProcess *process, int exitCode);
    void pollRemoteEdits();
    void startRemoteEditUpload(const NativeString &editKey);
    bool retryRemoteEdit(const NativeJsonDom::Object &params, NativeString &error);
    bool stopRemoteEdit(const NativeJsonDom::Object &params, NativeString &error);
    bool openRemoteEditDirectory(const NativeJsonDom::Object &params, NativeString &error) const;
    void startSftpOperation(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    void finishSftpOperation(NativeProcess *process, const NativeString &requestId,
                             const NativeString &operation, const NativeString &path,
                             const NativeString &targetPath, int exitCode);
    void startSftpTransfer(
        const NativeString &requestId, const NativeJsonDom::Object &params);
    void processSftpTransferOutput(NativeProcess *process);
    bool detachSftpTransfer(NativeProcess *process, NativeString &transferId,
                            NativeString &temporaryPath);
    void finishSftpTransfer(NativeProcess *process, int exitCode);
    void failSftpTransfer(NativeProcess *process, const NativeString &message);
    bool cancelSftpTransfer(const NativeString &transferId);
bool setSftpTransferPaused(const NativeString &transferId, bool paused);
    bool writeSftpStreamChunk(const NativeJsonDom::Object &params,
                              std::int64_t &bufferedBytes, NativeString &error);
    bool finishSftpStream(const NativeJsonDom::Object &params, NativeString &error);
    bool querySftpStream(const NativeJsonDom::Object &params,
                         std::int64_t &bufferedBytes, NativeString &error) const;
    void startRemoteMonitor(const NativeString &sessionId, int profileIndex);
    void stopRemoteMonitor(const NativeString &sessionId);
    void processRemoteMonitorOutput(NativeProcess *process);
    void finishRemoteMonitor(NativeProcess *process, int exitCode);
    void startRemoteLatencyProbe(const NativeString &sessionId, int profileIndex);
    void stopRemoteLatencyProbe(const NativeString &sessionId);
    void finishRemoteLatencyProbe(NativeProcess *process, int exitCode);
    NativeJsonDom::Value createServerProfile(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Value updateServerProfile(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Value copyServerProfile(
        const NativeJsonDom::Object &params, NativeString &error);
    bool reorderServerProfile(
        const NativeJsonDom::Object &params, NativeString &error);
    bool deleteServerProfile(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Array workspaceNames() const;
    bool createWorkspace(
        const NativeJsonDom::Object &params, std::string &name,
        NativeString &error);
    bool renameWorkspace(
        const NativeJsonDom::Object &params, NativeString &error);
    bool deleteWorkspace(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Object appInfo() const;
    bool writeSystemClipboard(const NativeString &text) const;
    NativeString readSystemClipboard() const;
    NativeJsonDom::Array serverProfiles() const;
    NativeJsonDom::Object localDirectory(
        const NativeString &path, NativeString &error) const;
    NativeJsonDom::Object chooseLocalDirectory(
        const NativeJsonDom::Object &params, NativeString &error) const;
    NativeJsonDom::Object chooseLocalFile(
        const NativeJsonDom::Object &params) const;
    NativeJsonDom::Object chooseLocalProgram(
        const NativeJsonDom::Object &params) const;
    NativeJsonDom::Object exportConfig(NativeString &error) const;
    NativeJsonDom::Object previewConfigImport(NativeString &error);
    NativeJsonDom::Object applyConfigImport(
        const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Object restoreConfigBackup(NativeString &error) const;
    NativeJsonDom::Array knownHosts() const;
    NativeJsonDom::Array localCommandHistory() const;
    bool appendLocalCommandHistory(
        const NativeJsonDom::Object &params, NativeString &error) const;
    bool removeKnownHost(const NativeJsonDom::Object &params, NativeString &error) const;
    NativeJsonDom::Object operateLocalEntry(
        const NativeJsonDom::Object &params, NativeString &error) const;
    NativeJsonDom::Object beginDroppedFile(
        const NativeJsonDom::Object &params, NativeString &error);
    bool appendDroppedFile(const NativeJsonDom::Object &params, NativeString &error);
    NativeJsonDom::Object finishDroppedFile(
        const NativeJsonDom::Object &params, NativeString &error);
    bool removeLocalEntries(const NativeJsonDom::Object &params, NativeString &error);
    void discardDroppedFile(const NativeString &token);
    std::function<void(const std::string &)> m_sendHandler;
    std::function<void()> m_uiReadyHandler;
    std::function<void(const std::string &, const std::string &)>
        m_notifyHandler;
    std::function<void(const std::string &)> m_themeHandler;
    ThemeMenuHandler m_themeMenuHandler;
    ToolsMenuHandler m_toolsMenuHandler;
    HelpMenuHandler m_helpMenuHandler;
    RdpKeyMenuHandler m_rdpKeyMenuHandler;
    ServerContextMenuHandler m_serverContextMenuHandler;
    ExistingConnectionsMenuHandler m_existingConnectionsMenuHandler;
    std::function<void(const std::string &)> m_closeDecisionHandler;
    std::function<void(const std::string &, int, const std::string &, int,
                       const std::string &, const std::string &,
                       const RdpConnectionOptions &)>
        m_rdpOpenHandler;
    std::function<void(const std::string &)> m_rdpCloseHandler;
    std::function<void(const std::string &, bool, bool, bool, bool)>
        m_rdpLayoutHandler;
    RdpAdvancedEditorTransitionHandler
        m_rdpAdvancedEditorTransitionHandler;
    std::function<void(const std::string &, int, int)> m_rdpContextMenuHandler;
    RdpTabsContextMenuHandler m_rdpTabsContextMenuHandler;
    std::function<void(const std::string &, bool)> m_rdpFullscreenHandler;
    std::function<void(const std::string &, const std::string &)>
        m_rdpFullscreenActionHandler;
    std::atomic<bool> m_shuttingDown{false};
    // beginShutdown() only closes the gate for new work.  Destruction still
    // has to run the one-time cleanup that terminates SFTP worker processes.
    std::atomic<bool> m_shutdownComplete{false};
    std::unordered_map<NativeString, SshSession *, NativeStringHash> m_sshSessions;
    std::unordered_map<NativeString, SerialSession *, NativeStringHash> m_serialSessions;
    std::unordered_map<NativeString, LocalShellSession *, NativeStringHash> m_localSessions;
    std::unordered_map<NativeString, int, NativeStringHash> m_sessionProfiles;
    struct ExitSelfTestState {
        NativeString sshSessionId;
        NativeString localSessionId;
        int profileIndex = -1;
        int stage = 0;
        bool sshConnected = false;
        bool markerRequested = false;
        bool markerSeen = false;
        bool sftpStarted = false;
        bool localStarted = false;
        std::string sshError;
        std::string localError;
        std::string sshOutput;
        std::chrono::steady_clock::time_point stageStarted{};
    };
    std::unique_ptr<ExitSelfTestState> m_exitSelfTest;
    std::function<void(int)> m_exitSelfTestCompletion;
    std::unordered_map<std::string, std::vector<std::unique_ptr<SshTunnel>>>
        m_tunnels;
    bool m_sessionLoggingEnabled = false;
    std::unordered_map<std::string, std::ofstream> m_sessionLogs;
    std::vector<std::string> m_sensitiveValues;
    // Tunnel IDs that were started automatically from saved profile config,
    // keyed by session ID, so the UI can mark them and remove the config.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_automaticTunnelIds;
    struct SftpListRequest {
        NativeString requestId;
        int profileIndex = -1;
        NativeString path;
    };
    struct SftpOperationRequest {
        NativeString requestId;
        NativeString operation;
        NativeString path;
        NativeString targetPath;
    };
    struct SftpHistoryRequest {
        NativeString requestId;
        int profileIndex = -1;
        std::chrono::steady_clock::time_point startedAt;
    };
    struct SftpPreviewRequest {
        NativeString requestId;
        int profileIndex = -1;
        NativeString path;
    };
    struct RemoteEditDownload {
        NativeString requestId;
        NativeString editKey;
        NativeString localPath;
        NativeString remotePath;
        int profileIndex = -1;
        bool chooseApplication = false;
        NativeString applicationPath;
        NativeString conflict;
    };
    void processRemoteEditDownloadProgress(
        NativeProcess *process, const RemoteEditDownload &download);
    struct RemoteEdit {
        NativeString localPath;
        NativeString remotePath;
        int profileIndex = -1;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        std::chrono::steady_clock::time_point changedAt{};
        NativeString conflict;
        bool changed = false;
        bool uploadInFlight = false;
    };
    struct RemoteMonitorRequest {
        NativeString sessionId;
        int profileIndex = -1;
    };
    struct RemoteLatencyRequest {
        NativeString sessionId;
        bool isProxyJump = false;
        NativeString probeHost;
    };
    std::unordered_set<NativeProcess *> m_sftpProcesses;
    std::unordered_map<NativeProcess *, SftpListRequest> m_sftpListRequests;
    std::unordered_map<NativeProcess *, SftpOperationRequest> m_sftpOperationRequests;
    std::unordered_map<NativeProcess *, SftpHistoryRequest> m_sftpHistoryRequests;
    std::unordered_map<NativeProcess *, SftpPreviewRequest> m_sftpPreviewRequests;
    std::unordered_map<NativeProcess *, RemoteEditDownload> m_remoteEditDownloads;
    std::unordered_map<NativeProcess *, NativeString> m_remoteEditUploads;
    std::unordered_map<NativeString, RemoteEdit, NativeStringHash> m_remoteEdits;
    std::unordered_map<NativeProcess *, RemoteMonitorRequest> m_remoteMonitorRequests;
    std::unordered_map<NativeProcess *, std::string> m_remoteMonitorBuffers;
    std::unordered_map<NativeString, NativeProcess *, NativeStringHash> m_remoteMonitors;
    std::unordered_map<NativeProcess *, RemoteLatencyRequest> m_remoteLatencyRequests;
    std::unordered_map<NativeString, NativeProcess *, NativeStringHash> m_remoteLatencyProbes;
    std::unordered_map<NativeString, std::chrono::steady_clock::time_point,
                       NativeStringHash> m_remoteLatencyProbeTimes;
    std::unordered_map<NativeProcess *, NativeString> m_sftpTransferIds;
    std::unordered_map<NativeProcess *, NativeString> m_sftpTransferNames;
    // Dragged files are fed to the worker through stdin.  Keep track of the
    // bytes accepted by that pipe so the UI has useful progress feedback even
    // before the worker's first remote-write progress record arrives.
    std::unordered_map<NativeProcess *, std::int64_t> m_sftpStreamTotals;
    std::unordered_map<NativeProcess *, std::int64_t> m_sftpStreamQueued;
    std::unordered_set<NativeProcess *> m_sftpTransferRemoteComplete;
    std::unordered_map<NativeProcess *, std::string> m_sftpTransferBuffers;
    std::unordered_map<NativeProcess *, std::string> m_sftpStandardOutputs;
    std::unordered_map<NativeProcess *, std::string> m_sftpStandardErrors;
    std::map<NativeString, NativeProcess *> m_sftpTransfers;
    std::unordered_set<NativeProcess *> m_sftpStreamTransfers;
    std::unordered_map<NativeString, std::unique_ptr<std::ofstream>, NativeStringHash> m_droppedFiles;
    std::unordered_map<NativeString, NativeString, NativeStringHash> m_droppedFilePaths;
    std::unordered_map<NativeString, NativeJsonDom::Object, NativeStringHash>
        m_pendingConfigImports;
    std::unordered_map<NativeProcess *, NativeString> m_sftpTemporaryFiles;
    int m_nextSessionNumber = 1;
    int m_nextTransferNumber = 1;
};
