#pragma once

#include "RdpConnectionOptions.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class WebViewBackend;
class WebViewHost;
class RdpSession;
struct RdpQualitySnapshot;

class WebViewWindow final
{
public:
    WebViewWindow();
    ~WebViewWindow();

    WebViewWindow(const WebViewWindow &) = delete;
    WebViewWindow &operator=(const WebViewWindow &) = delete;

    bool create(HINSTANCE instance, int showCommand);
    HWND handle() const { return m_window; }
    // Hidden diagnostic mode used by tools/connect-exit-stress.ps1: after the
    // WebView2 frontend is ready, the backend drives one SSH connect + SFTP
    // worker + ConPTY cycle and the window exits with the result code.
    void enableExitSelfTest() { m_exitSelfTestEnabled = true; }

private:
    struct NativePopupMenuItem;
    bool showStartupSplash() const;
    static LRESULT CALLBACK windowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool registerWindowClass(HINSTANCE instance);
    void shutdown();
    void saveWindowPlacement();
    void addTrayIcon();
    void removeTrayIcon();
    void hideToTray();
    void restoreFromTray();
    void showTrayNotification(
        const std::wstring &title, const std::wstring &message);
    void showTrayMenu(const POINT &cursor);
    void showThemeMenu(
        int x, int y,
        const std::string &currentTheme = "",
        const std::string &currentPreset = "");
    void showToolsMenu(int x, int y, bool broadcastActive = false);
    void showHelpMenu(int x, int y);
    void showRdpKeyMenu(int x, int y);
    void requestExit();
    int showCloseDialog();
    bool ensureSplashCache(
        HDC dc, int width, int height, const RECT &client,
        bool showBar, const wchar_t *message);
    void destroySplashCache();
    void openRdpSession(
        const std::string &sessionId, int profileIndex,
        const std::wstring &host, int port,
        const std::wstring &user, const std::wstring &password,
        const RdpConnectionOptions &options);
    void configureRdpSession(
        const std::string &sessionId, RdpSession *session);
    void closeRdpSession(const std::string &sessionId);
    bool recreateRdpSession(
        const std::string &sessionId, bool useMultimon);
    void layoutRdpSession(
        const std::string &sessionId, bool hostedVisible,
        bool refreshDisplaySettings,
        bool clearOcclusion = false);
    void transitionRdpAdvancedEditor(
        const std::string &sessionId, bool expanded,
        std::function<void(bool)> completion);
    void reflowAllRdpSessions(bool clearOcclusion = false);
    void notifyRdpState(
        const std::string &sessionId, const std::wstring &state);
    void notifyRdpQuality(
        const std::string &sessionId, const RdpQualitySnapshot &snapshot);
    void notifyRdpState(const std::wstring &state);
    // A terminal RDP error is delivered from an ActiveX callback. Releasing
    // the COM/OLE host in that callback is unsafe, so queue the destruction
    // until the callback has returned to the window message loop.
    void scheduleFailedRdpCleanup(const std::string &sessionId);
    void cleanupFailedRdpSessions();
    void cleanupFailedRdpSession(
        const std::string &sessionId, unsigned long long token);
    void setRdpFullscreen(const std::string &sessionId, bool enabled);
    void setApplicationTheme(const std::string &theme);
    void notifyRdpFullscreen();
    void ensureRdpFullscreenOverlay();
    void updateRdpFullscreenOverlay();
    void setRdpFullscreenOverlayVisible(bool visible);
    void pollRdpFullscreenOverlay();
    void handleRdpFullscreenOverlayAction(int button);
    void showRdpFullscreenOverlayMenu(int menu);
    int rdpFullscreenOverlayButtonAt(POINT point) const;
    int rdpFullscreenOverlayPopupButtonAt(POINT point) const;
    void paintRdpFullscreenOverlay(HDC dc) const;
    void paintRdpFullscreenOverlayPopup(HDC dc) const;
    static LRESULT CALLBACK rdpFullscreenOverlayProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensureNativePopupMenuClass();
    UINT showNativePopupMenu(
        int screenX, int screenY,
        std::vector<NativePopupMenuItem> items, int width = 150);
    void closeNativePopupMenu();
    int nativePopupMenuHeight(UINT dpi) const;
    int nativePopupMenuItemAt(POINT point) const;
    void paintNativePopupMenu(HDC dc) const;
    static LRESULT CALLBACK nativePopupMenuProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void showRdpContextMenu(
        const std::string &sessionId, int x, int y);
    void showServerContextMenu(
        int profileIndex, int x, int y,
        bool canMoveUp, bool canMoveDown);
    void showExistingConnectionsMenu(
        int x, int y, const std::vector<std::pair<int, std::string>> &items);
    void showRdpTabsContextMenu(int x, int y, bool canCloseSplit);
    void dismissActiveModal(bool lightDismissOnly = false);
    struct RdpHostedSession;
    RdpHostedSession *findRdpSession(const std::string &sessionId);

