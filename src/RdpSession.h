#pragma once

#include "RdpOcclusionRegion.h"

#include "RdpConnectionOptions.h"

#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <olectl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct RdpQualitySnapshot {
    int colorDepth = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    bool smartSizing = false;
    int performanceFlags = 0;
    int networkConnectionType = 0;
    bool bandwidthDetection = false;
    int clientProtocolSpec = 0;
    int fullFrameRefreshCount = 0;
    unsigned long lastHresult = 0;
    bool lastHresultSucceeded = true;
    std::wstring lastOperation;
    // mstscax exposes the selected protocol mode, but does not expose the
    // negotiated AVC/H.264 codec through the embedded interface. Keep that
    // distinction visible instead of implying that FullMode means AVC.
    std::wstring avcStatus = L"未由嵌入式 RDP 控件暴露";
};

// Embedded Microsoft RDP ActiveX host (mstscax.dll, "MsTscAx.MsTscAx.12").
// Renders the remote desktop inside the main window instead of launching a
// separate mstsc.exe window.
//
// Requires the RDP control COM class to be registered:
//   regsvr32 C:\Windows\System32\mstscax.dll
// (Windows 10/11 do not register it by default; MasterTerm checks and
//  prompts for registration when an RDP connection is first used.)

class RdpSession final {
public:
    RdpSession(HWND parent);
    ~RdpSession();

    RdpSession(const RdpSession &) = delete;
    RdpSession &operator=(const RdpSession &) = delete;

    // Creates the ActiveX container window inside the parent. Returns false
    // when the RDP control is not registered on this machine.
    bool create();

    void connect(const std::wstring &host, int port,
                 const std::wstring &user, const std::wstring &password,
                 const RdpConnectionOptions &options = {});
    void disconnect();
    // Moves the embedded control above or below the WebView without changing
    // the RDP connection.  Connected background controls stay visible in the
    // native z-order so mstscax keeps their last rendered desktop surface.
    void setHostedVisible(bool visible);
    // Removes any temporary region holes synchronously when an HTML overlay
    // closes, so the RDP surface can repaint the area immediately.
    void clearOcclusionRegion();
    // Display and remote utility commands exposed by the fullscreen toolbar.
    void setSmartSizing(bool enabled);
    // Applies multi-monitor mode before Connect(). The embedded ActiveX
    // control rejects changing this property after a session is connected.
    bool setUseMultimon(bool enabled);
    // In this mode MasterTerm supplies the borderless monitor-spanning
    // container and mstscax remains embedded instead of creating a second
    // UIMainClass fullscreen window.
    void setContainerHandledFullscreen(bool enabled)
    {
        m_containerHandledFullscreen = enabled;
    }
    // Lets mstscax own its native fullscreen windows. True multi-monitor
    // rendering is not a single child-window surface and must be entered
    // only after the control reports that the RDP session is connected.
    bool setNativeFullscreen(bool enabled);
    // Verifies that a negotiated multi-monitor desktop is actually presented
    // across the local monitor set. Some older mstscax controls report
    // FullScreen=TRUE while placing the combined desktop in a one-monitor
    // viewport with scroll bars.
    bool validateNativeFullscreenPresentation();
    void launchRemoteUtility(const std::wstring &command);
    void sendCtrlAltDelete();
    void sendCtrlAltEnd();
    void sendWindowsKey();
    void sendPrintScreen();
    void resize(
        int x, int y, int width, int height,
        bool updateDisplaySettings = true,
        const std::vector<RdpOcclusionRegion> &occlusionRects = {},
        bool modalDimmed = false);
    HWND window() const { return m_container; }
    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    bool hostBounds(int &x, int &y, int &width, int &height) const
    {
        if (m_hostWidth <= 0 || m_hostHeight <= 0)
            return false;
        x = m_hostX;
        y = m_hostY;
        width = m_hostWidth;
        height = m_hostHeight;
        return true;
    }

