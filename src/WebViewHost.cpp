#include "WebViewHost.h"

#include "WebViewBackend.h"
#include "NativeJsonDom.h"
#include "DiagnosticLog.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <filesystem>
#include <fstream>
#include <array>
#include <cwchar>
#include <cmath>
#include <string>
#include <string_view>
#include <tlhelp32.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::filesystem::path executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path webViewUserDataDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    std::filesystem::path directory;
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        directory = std::filesystem::path(buffer) / L"MasterTerm" / L"WebView2";
    } else {
        std::error_code temporaryError;
        directory = std::filesystem::temp_directory_path(temporaryError)
            / L"MasterTerm" / L"WebView2";
        if (temporaryError)
            return {};
    }

    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    return directoryError ? std::filesystem::path() : directory;
}

std::filesystem::path abnormalExitMarkerPath()
{
    const std::filesystem::path directory = webViewUserDataDirectory();
    return directory.empty()
        ? std::filesystem::path()
        : directory / L"abnormal-exit.marker";
}

// Shared environment created by prewarm().  initialize() reuses it when it is
// already available; otherwise initialize() falls back to its own environment
// request, which shares the browser process group that prewarm() started.
Microsoft::WRL::ComPtr<ICoreWebView2Environment> g_prewarmedEnvironment;

// Hidden diagnostic hook for the WebView2 end-to-end harness
// (tools/webview-e2e.ps1 + tests/webview-e2e.js): when MASTERTERM_CDP_PORT
// is set, the WebView2 browser process exposes the Chrome DevTools Protocol
// on 127.0.0.1:<port> so the test can drive the real UI, capture screenshots
// and inspect the DOM.  Never enabled for normal runs.
Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> diagnosticEnvironmentOptions()
{
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> options;
    wchar_t buffer[32]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MASTERTERM_CDP_PORT", buffer,
        static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return options;
    const std::wstring port(buffer, length);
    bool digitsOnly = !port.empty() && port.size() <= 5;
    for (const wchar_t character : port) {
        if (character < L'0' || character > L'9')
            digitsOnly = false;
    }
    const int portNumber = digitsOnly ? _wtoi(port.c_str()) : 0;
    if (!digitsOnly || portNumber < 1024 || portNumber > 65535)
        return options;
    const auto created =
        Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!created)
        return options;
    const std::wstring arguments =
        L"--remote-debugging-port=" + port;
    if (FAILED(created->put_AdditionalBrowserArguments(arguments.c_str())))
        return options;
    created.As(&options);
    DiagnosticLog::write(
        "webview-cdp-enabled", "port=" + std::to_string(portNumber));
    return options;
}

std::string wideToUtf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) {
        length = MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), length);
        return result;
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

void reportWebViewError(const wchar_t *message, HRESULT result)
{
    wchar_t output[256]{};
    swprintf_s(output, L"%s (HRESULT 0x%08X)\n", message,
               static_cast<unsigned int>(result));
    OutputDebugStringW(output);
}

typedef LONG (NTAPI *NtQueryInformationProcessFunction)(
    HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ProcessBasicInformation
{
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

struct ProcessUnicodeString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct ProcessParameters
{
    BYTE Reserved1[16];
    PVOID Reserved2[10];
    ProcessUnicodeString ImagePathName;
    ProcessUnicodeString CommandLine;
};

std::wstring processCommandLine(DWORD processId)
{
    const HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!process)
        return {};
    const auto closeProcess = [&process] { CloseHandle(process); };
    static const NtQueryInformationProcessFunction queryInformation =
        reinterpret_cast<NtQueryInformationProcessFunction>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                           "NtQueryInformationProcess"));
    if (!queryInformation) {
        closeProcess();
        return {};
    }
    ProcessBasicInformation basic{};
    if (queryInformation(process, 0, &basic, sizeof(basic), nullptr) < 0
        || !basic.PebBaseAddress) {
        closeProcess();
        return {};
    }
    PVOID parametersAddress = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process,
            static_cast<BYTE *>(basic.PebBaseAddress) + 0x20,
            &parametersAddress, sizeof(parametersAddress), &bytesRead)
        || bytesRead != sizeof(parametersAddress)) {
        closeProcess();
        return {};
    }
    ProcessParameters parameters{};
    if (!ReadProcessMemory(process, parametersAddress, &parameters,
            sizeof(parameters), &bytesRead)
        || bytesRead != sizeof(parameters)
        || parameters.CommandLine.Length == 0
        || parameters.CommandLine.Length > 16384
        || !parameters.CommandLine.Buffer) {
        closeProcess();
        return {};
    }
    std::wstring commandLine(
        parameters.CommandLine.Length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(process, parameters.CommandLine.Buffer,
            commandLine.data(), parameters.CommandLine.Length, &bytesRead)
        || bytesRead != parameters.CommandLine.Length) {
        closeProcess();
        return {};
    }
    closeProcess();
    return commandLine;
}