    static constexpr UINT_PTR BackendPollTimer = 1;
    static constexpr UINT_PTR ResizeSettledTimer = 2;
    static constexpr UINT_PTR RdpFullscreenOverlayTimer = 3;
    static constexpr UINT_PTR RdpFullscreenValidationTimer = 4;
    static constexpr UINT RdpNativeFullscreenChangedMessage = WM_APP + 42;
    static constexpr UINT RdpFailedCleanupMessage = WM_APP + 43;
    static constexpr DWORD ShutdownTimeoutMs = 8000;
    static constexpr UINT TrayIconId = 1;
    enum TrayCommand {
        TrayCommandRestore = 1,
        TrayCommandExit = 2,
    };
    enum CloseChoice {
        CloseChoiceTray = 100,
        CloseChoiceExit = 101,
    };
    enum RdpContextCommand {
        RdpContextReconnect = 200,
        RdpContextClose = 201,
        RdpContextCloseOthers = 202,
        RdpContextCloseAll = 203,
        RdpContextMoveLeft = 204,
        RdpContextMoveRight = 205,
        RdpContextFullscreen = 206,
    };
    enum RdpTabsContextCommand {
        RdpTabsNewLocal = 250,
        RdpTabsNewLocalPwsh = 251,
        RdpTabsNewLocalPowerShell = 252,
        RdpTabsNewLocalCmd = 253,
        RdpTabsNewLocalWsl = 254,
        RdpTabsNewLocalGitBash = 255,
        RdpTabsNewSsh = 256,
        RdpTabsNewRdp = 257,
        RdpTabsCloseAll = 258,
        RdpTabsCloseSplit = 259,
        RdpTabsBatchCommand = 260,
        RdpTabsSortName = 261,
        RdpTabsSortType = 262,
    };
public:
    enum ThemeMenuCommand {
        ThemeMenuDark = 300,
        ThemeMenuLight = 301,
        ThemeMenuBlue = 302,

        ThemePresetDarkCustom = 310,
        ThemePresetDarkOneDark = 311,
        ThemePresetDarkDracula = 312,
        ThemePresetDarkNord = 313,
        ThemePresetDarkMonokai = 314,
        ThemePresetDarkSolarizedDark = 315,
        ThemePresetDarkCatppuccinMocha = 316,
        ThemePresetDarkGithubDark = 317,

        ThemePresetLightCustom = 330,
        ThemePresetLightGithubLight = 331,
        ThemePresetLightOneLight = 332,
        ThemePresetLightSolarizedLight = 333,

        ThemePresetBlueCustom = 350,
    };
private:
    enum ServerContextMenuCommand {
        ServerContextConnect = 400,
        ServerContextEdit = 401,
        ServerContextCopy = 402,
        ServerContextMoveUp = 403,
        ServerContextMoveDown = 404,
        ServerContextBatchWorkspace = 405,
        ServerContextBatchColor = 406,
        ServerContextBatchIcon = 407,
        ServerContextBatchDelete = 408,
        ServerContextDelete = 409,
    };
    static constexpr UINT ExistingConnectionMenuCommand = 500;
    enum RdpKeyMenuCommand {
        RdpKeyCmdCad = 600,
        RdpKeyCmdWin = 601,
        RdpKeyCmdRun = 602,
        RdpKeyCmdTaskMgr = 603,
        RdpKeyCmdExplorer = 604,
        RdpKeyCmdCmd = 605,
        RdpKeyCmdPrtScn = 606,
    };
    enum ToolsMenuCommand {
        ToolsCmdBroadcast = 700,
        ToolsCmdTunnel = 701,
        ToolsCmdBatchCmd = 702,
        ToolsCmdCloudSync = 703,
        ToolsCmdDiagnostics = 704,
    };
    enum HelpMenuCommand {
        HelpCmdShortcuts = 800,
        HelpCmdCheckUpdates = 801,
        HelpCmdAbout = 802,
    };
    struct NativePopupMenuItem {
        UINT command = 0;
        std::wstring label;
        bool separator = false;
        bool disabled = false;
    };