    // Fired with "connecting" / "connected" / "disconnected" / "error:<msg>".
    void setStateHandler(std::function<void(const std::wstring &)> handler);
    // Receives F11/Esc while the native RDP child window has keyboard focus.
    void setKeyHandler(std::function<void(UINT)> handler)
    {
        m_keyHandler = std::move(handler);
    }
    void setMouseHandler(std::function<void(bool)> handler)
    {
        m_mouseHandler = std::move(handler);
    }
    void setQualityHandler(
        std::function<void(const RdpQualitySnapshot &)> handler)
    {
        m_qualityHandler = std::move(handler);
    }
    void setNativeFullscreenHandler(
        std::function<void(bool)> handler)
    {
        m_nativeFullscreenHandler = std::move(handler);
    }
    void setDismissModalHandler(std::function<void()> handler)
    {
        m_dismissModalHandler = std::move(handler);
    }
    const RdpQualitySnapshot &qualitySnapshot() const
    {
        return m_quality;
    }

private:
    static LRESULT CALLBACK controlWindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void applyOcclusionRegion(
        int x, int y, int width, int height,
        const std::vector<RdpOcclusionRegion> &occlusionRects);
    void redrawOcclusionSurface();
    void updateShadowOverlay(
        int x, int y, int width, int height,
        const std::vector<RdpOcclusionRegion> &occlusionRects);
    void updateShadowOverlay();
    void updateModalDimOverlay(
        int x, int y, int width, int height, bool dimmed,
        const std::vector<RdpOcclusionRegion> &occlusionRects);
    void scheduleSurfaceRefresh(bool requestDisplayUpdate = true);
    void focusControlForKeyboard();
    void repaintDesktopSurface();
    void emitQuality();
    UINT sendInputSequence(
        const WORD *virtualKeys, const DWORD *flags, UINT count);
    void emitState(const std::wstring &state);
    void writeLog(const std::wstring &line);
    std::wstring logPath() const;
    // ActiveX container plumbing.
    class ClientSite;
    class InPlaceSite;
    class EventSink;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    HWND m_parent = nullptr;
    HWND m_container = nullptr;
    HWND m_dimOverlay = nullptr;
    HWND m_shadowOverlay = nullptr;
    std::function<void(const std::wstring &)> m_stateHandler;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_created = false;
    bool m_visible = false;
    bool m_hostedVisible = false;
    bool m_loopbackConnection = false;
    bool m_lanConnection = false;
    // UpdateSessionDisplaySettings accepts one desktop rectangle and would
    // flatten a multi-monitor session into a single virtual display. When
    // UseMultimon is enabled, mstscax negotiates the monitor topology during
    // Connect(); later resize/refresh passes must not send a single virtual
    // rectangle back to the control.
    bool m_useMultimon = false;
    bool m_containerHandledFullscreen = false;
    std::wstring m_controlProgId;
    std::wstring m_controlVersion;
    int m_surfaceRefreshPass = 0;
    bool m_surfaceRefreshNeedsDisplayUpdate = false;
    bool m_displaySettingsSynchronized = false;
    std::wstring m_lastState;
    int m_displayWidth = 0;
    int m_displayHeight = 0;
    int m_hostX = 0;
    int m_hostY = 0;
    int m_hostWidth = 0;
    int m_hostHeight = 0;
    int m_occlusionHostX = 0;
    int m_occlusionHostY = 0;
    int m_occlusionHostWidth = 0;
    int m_occlusionHostHeight = 0;
    bool m_occlusionStateValid = false;
    std::vector<RdpOcclusionRegion> m_appliedOcclusionRects;
    RECT m_shadowBounds{};
    bool m_shadowBoundsValid = false;
    HWND m_controlWindow = nullptr;
    WNDPROC m_originalControlProcedure = nullptr;
    std::function<void(UINT)> m_keyHandler;
    std::function<void(bool)> m_mouseHandler;
    std::function<void(const RdpQualitySnapshot &)> m_qualityHandler;
    std::function<void(bool)> m_nativeFullscreenHandler;
    std::function<void()> m_dismissModalHandler;
    bool m_modalDimmed = false;
    RdpQualitySnapshot m_quality;
};