// WebView2 browser processes are direct children of MasterTerm.exe.  When
// MasterTerm is killed forcefully (Task Manager, crash), those processes are
// orphaned and keep the WebView2 profile locked, which slows every later
// startup.  Clean them up before creating the environment, but only when no
// other MasterTerm instance is running.
void cleanupOrphanedWebViewProcesses()
{
    DWORD masterTermCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"MasterTerm.exe") == 0)
                ++masterTermCount;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (masterTermCount != 1)
        return;

    int terminated = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"msedgewebview2.exe") != 0)
                continue;
            const std::wstring commandLine =
                processCommandLine(entry.th32ProcessID);
            if (commandLine.find(L"--webview-exe-name=MasterTerm.exe")
                == std::wstring::npos)
                continue;
            const HANDLE process = OpenProcess(
                PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (process) {
                TerminateProcess(process, 1);
                CloseHandle(process);
                ++terminated;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (terminated > 0)
        DiagnosticLog::write(
            "webview-orphans-terminated", std::to_string(terminated));
}

} // namespace

struct WebViewHost::Lifetime
{
    WebViewHost *host = nullptr;
};

WebViewHost::WebViewHost(HWND parentWindow, WebViewBackend *backend)
    : m_parentWindow(parentWindow),
      m_backend(backend),
      m_uiThreadId(GetCurrentThreadId()),
      m_lifetime(std::make_shared<Lifetime>())
{
    m_lifetime->host = this;
}