    HWND m_window = nullptr;
    UINT m_trayMessage = 0;
    WINDOWPLACEMENT m_trayRestorePlacement{sizeof(WINDOWPLACEMENT)};
    bool m_trayHidden = false;
    HBRUSH m_backgroundBrush = nullptr;
    HDC m_splashBackgroundDc = nullptr;
    HBITMAP m_splashBackground = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> m_splashLogo;
    ULONG_PTR m_gdiplusToken = 0;
    int m_splashBackgroundWidth = 0;
    int m_splashBackgroundHeight = 0;
    bool m_splashCacheShowsBar = false;
    std::wstring m_splashCacheMessage;
    std::unique_ptr<WebViewBackend> m_backend;
    std::unique_ptr<WebViewHost> m_host;
    bool m_shuttingDown = false;
    std::thread m_shutdownWatchdog;
    HANDLE m_shutdownDoneEvent = nullptr;
    unsigned int m_startupTicks = 0;
    bool m_startupReady = false;
    unsigned int m_startupReadyTicks = 0;
    bool m_exitRequested = false;
    bool m_closeDialogPending = false;
    bool m_exitSelfTestEnabled = false;
    bool m_exitSelfTestStarted = false;
    bool m_exitSelfTestDone = false;
    int m_exitSelfTestExitCode = 1;
    std::unordered_map<std::string, std::unique_ptr<RdpHostedSession>>
        m_rdpSessions;
    // A monotonically increasing token prevents a deferred cleanup for an old
    // session from deleting a replacement that reuses the same session id.
    std::unordered_map<std::string, unsigned long long>
        m_rdpFailedCleanupTokens;
    unsigned long long m_rdpFailureCleanupToken = 0;
    std::string m_activeRdpSessionId;
    // The session selected by the frontend can change again while an
    // asynchronous WebView bounds query is pending. Track the native surface
    // that is actually on top separately so a cancelled handoff never lowers
    // the wrong RDP window and exposes a black frame.
    std::string m_presentedRdpSessionId;
    bool m_rdpHostedVisible = false;
    bool m_rdpModalDimmed = false;
    bool m_rdpFullscreen = false;
    bool m_rdpFullscreenTransition = false;
    std::string m_rdpFullscreenSessionId;
    LONG_PTR m_rdpNormalStyle = 0;
    LONG_PTR m_rdpNormalExStyle = 0;
    WINDOWPLACEMENT m_rdpNormalPlacement{};
    RECT m_rdpNormalWindowRect{};
    bool m_rdpNormalWindowRectValid = false;
    RECT m_rdpNormalHostRect{};
    bool m_rdpNormalHostRectValid = false;
    bool m_rdpFullscreenUseMultimon = false;
    bool m_rdpMainWindowDemotedForNativeFullscreen = false;
    unsigned long long m_rdpLayoutGeneration = 0;
    HWND m_rdpFullscreenOverlay = nullptr;
    HWND m_rdpFullscreenOverlayPopup = nullptr;
    HWND m_nativePopupMenuWindow = nullptr;
    std::vector<NativePopupMenuItem> m_nativePopupMenuItems;
    int m_nativePopupMenuWidth = 150;
    UINT m_nativePopupMenuCommand = 0;
    int m_nativePopupMenuHover = -1;
    bool m_nativePopupMenuClosing = false;
    // The toolbar is transient by default; pinning is an explicit action.
    bool m_rdpFullscreenOverlayPinned = false;
    bool m_rdpFullscreenOverlayVisible = false;
    ULONGLONG m_rdpFullscreenOverlayHideAt = 0;
    ULONGLONG m_rdpFullscreenOverlayHoverAt = 0;
    int m_rdpFullscreenOverlayHover = -1;
    int m_rdpFullscreenOverlayMenu = 0;
    int m_rdpFullscreenOverlayTheme = 0;
};
