#pragma once

#include "RdpOcclusionRegion.h"

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>

#include <memory>
#include <string>
#include <functional>
#include <vector>

class WebViewBackend;

class WebViewHost final
{
public:
    WebViewHost(HWND parentWindow, WebViewBackend *backend);
    ~WebViewHost();

    WebViewHost(const WebViewHost &) = delete;
    WebViewHost &operator=(const WebViewHost &) = delete;

    // Start creating the WebView2 environment as early as possible so the
    // browser process group boots in parallel with window creation.  Safe to
    // call before a message loop exists; the completion callback is pumped by
    // the first GetMessage cycle and initialize() reuses the environment.
    static void prewarm();

    void initialize();
    void forceShow();
    void resize();
    void notifyFrontendResize();
    void shutdown();
    // ICoreWebView2 is not thread-safe: PostWebMessageAsJson must run on the
    // thread that created the controller.  Backend worker threads use
    // postJsonToUiThread(), which either dispatches inline when already on the
    // UI thread or posts a window message that the message loop forwards to
    // sendJsonToWebView().
    static constexpr UINT sendJsonMessage() { return WM_APP + 41; }
    void postJsonToUiThread(const std::wstring &json);
    void sendJsonToWebView(const std::wstring &json);
    // Toggles the WebView2 controller visibility.  Used while an embedded
    // RDP session covers the client area.
    void setWebViewVisible(bool visible);
    void executeScript(const std::wstring &script);
    void focus();
    struct RdpLayoutSnapshot {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool modalOpen = false;
        // Physical-pixel rectangles in the WebView/controller coordinate
        // space. The native RDP host leaves these areas uncovered so HTML
        // dialogs and menus remain visible above the embedded desktop.
        std::vector<RdpOcclusionRegion> occlusionRects;
    };
    // Returns the active native-RDP panel's client bounds in the
    // WebView/controller coordinate space. In split mode this selects only
    // the RDP tab's pane; otherwise it falls back to the terminal panel area.
    void queryTerminalBounds(
        std::function<void(RdpLayoutSnapshot)> callback,
        int expectedRdpAdvancedEditorState = -1);
    // Capturing a preview is used as a compositor barrier: completion means
    // WebView2 has produced pixels for the current DOM state, not merely that
    // ExecuteScript has observed its target bounds.
    void waitForVisualCommit(std::function<void(bool)> callback);
    // The controller is created before the frontend has finished loading.
    // This lets the native splash report the actual startup phase instead of
    // showing the WebView2 environment message for the whole transition.
    bool isControllerReady() const { return m_controller != nullptr; }
    bool isReady() const { return m_ready; }
    bool hasStartupError() const { return !m_startupError.empty(); }
    const std::wstring &startupError() const { return m_startupError; }

private:
    struct Lifetime;

    void navigateFrontend();
    void updateBounds();

    HWND m_parentWindow = nullptr;
    WebViewBackend *m_backend = nullptr;
    DWORD m_uiThreadId = 0;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    EventRegistrationToken m_messageToken{};
    EventRegistrationToken m_acceleratorToken{};
    EventRegistrationToken m_navigationToken{};
    std::shared_ptr<Lifetime> m_lifetime;
    bool m_ready = false;
    std::wstring m_startupError;
};