void WebViewHost::postJsonToUiThread(const std::wstring &json)
{
    if (json.empty())
        return;
    if (GetCurrentThreadId() == m_uiThreadId) {
        sendJsonToWebView(json);
        return;
    }
    auto *payload = new std::wstring(json);
    if (!PostMessageW(
            m_parentWindow, sendJsonMessage(), 0,
            reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void WebViewHost::sendJsonToWebView(const std::wstring &json)
{
    if (m_webView && !json.empty())
        m_webView->PostWebMessageAsJson(json.c_str());
}

void WebViewHost::setWebViewVisible(bool visible)
{
    if (m_controller)
        m_controller->put_IsVisible(visible ? TRUE : FALSE);
}

void WebViewHost::waitForVisualCommit(std::function<void(bool)> callback)
{
    if (!callback)
        return;
    if (!m_webView) {
        callback(false);
        return;
    }

    ComPtr<IStream> previewStream;
    if (FAILED(CreateStreamOnHGlobal(
            nullptr, TRUE, previewStream.GetAddressOf()))
        || !previewStream) {
        callback(false);
        return;
    }

    auto completion = std::make_shared<std::function<void(bool)>>(
        std::move(callback));
    const HRESULT started = m_webView->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        previewStream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [completion, previewStream](HRESULT error) {
                if (*completion) {
                    auto callback = std::move(*completion);
                    callback(SUCCEEDED(error));
                }
                return S_OK;
            }).Get());
    if (FAILED(started) && *completion) {
        auto callback = std::move(*completion);
        callback(false);
    }
}

void WebViewHost::executeScript(const std::wstring &script)
{
    if (m_webView)
        m_webView->ExecuteScript(script.c_str(), nullptr);
}

void WebViewHost::focus()
{
    if (m_controller)
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::queryTerminalBounds(
    std::function<void(RdpLayoutSnapshot)> callback,
    int expectedRdpAdvancedEditorState)
{
    if (!callback)
        return;
    if (!m_webView) {
        callback({});
        return;
    }
    std::wstring script = L"(()=>{";
    if (expectedRdpAdvancedEditorState >= 0) {
        script += expectedRdpAdvancedEditorState != 0
            ? L"const a=document.querySelector('#rdp-advanced-settings');"
              L"const v=document.querySelector('#server-dialog');"
              L"if(!a||!v||!a.open||"
              L"!v.classList.contains('rdp-advanced-open'))return null;"
            : L"const a=document.querySelector('#rdp-advanced-settings');"
              L"const v=document.querySelector('#server-dialog');"
              L"if(!a||!v||a.open||"
              L"v.classList.contains('rdp-advanced-open'))return null;";
    }
    script +=
        L"const e=document.querySelector("
        L"'.terminal-panel.active[data-native-rdp=\"true\"]')||"
        L"document.querySelector('.terminal-panel.active');"
        L"if(!e)return null;const r=e.getBoundingClientRect();"
        L"const d=window.devicePixelRatio||1;"
        L"const m=document.querySelector('dialog[open]:not(.rdp-editor-fallback-freeze)')?1:0;"
        L"const o=[...document.querySelectorAll("
        L"'dialog[open]:not(.rdp-editor-fallback-freeze):not(#server-dialog):not(#settings-dialog),"
        L"#server-dialog[open]:not(.rdp-editor-fallback-freeze) > form,"
        L"#settings-dialog[open]:not(.rdp-editor-fallback-freeze) > form,"
        L"#sidebar-flyout-occlusion:not([hidden]),"
        L"#sidebar-card-shadow:not([hidden]),"
        L".context-menu:not([hidden]),"
        L".terminal-create-dropdown:not([hidden]),"
        L"#rdp-fullscreen-bar:not([hidden]),"
        L"#rdp-fullscreen-menu:not([hidden]),"
        L"#rdp-quality-panel:not([hidden]),"
        L"#rdp-fullscreen-hot-zone:not([hidden]),"
        L".rdp-connecting-notice:not([hidden])')].flatMap(e=>{"
        L"const r=e.getBoundingClientRect();const s=getComputedStyle(e);"
        L"const q=Math.max(0,parseFloat(s.borderTopLeftRadius)||0,"
        L"parseFloat(s.borderTopRightRadius)||0,"
        L"parseFloat(s.borderBottomRightRadius)||0,"
        L"parseFloat(s.borderBottomLeftRadius)||0);"
        L"const l=Math.round(r.left*d);const t=Math.round(r.top*d);"
        L"const r2=Math.round((r.left+r.width)*d);const b2=Math.round((r.top+r.height)*d);"
        L"const isShadow=e.id==='sidebar-card-shadow'?1:0;"
        L"return r2>l&&b2>t?[l,t,r2,b2,q*d,0,isShadow]:[];});"
        L"return [r.left*d,r.top*d,r.width*d,r.height*d,m,...o];})()";
    m_webView->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback = std::move(callback)](
                HRESULT error, LPCWSTR result) -> HRESULT {
                RdpLayoutSnapshot snapshot;
                if (FAILED(error) || !result) {
                    callback(snapshot);
                    return S_OK;
                }
                NativeJsonDom::Value parsed;
                if (!NativeJsonDom::parse(wideToUtf8(result), parsed)
                    || !parsed.isArray()
                    || parsed.array().values.size() < 5) {
                    callback(snapshot);
                    return S_OK;
                }
                const auto &values = parsed.array().values;
                auto numberAt = [&values](std::size_t index, double &value) {
                    if (index >= values.size() || !values[index].isNumber())
                        return false;
                    value = values[index].number();
                    return std::isfinite(value);
                };
                double left = 0.0;
                double top = 0.0;
                double width = 0.0;
                double height = 0.0;
                if (!numberAt(0, left) || !numberAt(1, top)
                    || !numberAt(2, width) || !numberAt(3, height)) {
                    callback(snapshot);
                    return S_OK;
                }
                snapshot.x = static_cast<int>(std::lround(left));
                snapshot.y = static_cast<int>(std::lround(top));
                snapshot.width = static_cast<int>(std::lround(width));
                snapshot.height = static_cast<int>(std::lround(height));
                double modalOpen = 0.0;
                if (!numberAt(4, modalOpen)) {
                    callback({});
                    return S_OK;
                }
                snapshot.modalOpen = modalOpen != 0.0;
                for (std::size_t index = 5; index + 6 < values.size(); index += 7) {
                    double overlayLeft = 0.0;
                    double overlayTop = 0.0;
                    double overlayRight = 0.0;
                    double overlayBottom = 0.0;
                    double cornerRadius = 0.0;
                    double isMenuFlag = 0.0;
                    double isShadowFlag = 0.0;
                    if (!numberAt(index, overlayLeft)
                        || !numberAt(index + 1, overlayTop)
                        || !numberAt(index + 2, overlayRight)
                        || !numberAt(index + 3, overlayBottom)
                        || !numberAt(index + 4, cornerRadius)
                        || !numberAt(index + 5, isMenuFlag)
                        || !numberAt(index + 6, isShadowFlag)
                        || overlayRight <= overlayLeft || overlayBottom <= overlayTop)
                        continue;
                    snapshot.occlusionRects.push_back(RdpOcclusionRegion{
                        RECT{
                            static_cast<LONG>(std::lround(overlayLeft)),
                            static_cast<LONG>(std::lround(overlayTop)),
                            static_cast<LONG>(std::lround(overlayRight)),
                            static_cast<LONG>(std::lround(overlayBottom))},
                        static_cast<LONG>(std::lround(cornerRadius)),
                        isMenuFlag != 0.0,
                        isShadowFlag != 0.0});
                }
                callback(std::move(snapshot));
                return S_OK;
            })
            .Get());
}

struct EmbeddedWebAsset {
    int resourceId;
    const wchar_t *relativePath;
};

constexpr std::array<EmbeddedWebAsset, 8> kEmbeddedWebAssets{{
    {103, L"index.html"},
    {104, L"app.css"},
    {105, L"app.js"},
    {106, L"vendor\\xterm\\LICENSE"},
    {107, L"vendor\\xterm\\xterm.css"},
    {108, L"vendor\\xterm\\xterm.js"},
    {109, L"vendor\\xterm-fit\\addon-fit.js"},
    {110, L"vendor\\xterm-fit\\LICENSE"},
}};

bool writeEmbeddedWebAsset(
    const EmbeddedWebAsset &asset, const std::filesystem::path &root)
{
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(asset.resourceId), RT_RCDATA);
    if (!resource)
        return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void *bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size == 0)
        return false;
    const std::filesystem::path target = root / asset.relativePath;
    std::error_code directoryError;
    std::filesystem::create_directories(target.parent_path(), directoryError);
    if (directoryError)
        return false;
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(
        static_cast<const char *>(bytes), static_cast<std::streamsize>(size));
    return output.good();
}

std::filesystem::path recoverEmbeddedWebAssets()
{
    std::wstring localData(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localData.data(),
        static_cast<DWORD>(localData.size()));
    if (length == 0 || length >= localData.size())
        return {};
    localData.resize(length);
    const std::filesystem::path root = std::filesystem::path(localData)
        / L"MasterTerm" / L"WebAssets" / MASTERTERM_VERSION_W;
    for (const EmbeddedWebAsset &asset : kEmbeddedWebAssets) {
        if (!writeEmbeddedWebAsset(asset, root))
            return {};
    }
    return root;
}

WebViewHost::~WebViewHost()
{
    shutdown();
    if (m_lifetime)
        m_lifetime->host = nullptr;
}

void WebViewHost::shutdown()
{
    if (m_backend)
        m_backend->setSendHandler({});
    if (m_lifetime)
        m_lifetime->host = nullptr;
    m_messageToken = {};
    m_acceleratorToken = {};
    m_webView.Reset();
    m_controller.Reset();
    m_ready = false;
    std::error_code ignored;
    std::filesystem::remove(abnormalExitMarkerPath(), ignored);
}

void WebViewHost::prewarm()
{
    const std::filesystem::path userDataDirectory = webViewUserDataDirectory();
    if (userDataDirectory.empty())
        return;
    if (std::filesystem::exists(abnormalExitMarkerPath()))
        cleanupOrphanedWebViewProcesses();
    const auto options = diagnosticEnvironmentOptions();
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT error, ICoreWebView2Environment *environment) -> HRESULT {
                if (SUCCEEDED(error) && environment) {
                    g_prewarmedEnvironment = environment;
                    DiagnosticLog::write("webview-prewarmed");
                }
                return S_OK;
            }).Get());
}

void WebViewHost::initialize()
{
    const std::weak_ptr<Lifetime> lifetime = m_lifetime;
    const HWND parentWindow = m_parentWindow;
    const std::filesystem::path userDataDirectory = webViewUserDataDirectory();
    if (!userDataDirectory.empty()) {
        // Only scan for and terminate orphaned WebView2 processes when the
        // previous run did not shut down cleanly.  A clean exit removes the
        // marker, so normal startups skip the process-table scan entirely.
        if (std::filesystem::exists(abnormalExitMarkerPath()))
            cleanupOrphanedWebViewProcesses();
        std::ofstream touch(
            abnormalExitMarkerPath(), std::ios::out | std::ios::trunc);
        touch.close();
    }
    const auto onEnvironmentReady =
        [lifetime, parentWindow](
            HRESULT error, ICoreWebView2Environment *environment) -> HRESULT {
                const auto state = lifetime.lock();
                WebViewHost *host = state ? state->host : nullptr;
                if (!host)
                    return S_OK;
                if (FAILED(error) || !environment) {
                    reportWebViewError(L"WebView2 environment creation failed", error);
                    host->m_startupError =
                        L"WebView2 运行时初始化失败，请先安装 WebView2 运行时"
                        L"（Release 页面下载 WebView2Setup.exe 后运行一次）。";
                    return error;
                }
                return environment->CreateCoreWebView2Controller(
                    parentWindow,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [lifetime](
                            HRESULT controllerError,
                            ICoreWebView2Controller *controller) -> HRESULT {
                            const auto state = lifetime.lock();
                            WebViewHost *host = state ? state->host : nullptr;
                            if (!host)
                                return S_OK;
                            if (FAILED(controllerError) || !controller) {
                                reportWebViewError(
                                    L"WebView2 controller creation failed",
                                    controllerError);
                                host->m_startupError =
                                    L"WebView2 界面创建失败，请关闭程序后重新打开；"
                                    L"若仍失败请更新 WebView2 运行时。";
                                return controllerError;
                            }

                            host->m_controller = controller;
                            if (FAILED(host->m_controller->get_CoreWebView2(
                                    &host->m_webView))
                                || !host->m_webView) {
                                reportWebViewError(
                                    L"Could not obtain WebView2 instance", E_FAIL);
                                return E_FAIL;
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(host->m_webView->get_Settings(&settings))
                                && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3)) && settings3)
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(host->m_controller.As(&controller2))) {
                                // Match the native splash while WebView2 is
                                // being revealed; this prevents a white frame
                                // between the splash and the first page paint.
                                const COREWEBVIEW2_COLOR background{255, 9, 18, 33};
                                controller2->put_DefaultBackgroundColor(background);
                            }

                            // Keep the controller hidden while the native
                            // splash is on screen so WebView2 cannot flicker
                            // while its first frames are produced.
                            // forceShow() reveals it once the frontend
                            // signals readiness.
                            host->m_controller->put_IsVisible(FALSE);
                            host->m_controller->add_AcceleratorKeyPressed(
                                Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                    [lifetime](
                                        ICoreWebView2Controller *,
                                        ICoreWebView2AcceleratorKeyPressedEventArgs *args)
                                        -> HRESULT {
                                        const auto state = lifetime.lock();
                                        WebViewHost *host = state ? state->host : nullptr;
                                        if (!host || !host->m_webView || !args)
                                            return S_OK;
                                        COREWEBVIEW2_KEY_EVENT_KIND kind{};
                                        UINT key = 0;
                                        args->get_KeyEventKind(&kind);
                                        args->get_VirtualKey(&key);
                                        if ((kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
                                             || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                                            && key == VK_F11) {
                                            args->put_Handled(TRUE);
                                            host->m_webView->PostWebMessageAsJson(
                                                LR"({"event":"app.shortcut","payload":{"name":"rdp-toggle-fullscreen"}})");
                                            return S_OK;
                                        }
                                        if ((kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
                                             || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                                            && (key == VK_SPACE)
                                            && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                                            args->put_Handled(TRUE);
                                            host->m_webView->PostWebMessageAsJson(
                                                LR"({"event":"app.shortcut","payload":{"name":"history-completion"}})");
                                            return S_OK;
                                        }
                                        if ((kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
                                             || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                                            && (key == 'W' || key == 'w')
                                            && (GetKeyState(VK_CONTROL) & 0x8000) != 0
                                            && (GetKeyState(VK_SHIFT) & 0x8000) == 0
                                            && (GetKeyState(VK_MENU) & 0x8000) == 0) {
                                            args->put_Handled(TRUE);
                                            host->m_webView->PostWebMessageAsJson(
                                                LR"({"event":"app.shortcut","payload":{"name":"close-tab"}})");
                                            return S_OK;
                                        }
                                        if ((kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
                                             || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                                            && (key == VK_OEM_COMMA)
                                            && (GetKeyState(VK_CONTROL) & 0x8000) != 0
                                            && (GetKeyState(VK_SHIFT) & 0x8000) == 0
                                            && (GetKeyState(VK_MENU) & 0x8000) == 0) {
                                            args->put_Handled(TRUE);
                                            host->m_webView->PostWebMessageAsJson(
                                                LR"({"event":"app.shortcut","payload":{"name":"open-settings"}})");
                                            return S_OK;
                                        }
                                        return S_OK;
                                    }).Get(),
                                &host->m_acceleratorToken);
                            host->m_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [lifetime](
                                        ICoreWebView2 *,
                                        ICoreWebView2WebMessageReceivedEventArgs *args)
                                        -> HRESULT {
                                        const auto state = lifetime.lock();
                                        WebViewHost *host = state ? state->host : nullptr;
                                        if (!host)
                                            return S_OK;
                                        LPWSTR rawMessage = nullptr;
                                        if (FAILED(args->get_WebMessageAsJson(&rawMessage))
                                            || !rawMessage)
                                            return S_OK;
                                        const std::string message =
                                            wideToUtf8(rawMessage);
                                        CoTaskMemFree(rawMessage);
                                        if (host->m_backend)
                                            host->m_backend->receiveMessage(message);
                                        return S_OK;
                                    })
                                    .Get(),
                                &host->m_messageToken);

                            if (host->m_backend) {
                                host->m_backend->setSendHandler(
                                    [lifetime](const std::string &message) {
                                        const auto state = lifetime.lock();
                                        WebViewHost *host =
                                            state ? state->host : nullptr;
                                        if (!host || !host->m_webView)
                                            return;
                                        host->postJsonToUiThread(
                                            utf8ToWide(message));
                                    });
                            }

                            host->m_webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [lifetime](
                                        ICoreWebView2 *,
                                        ICoreWebView2NavigationCompletedEventArgs *args)
                                        -> HRESULT {
                                        const auto state = lifetime.lock();
                                        WebViewHost *host =
                                            state ? state->host : nullptr;
                                        if (!host || !host->m_controller
                                            || host->m_ready)
                                            return S_OK;
                                        BOOL succeeded = FALSE;
                                        if (args)
                                            args->get_IsSuccess(&succeeded);
                                        (void)succeeded;
                                        return S_OK;
                                    }).Get(),
                                &host->m_navigationToken);
                            host->updateBounds();
                            host->navigateFrontend();
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            };
    if (g_prewarmedEnvironment) {
        onEnvironmentReady(S_OK, g_prewarmedEnvironment.Get());
        return;
    }
    const auto options = diagnosticEnvironmentOptions();
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDirectory.empty() ? nullptr : userDataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            onEnvironmentReady)
            .Get());
    if (FAILED(result))
        reportWebViewError(L"WebView2 startup failed", result);
}

void WebViewHost::forceShow()
{
    if (!m_controller || m_ready)
        return;
    m_ready = true;
    m_controller->put_IsVisible(TRUE);
    updateBounds();
}

void WebViewHost::navigateFrontend()
{
    if (!m_webView)
        return;

    Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(m_webView.As(&webView3))) {
        const auto mapWebRoot = [&webView3](
            const std::filesystem::path &root) {
            std::error_code fileError;
            return !root.empty()
                && std::filesystem::exists(root / L"index.html", fileError)
                && SUCCEEDED(webView3->SetVirtualHostNameToFolderMapping(
                    L"masterterm.local", root.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS));
        };
        const std::filesystem::path externalRoot =
            executableDirectory() / L"web";
        if (mapWebRoot(externalRoot)
            || mapWebRoot(recoverEmbeddedWebAssets())) {
            m_webView->Navigate(L"https://masterterm.local/index.html");
            return;
        }
    }

    m_webView->NavigateToString(
        LR"(<html><body style="margin:0;background:#111827;color:#e5e7eb;font:14px Segoe UI;display:grid;place-items:center;height:100vh"><div>MasterTerm 无法加载内置或外部前端资源，请重新解压完整更新包。</div></body></html>)");
}

void WebViewHost::resize()
{
    updateBounds();
}

void WebViewHost::updateBounds()
{
    if (!m_controller || !IsWindow(m_parentWindow))
        return;
    RECT bounds{};
    GetClientRect(m_parentWindow, &bounds);
    m_controller->put_Bounds(bounds);
}

void WebViewHost::notifyFrontendResize()
{
    if (!m_webView || !m_ready)
        return;
    m_webView->ExecuteScript(
        L"window.dispatchEvent(new Event('masterterm-host-resize'));",
        nullptr);
}
