#include "RdpSession.h"
#include "NativeDataDir.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

// Import the MSTSCLib type definitions from mstscax.dll.  raw_interfaces_only
// keeps the generated wrapper small; we call the interfaces through the
// generated smart pointers (_com_ptr_t).
#import "C:\\Windows\\System32\\mstscax.dll" \
    named_guids raw_interfaces_only no_namespace

namespace {

constexpr wchar_t kContainerClass[] = L"MasterTermRdpContainer";
constexpr wchar_t kDimOverlayClass[] = L"MasterTermRdpDimOverlay";
constexpr wchar_t kShadowOverlayClass[] = L"MasterTermRdpShadowOverlay";
constexpr wchar_t kControlSessionProperty[] = L"MasterTerm.RdpSession";
constexpr UINT_PTR RdpConnectTimeoutTimer = 1;
constexpr UINT_PTR RdpInitialSurfaceRepaintTimer = 2;
constexpr UINT_PTR RdpNativeFullscreenTimer = 3;

struct MonitorSnapshot {
    HMONITOR handle = nullptr;
    RECT bounds{};
    std::wstring device;
    bool primary = false;
};

struct WindowSnapshot {
    HWND handle = nullptr;
    RECT bounds{};
    std::wstring className;
    std::wstring title;
    LONG_PTR style = 0;
    LONG_PTR exStyle = 0;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
};

BOOL CALLBACK collectMonitorSnapshot(
    HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    auto *snapshots = reinterpret_cast<std::vector<MonitorSnapshot> *>(
        parameter);
    if (!snapshots)
        return FALSE;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    MonitorSnapshot snapshot;
    snapshot.handle = monitor;
    snapshot.bounds = info.rcMonitor;
    snapshot.device = info.szDevice;
    snapshot.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    snapshots->push_back(std::move(snapshot));
    return TRUE;
}

BOOL CALLBACK collectProcessWindowSnapshot(HWND window, LPARAM parameter)
{
    auto *snapshots = reinterpret_cast<std::vector<WindowSnapshot> *>(
        parameter);
    if (!snapshots || !IsWindowVisible(window))
        return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId())
        return TRUE;

    WindowSnapshot snapshot;
    snapshot.handle = window;
    if (!GetWindowRect(window, &snapshot.bounds))
        return TRUE;
    wchar_t className[256]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    snapshot.className = className;
    const int titleLength = GetWindowTextLengthW(window);
    if (titleLength > 0) {
        std::vector<wchar_t> title(static_cast<std::size_t>(titleLength) + 1);
        GetWindowTextW(window, title.data(), titleLength + 1);
        snapshot.title.assign(title.data());
    }
    snapshot.style = GetWindowLongPtrW(window, GWL_STYLE);
    snapshot.exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    snapshot.dpi = GetDpiForWindow(window);
    snapshots->push_back(std::move(snapshot));
    return TRUE;
}

long long rectangleArea(const RECT &rectangle)
{
    return static_cast<long long>(
        std::max<LONG>(0, rectangle.right - rectangle.left))
        * std::max<LONG>(0, rectangle.bottom - rectangle.top);
}

long long intersectionArea(const RECT &first, const RECT &second)
{
    RECT intersection{};
    return IntersectRect(&intersection, &first, &second)
        ? rectangleArea(intersection) : 0;
}

std::wstring hexadecimalValue(unsigned long long value)
{
    std::wostringstream stream;
    stream << L"0x" << std::hex << value;
    return stream.str();
}

bool isLoopbackHost(const std::wstring &host)
{
    std::wstring normalized = host;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    if (normalized.size() >= 2 && normalized.front() == L'['
        && normalized.back() == L']') {
        normalized = normalized.substr(1, normalized.size() - 2);
    }
    return normalized == L"localhost" || normalized == L"::1"
        || normalized.rfind(L"127.", 0) == 0;
}

bool parseIpv4Address(
    const std::wstring &host, std::array<unsigned int, 4> &octets)
{
    std::size_t start = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const std::size_t end = host.find(L'.', start);
        if ((end == std::wstring::npos) != (index == octets.size() - 1))
            return false;
        const std::size_t length = (end == std::wstring::npos
            ? host.size() : end) - start;
        if (length == 0 || length > 3)
            return false;
        unsigned int value = 0;
        for (std::size_t offset = 0; offset < length; ++offset) {
            const wchar_t character = host[start + offset];
            if (character < L'0' || character > L'9')
                return false;
            value = value * 10 + static_cast<unsigned int>(character - L'0');
        }
        if (value > 255)
            return false;
        octets[index] = value;
        start = end == std::wstring::npos ? host.size() : end + 1;
    }
    return start == host.size();
}

bool isLanHost(const std::wstring &host)
{
    if (isLoopbackHost(host))
        return true;
    std::wstring normalized = host;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    if (normalized.size() >= 2 && normalized.front() == L'['
        && normalized.back() == L']') {
        normalized = normalized.substr(1, normalized.size() - 2);
    }
    // IPv6 unique-local and link-local addresses are LAN peers too.
    if (normalized.rfind(L"fc", 0) == 0
        || normalized.rfind(L"fd", 0) == 0
        || normalized.rfind(L"fe80:", 0) == 0)
        return true;
    std::array<unsigned int, 4> octets{};
    if (!parseIpv4Address(normalized, octets))
        return false;
    return octets[0] == 10
        || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)
        || (octets[0] == 192 && octets[1] == 168)
        || (octets[0] == 169 && octets[1] == 254);
}

bool sameOcclusionRegions(
    const std::vector<RdpOcclusionRegion> &first,
    const std::vector<RdpOcclusionRegion> &second)
{
    if (first.size() != second.size())
        return false;
    return std::equal(
        first.begin(), first.end(), second.begin(),
        [](const RdpOcclusionRegion &left,
           const RdpOcclusionRegion &right) {
            return EqualRect(&left.bounds, &right.bounds)
                && left.cornerRadius == right.cornerRadius
                && left.topSquareBottomRounded
                    == right.topSquareBottomRounded
                && left.nativeGradientShadow
                    == right.nativeGradientShadow;
        });
}

HRGN createVisibleRegion(
    int x, int y, int width, int height,
    const std::vector<RdpOcclusionRegion> &occlusionRects)
{
    HRGN visibleRegion = CreateRectRgn(0, 0, width, height);
    if (!visibleRegion)
        return nullptr;
    for (const RdpOcclusionRegion &occlusionRegion : occlusionRects) {
        // The sidebar shadow is rendered by a native layered window.  It is
        // intentionally not a hole in the RDP surface; subtracting it here
        // would expose the opaque WebView background and turn the gradient
        // into a solid-looking strip.
        if (occlusionRegion.nativeGradientShadow)
            continue;
        const RECT &occlusion = occlusionRegion.bounds;
        RECT local{
            std::max<LONG>(0, occlusion.left - x),
            std::max<LONG>(0, occlusion.top - y),
            std::min<LONG>(width, occlusion.right - x),
            std::min<LONG>(height, occlusion.bottom - y)};
        if (local.left >= local.right || local.top >= local.bottom)
            continue;
        const LONG maximumRadius = std::max<LONG>(0, std::min(
            (local.right - local.left) / 2,
            (local.bottom - local.top) / 2));
        const LONG radius = std::clamp(
            occlusionRegion.cornerRadius, 0L, maximumRadius);
        HRGN hole = nullptr;
        if (radius > 0) {
            if (occlusionRegion.topSquareBottomRounded) {
                HRGN rounded = CreateRoundRectRgn(
                    local.left, local.top, local.right, local.bottom,
                    radius * 2, radius * 2);
                HRGN topLeft = CreateRectRgn(
                    local.left, local.top, local.left + radius, local.top + radius);
                HRGN topRight = CreateRectRgn(
                    local.right - radius, local.top, local.right, local.top + radius);
                hole = CreateRectRgn(0, 0, 0, 0);
                if (rounded && topLeft && topRight && hole) {
                    CombineRgn(hole, rounded, topLeft, RGN_OR);
                    CombineRgn(hole, hole, topRight, RGN_OR);
                } else {
                    if (hole) DeleteObject(hole);
                    hole = rounded;
                    if (topLeft) DeleteObject(topLeft);
                    if (topRight) DeleteObject(topRight);
                }
                if (rounded) DeleteObject(rounded);
                if (topLeft) DeleteObject(topLeft);
                if (topRight) DeleteObject(topRight);
            } else {
                hole = CreateRoundRectRgn(
                    local.left, local.top, local.right, local.bottom,
                    radius * 2, radius * 2);
            }
        } else {
            hole = CreateRectRgn(
                local.left, local.top, local.right, local.bottom);
        }
        if (hole) {
            CombineRgn(visibleRegion, visibleRegion, hole, RGN_DIFF);
            DeleteObject(hole);
        }
    }
    return visibleRegion;
}

// Minimal IOleClientSite / IOleInPlaceSite implementation so the RDP
// ActiveX control can be embedded without ATL/MFC.
class ActiveXSite final : public IOleClientSite, public IOleInPlaceSite {
public:
    explicit ActiveXSite(HWND hostWindow) : m_host(hostWindow) {}
    ~ActiveXSite()
    {
        if (m_object) {
            m_object->Release();
            m_object = nullptr;
        }
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override
    {
        if (iid == IID_IUnknown || iid == IID_IOleClientSite) {
            *object = static_cast<IOleClientSite *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IOleInPlaceSite || iid == IID_IOleWindow) {
            *object = static_cast<IOleInPlaceSite *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --m_refs;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    // IOleClientSite
    STDMETHODIMP SaveObject() override { return S_OK; }
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker **moniker) override
    {
        if (moniker)
            *moniker = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetContainer(IOleContainer **container) override
    {
        if (container)
            *container = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP ShowObject() override { return S_OK; }
    STDMETHODIMP OnShowWindow(BOOL) override { return S_OK; }
    STDMETHODIMP RequestNewObjectLayout() override { return S_OK; }

    // IOleWindow
    STDMETHODIMP GetWindow(HWND *window) override
    {
        if (!window)
            return E_POINTER;
        *window = m_host;
        return S_OK;
    }
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }

    // IOleInPlaceSite
    STDMETHODIMP CanInPlaceActivate() override { return S_OK; }
    STDMETHODIMP OnInPlaceActivate() override { return S_OK; }
    STDMETHODIMP OnUIActivate() override { return S_OK; }
    STDMETHODIMP GetWindowContext(
        IOleInPlaceFrame **frame, IOleInPlaceUIWindow **doc,
        LPRECT position, LPRECT clip,
        LPOLEINPLACEFRAMEINFO frameInfo) override
    {
        if (!frame || !doc || !position || !clip || !frameInfo)
            return E_POINTER;
        *frame = nullptr;
        *doc = nullptr;
        GetClientRect(m_host, position);
        GetClientRect(m_host, clip);
        frameInfo->cb = sizeof(OLEINPLACEFRAMEINFO);
        frameInfo->fMDIApp = FALSE;
        frameInfo->hwndFrame = m_host;
        frameInfo->haccel = nullptr;
        frameInfo->cAccelEntries = 0;
        return S_OK;
    }
    STDMETHODIMP Scroll(SIZE) override { return S_OK; }
    STDMETHODIMP OnUIDeactivate(BOOL) override { return S_OK; }
    STDMETHODIMP OnInPlaceDeactivate() override { return S_OK; }
    STDMETHODIMP DiscardUndoState() override { return S_OK; }
    STDMETHODIMP DeactivateAndUndo() override { return S_OK; }
    STDMETHODIMP OnPosRectChange(LPCRECT position) override
    {
        IOleInPlaceObject *object = m_object;
        if (object && position)
            object->SetObjectRects(position, position);
        return S_OK;
    }

    void attachObject(IOleInPlaceObject *object)
    {
        if (m_object)
            m_object->Release();
        m_object = object;
        if (m_object)
            m_object->AddRef();
    }

private:
    HWND m_host = nullptr;
    IOleInPlaceObject *m_object = nullptr;
    ULONG m_refs = 1;
};

// Receives RDP control connection events (IMsTscAxEvents) so the app can
// reflect "connecting / connected / disconnected / error" to the frontend.
class RdpEventSink final : public IDispatch {
public:
    using Handler = std::function<void(const std::wstring &)>;
    using HandlerPtr = std::shared_ptr<Handler>;
    using FullscreenHandler = std::function<void(bool)>;
    using FullscreenHandlerPtr = std::shared_ptr<FullscreenHandler>;

    RdpEventSink(
        std::function<void(const std::wstring &)> handler,
        std::function<void(bool)> fullscreenHandler)
        : m_handler(std::make_shared<Handler>(std::move(handler))),
          m_fullscreenHandler(std::make_shared<FullscreenHandler>(
              std::move(fullscreenHandler))) {}

    STDMETHODIMP QueryInterface(REFIID iid, void **object) override
    {
        if (iid == IID_IUnknown || iid == IID_IDispatch
            || iid == __uuidof(IMsTscAxEvents)) {
            *object = static_cast<IDispatch *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --m_refs;
        if (remaining == 0)
            delete this;
        return remaining;
    }
    STDMETHODIMP GetTypeInfoCount(UINT *) override { return S_OK; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetIDsOfNames(
        REFIID, LPOLESTR *, UINT, LCID, DISPID *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(
        DISPID member, REFIID, LCID, WORD,
        DISPPARAMS *params, VARIANT *, EXCEPINFO *, UINT *) override
    {
        // ActiveX events can be delivered while the host is unadvising or
        // releasing the control. Keep a strong snapshot for this entire
        // Invoke call so clearing the sink cannot destroy the callback while
        // another COM callback is using it.
        const HandlerPtr handler = std::atomic_load_explicit(
            &m_handler, std::memory_order_acquire);
        const FullscreenHandlerPtr fullscreenHandler =
            std::atomic_load_explicit(
                &m_fullscreenHandler, std::memory_order_acquire);
        if ((!handler || !*handler)
            && (!fullscreenHandler || !*fullscreenHandler))
            return S_OK;
        const auto emit = [&handler](const std::wstring &state) {
            if (handler && *handler)
                (*handler)(state);
        };
        const auto eventCode = [params]() -> std::wstring {
            if (!params || params->cArgs == 0)
                return {};
            const VARIANT &value = params->rgvarg[0];
            if (value.vt == VT_I4)
                return std::to_wstring(value.lVal);
            if (value.vt == VT_UI4)
                return std::to_wstring(value.ulVal);
            if (value.vt == VT_I2)
                return std::to_wstring(value.iVal);
            if (value.vt == VT_UI2)
                return std::to_wstring(value.uiVal);
            return {};
        };
        switch (member) {
        // IMsTscAxEvents uses the small DISPIDs below.  The previous
        // implementation used an old 0x100-based mapping, which silently
        // discarded every lifecycle event even though Advise() succeeded.
        case 1:  // DISPID_CONNECTING / OnConnecting
            emit(L"connecting");
            break;
        case 2:  // DISPID_CONNECTED / OnConnected
            emit(L"connected");
            break;
        case 3:  // DISPID_LOGINCOMPLETE / OnLoginComplete
            emit(L"connected");
            break;
        case 4:  // DISPID_DISCONNECTED / OnDisconnected
            if (const std::wstring code = eventCode(); !code.empty())
                emit(L"error:远程桌面已断开（错误码 " + code + L"）");
            else
                emit(L"disconnected");
            break;
        case 5:  // DISPID_ENTERFULLSCREENMODE / OnEnterFullScreenMode
            if (fullscreenHandler && *fullscreenHandler)
                (*fullscreenHandler)(true);
            break;
        case 6:  // DISPID_LEAVEFULLSCREENMODE / OnLeaveFullScreenMode
            if (fullscreenHandler && *fullscreenHandler)
                (*fullscreenHandler)(false);
            break;
        case 10:  // DISPID_FATALERROR / OnFatalError
            if (const std::wstring code = eventCode(); !code.empty())
                emit(L"error:远程桌面发生致命错误（错误码 " + code + L"）");
            else
                emit(L"error:远程桌面发生致命错误");
            break;
        case 11:  // DISPID_WARNING / OnWarning
            if (const std::wstring code = eventCode(); !code.empty())
                emit(L"warning:远程桌面控件报告警告（代码 " + code + L"）");
            else
                emit(L"warning:远程桌面控件报告警告");
            break;
        case 17:  // DISPID_AUTORECONNECTING / OnAutoReconnecting
        case 34:  // DISPID_AUTORECONNECTING2 / OnAutoReconnecting2
            emit(L"connecting");
            break;
        case 22:  // DISPID_LOGONERROR / OnLogonError (errorCode)
            if (const std::wstring code = eventCode(); !code.empty())
                emit(L"error:远程桌面登录失败（错误码 " + code + L"）");
            else
                emit(L"error:远程桌面登录失败（请检查用户名/密码）");
            break;
        case 33:  // DISPID_AUTORECONNECTED / OnAutoReconnected
            emit(L"connected");
            break;
        default:
            break;
        }
        return S_OK;
    }

    void clearHandler()
    {
        std::atomic_store_explicit(
            &m_handler, HandlerPtr{}, std::memory_order_release);
        std::atomic_store_explicit(
            &m_fullscreenHandler, FullscreenHandlerPtr{},
            std::memory_order_release);
    }

private:
    HandlerPtr m_handler;
    FullscreenHandlerPtr m_fullscreenHandler;
    ULONG m_refs = 1;
};

} // namespace

struct RdpSession::Impl {
    IMsTscAxPtr control;
    ActiveXSite *site = nullptr;
    RdpEventSink *eventSink = nullptr;
    IConnectionPoint *connectionPoint = nullptr;
    DWORD adviseCookie = 0;
};

RdpSession::RdpSession(HWND parent) : m_parent(parent)
{
    m_impl = std::make_unique<Impl>();
}

RdpSession::~RdpSession()
{
    // Do NOT call Disconnect() here: it can block synchronously while the
    // control waits on the network, which would freeze the UI thread on
    // close.  Destroying the container window first tears the control down
    // quickly, then the control is released.
    if (m_controlWindow && IsWindow(m_controlWindow)) {
        if (m_originalControlProcedure)
            SetWindowLongPtrW(
                m_controlWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(m_originalControlProcedure));
        RemovePropW(m_controlWindow, kControlSessionProperty);
    }
    m_originalControlProcedure = nullptr;
    m_controlWindow = nullptr;
    if (m_dimOverlay) {
        RemovePropW(m_dimOverlay, kControlSessionProperty);
        DestroyWindow(m_dimOverlay);
        m_dimOverlay = nullptr;
    }
    if (m_shadowOverlay) {
        DestroyWindow(m_shadowOverlay);
        m_shadowOverlay = nullptr;
    }
    if (m_impl->eventSink)
        m_impl->eventSink->clearHandler();
    if (m_impl->connectionPoint && m_impl->adviseCookie) {
        m_impl->connectionPoint->Unadvise(m_impl->adviseCookie);
        m_impl->adviseCookie = 0;
    }
    if (m_impl->connectionPoint) {
        m_impl->connectionPoint->Release();
        m_impl->connectionPoint = nullptr;
    }
    if (m_impl->eventSink) {
        m_impl->eventSink->Release();
        m_impl->eventSink = nullptr;
    }
    // Explicitly deactivate the in-place object and detach the client site
    // before destroying the HWND or releasing the control.  Otherwise the
    // RDP ActiveX can call back into a freed site during its final release,
    // which manifests as CRT heap corruption on disconnect.
    if (m_impl->control) {
        IOleInPlaceObject *inPlace = nullptr;
        if (SUCCEEDED(m_impl->control.QueryInterface(
                IID_IOleInPlaceObject,
                reinterpret_cast<void **>(&inPlace)))
            && inPlace) {
            inPlace->UIDeactivate();
            inPlace->InPlaceDeactivate();
            inPlace->Release();
        }
        IOleObject *oleObject = nullptr;
        if (SUCCEEDED(m_impl->control.QueryInterface(
                IID_IOleObject,
                reinterpret_cast<void **>(&oleObject)))
            && oleObject) {
            oleObject->SetClientSite(nullptr);
            oleObject->Close(OLECLOSE_NOSAVE);
            oleObject->Release();
        }
    }
    if (m_container) {
        KillTimer(m_container, RdpConnectTimeoutTimer);
        KillTimer(m_container, RdpInitialSurfaceRepaintTimer);
        KillTimer(m_container, RdpNativeFullscreenTimer);
        DestroyWindow(m_container);
        m_container = nullptr;
    }
    // Release the ActiveX control while the site objects are still alive:
    // the control may call back into its client site during teardown, so the
    // site must outlive the control.
    m_impl->control = nullptr;
    if (m_impl->site) {
        m_impl->site->attachObject(nullptr);
        m_impl->site->Release();
        m_impl->site = nullptr;
    }
}

void RdpSession::setStateHandler(
    std::function<void(const std::wstring &)> handler)
{
    m_stateHandler = std::move(handler);
}

void RdpSession::setHostedVisible(bool visible)
{
    const bool unchanged = m_hostedVisible == visible;
    m_hostedVisible = visible;
    if (!m_container)
        return;
    if (!m_visible) {
        if (m_dimOverlay)
            ShowWindow(m_dimOverlay, SW_HIDE);
        if (m_shadowOverlay)
            ShowWindow(m_shadowOverlay, SW_HIDE);
        m_shadowBoundsValid = false;
        // Before the first desktop frame (and after disconnect), keep the
        // empty ActiveX host out of the composition entirely.
        ShowWindow(m_container, SW_HIDE);
        if (m_controlWindow && m_controlWindow != m_container)
            ShowWindow(m_controlWindow, SW_HIDE);
        return;
    }

    // Never hide a connected mstscax window when switching tabs. Hiding and
    // showing the ActiveX control discards its presentation surface and the
    // next frame is black even though the RDP transport stayed connected.
    // A background RDP host remains visible at HWND_BOTTOM, underneath the
    // windowed WebView2 controller. The active host is raised atomically.
    if (m_controlWindow && m_controlWindow != m_container
        && !IsWindowVisible(m_controlWindow))
        ShowWindow(m_controlWindow, SW_SHOWNA);
    // Layout/overlay notifications may arrive several times in one frame.
    // Re-raising an already active HWND lets mstscax paint above WebView2
    // while a modal's replacement clipping region is still being queried.
    if (unchanged && m_hostedVisible && IsWindowVisible(m_container)) {
        updateShadowOverlay();
        return;
    }
    if (!m_hostedVisible && m_dimOverlay)
        ShowWindow(m_dimOverlay, SW_HIDE);
    if (!m_hostedVisible && m_shadowOverlay)
        ShowWindow(m_shadowOverlay, SW_HIDE);
    SetWindowPos(
        m_container, m_hostedVisible ? HWND_TOP : HWND_BOTTOM,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (m_hostedVisible)
        updateShadowOverlay();
}

void RdpSession::clearOcclusionRegion()
{
    // Top-level overlays must disappear before the RDP host is exposed again.
    // Otherwise their last layered frame can survive one DWM composition pass
    // after a dialog or flyout has already closed.
    if (m_dimOverlay)
        ShowWindow(m_dimOverlay, SW_HIDE);
    if (m_shadowOverlay)
        ShowWindow(m_shadowOverlay, SW_HIDE);
    m_shadowBoundsValid = false;

    const bool alreadyClear = m_occlusionStateValid
        && m_occlusionHostX == m_hostX
        && m_occlusionHostY == m_hostY
        && m_occlusionHostWidth == m_hostWidth
        && m_occlusionHostHeight == m_hostHeight
        && m_appliedOcclusionRects.empty();
    if (alreadyClear)
        return;

    // The ActiveX presentation HWND is nested inside a container whose paint
    // handler intentionally fills uncovered pixels black. Restore the child
    // region first and commit the outer region last, with redraw suppressed,
    // so the black container can never be exposed between the two calls.
    bool cleared = true;
    if (m_controlWindow && m_controlWindow != m_container
        && !SetWindowRgn(m_controlWindow, nullptr, FALSE))
        cleared = false;
    if (m_container && !SetWindowRgn(m_container, nullptr, FALSE))
        cleared = false;

    m_occlusionHostX = m_hostX;
    m_occlusionHostY = m_hostY;
    m_occlusionHostWidth = m_hostWidth;
    m_occlusionHostHeight = m_hostHeight;
    m_appliedOcclusionRects.clear();
    m_occlusionStateValid = cleared;
    redrawOcclusionSurface();
}

void RdpSession::setSmartSizing(bool enabled)
{
    if (!m_impl || !m_impl->control)
        return;
    IMsRdpClientPtr client;
    if (FAILED(m_impl->control.QueryInterface(
            IID_IMsRdpClient, reinterpret_cast<void **>(&client)))
        || !client)
        return;
    IMsRdpClientAdvancedSettingsPtr baseSettings;
    IMsRdpClientAdvancedSettings8Ptr settings;
    if (FAILED(client->get_AdvancedSettings2(&baseSettings))
        || !baseSettings
        || FAILED(baseSettings.QueryInterface(
            IID_IMsRdpClientAdvancedSettings8,
            reinterpret_cast<void **>(&settings)))
        || !settings)
        return;
    const HRESULT result = settings->put_SmartSizing(
        enabled ? VARIANT_TRUE : VARIANT_FALSE);
    m_quality.smartSizing = enabled;
    m_quality.lastOperation = L"SmartSizing 设置";
    m_quality.lastHresult = static_cast<unsigned long>(result);
    m_quality.lastHresultSucceeded = SUCCEEDED(result);
    emitQuality();
    writeLog(
        std::wstring(L"  SmartSizing=") + (enabled ? L"TRUE" : L"FALSE")
        + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(result)));
}

bool RdpSession::setUseMultimon(bool enabled)
{
    if (!m_impl || !m_impl->control)
        return false;
    IMsRdpClientNonScriptable5Ptr nonScriptable5;
    if (FAILED(m_impl->control.QueryInterface(
        IID_IMsRdpClientNonScriptable5,
            reinterpret_cast<void **>(&nonScriptable5)))
        || !nonScriptable5) {
        writeLog(L"  IMsRdpClientNonScriptable5: QI FAILED");
        return false;
    }
    const HRESULT result = nonScriptable5->put_UseMultimon(
        enabled ? VARIANT_TRUE : VARIANT_FALSE);
    if (SUCCEEDED(result)) {
        m_useMultimon = enabled;
        m_displaySettingsSynchronized = false;
    }
    writeLog(
        std::wstring(L"  UseMultimon=") + (enabled ? L"TRUE" : L"FALSE")
        + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(result)));
    VARIANT_BOOL actual = VARIANT_FALSE;
    const HRESULT readResult = nonScriptable5->get_UseMultimon(&actual);
    writeLog(L"  UseMultimon readback="
        + std::wstring(actual == VARIANT_TRUE ? L"TRUE" : L"FALSE")
        + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(readResult)));
    return SUCCEEDED(result) && SUCCEEDED(readResult)
        && (actual == VARIANT_TRUE) == enabled;
}

bool RdpSession::setNativeFullscreen(bool enabled)
{
    if (!m_impl || !m_impl->control || (enabled && !m_connected))
        return false;
    IMsRdpClientPtr client;
    if (FAILED(m_impl->control.QueryInterface(
            IID_IMsRdpClient, reinterpret_cast<void **>(&client)))
        || !client) {
        writeLog(L"  native fullscreen: IMsRdpClient QI FAILED");
        return false;
    }
    if (enabled) {
        IMsRdpClientNonScriptable3Ptr nonScriptable;
        if (SUCCEEDED(m_impl->control.QueryInterface(
                IID_IMsRdpClientNonScriptable3,
                reinterpret_cast<void **>(&nonScriptable)))
            && nonScriptable) {
            const HRESULT titleResult =
                nonScriptable->put_ConnectionBarText(
                    _bstr_t(L"MasterTerm RDP"));
            writeLog(L"  native fullscreen: ConnectionBarText -> 0x"
                + std::to_wstring(static_cast<unsigned long>(titleResult)));
        }
    }
    const HRESULT result = client->put_FullScreen(
        enabled ? VARIANT_TRUE : VARIANT_FALSE);
    VARIANT_BOOL actual = VARIANT_FALSE;
    const HRESULT readResult = client->get_FullScreen(&actual);
    writeLog(std::wstring(L"  native fullscreen=")
        + (enabled ? L"TRUE" : L"FALSE") + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(result))
        + L" readback="
        + (actual == VARIANT_TRUE ? L"TRUE" : L"FALSE") + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(readResult)));
    return SUCCEEDED(result) && SUCCEEDED(readResult)
        && (actual == VARIANT_TRUE) == enabled;
}

bool RdpSession::validateNativeFullscreenPresentation()
{
    if (!m_impl || !m_impl->control || !m_useMultimon)
        return false;

    long horizontalScroll = 0;
    long verticalScroll = 0;
    const HRESULT horizontalResult =
        m_impl->control->get_HorizontalScrollBarVisible(&horizontalScroll);
    const HRESULT verticalResult =
        m_impl->control->get_VerticalScrollBarVisible(&verticalScroll);

    VARIANT_BOOL fullScreen = VARIANT_FALSE;
    HRESULT fullScreenResult = E_NOINTERFACE;
    IMsRdpClientPtr client;
    if (SUCCEEDED(m_impl->control.QueryInterface(
            IID_IMsRdpClient, reinterpret_cast<void **>(&client)))
        && client) {
        fullScreenResult = client->get_FullScreen(&fullScreen);
    }

    unsigned long remoteMonitorCount = 0;
    VARIANT_BOOL layoutMatches = VARIANT_FALSE;
    HRESULT monitorCountResult = E_NOINTERFACE;
    HRESULT layoutResult = E_NOINTERFACE;
    IMsRdpClientNonScriptable5Ptr nonScriptable5;
    if (SUCCEEDED(m_impl->control.QueryInterface(
            IID_IMsRdpClientNonScriptable5,
            reinterpret_cast<void **>(&nonScriptable5)))
        && nonScriptable5) {
        monitorCountResult = nonScriptable5->get_RemoteMonitorCount(
            &remoteMonitorCount);
        layoutResult = nonScriptable5->get_RemoteMonitorLayoutMatchesLocal(
            &layoutMatches);
    }

    std::vector<MonitorSnapshot> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr, collectMonitorSnapshot,
        reinterpret_cast<LPARAM>(&monitors));
    RECT virtualBounds{};
    bool haveVirtualBounds = false;
    long long smallestMonitorArea = 0;
    for (const MonitorSnapshot &monitor : monitors) {
        if (!haveVirtualBounds) {
            virtualBounds = monitor.bounds;
            haveVirtualBounds = true;
        } else {
            UnionRect(&virtualBounds, &virtualBounds, &monitor.bounds);
        }
        const long long area = rectangleArea(monitor.bounds);
        if (area > 0 && (smallestMonitorArea == 0
                        || area < smallestMonitorArea))
            smallestMonitorArea = area;
        writeLog(L"  local monitor: device=" + monitor.device
            + L" rect=" + std::to_wstring(monitor.bounds.left) + L","
            + std::to_wstring(monitor.bounds.top) + L"-"
            + std::to_wstring(monitor.bounds.right) + L","
            + std::to_wstring(monitor.bounds.bottom)
            + (monitor.primary ? L" primary=TRUE" : L" primary=FALSE"));
    }

    if (m_containerHandledFullscreen) {
        const HWND fullscreenHost = m_container
            ? GetAncestor(m_container, GA_ROOT) : nullptr;
        RECT hostBounds{};
        RECT containerBounds{};
        const bool haveHostBounds = fullscreenHost
            && GetWindowRect(fullscreenHost, &hostBounds);
        const bool haveContainerBounds = m_container
            && GetWindowRect(m_container, &containerBounds);
        bool hostCoverageValid = haveVirtualBounds && haveHostBounds
            && haveContainerBounds;
        for (const MonitorSnapshot &monitor : monitors) {
            const long long monitorArea = rectangleArea(monitor.bounds);
            if (monitorArea <= 0
                || intersectionArea(monitor.bounds, hostBounds) * 100
                    < monitorArea * 90
                || intersectionArea(monitor.bounds, containerBounds) * 100
                    < monitorArea * 90) {
                hostCoverageValid = false;
                break;
            }
        }
        const bool scrollBarsHidden = SUCCEEDED(horizontalResult)
            && SUCCEEDED(verticalResult)
            && horizontalScroll == 0 && verticalScroll == 0;
        const bool topologyValid = monitors.size() < 2
            || (SUCCEEDED(monitorCountResult)
                && remoteMonitorCount >= 2
                && SUCCEEDED(layoutResult)
                && layoutMatches == VARIANT_TRUE);
        const bool valid = topologyValid && scrollBarsHidden
            && hostCoverageValid;
        writeLog(L"  container fullscreen validation: control="
            + m_controlProgId + L" version="
            + (m_controlVersion.empty() ? L"unknown" : m_controlVersion)
            + L" fullscreenProperty="
            + (fullScreen == VARIANT_TRUE ? L"TRUE" : L"FALSE")
            + L" hscroll=" + std::to_wstring(horizontalScroll)
            + L" vscroll=" + std::to_wstring(verticalScroll)
            + L" localMonitors=" + std::to_wstring(monitors.size())
            + L" remoteMonitors=" + std::to_wstring(remoteMonitorCount)
            + L" layoutMatches="
            + (layoutMatches == VARIANT_TRUE ? L"TRUE" : L"FALSE")
            + L" host=" + std::to_wstring(hostBounds.left) + L","
            + std::to_wstring(hostBounds.top) + L"-"
            + std::to_wstring(hostBounds.right) + L","
            + std::to_wstring(hostBounds.bottom)
            + L" container=" + std::to_wstring(containerBounds.left)
            + L"," + std::to_wstring(containerBounds.top) + L"-"
            + std::to_wstring(containerBounds.right) + L","
            + std::to_wstring(containerBounds.bottom)
            + L" coverage="
            + (hostCoverageValid ? L"TRUE" : L"FALSE")
            + L" result=" + (valid ? L"PASS" : L"FAIL"));
        return valid;
    }

    std::vector<WindowSnapshot> windows;
    EnumWindows(
        collectProcessWindowSnapshot,
        reinterpret_cast<LPARAM>(&windows));
    const HWND applicationWindow = m_parent
        ? GetAncestor(m_parent, GA_ROOT) : nullptr;
    std::vector<WindowSnapshot *> presentationWindows;
    for (WindowSnapshot &window : windows) {
        writeLog(L"  process window: hwnd="
            + hexadecimalValue(reinterpret_cast<ULONG_PTR>(window.handle))
            + L" class=" + window.className
            + L" title=" + window.title
            + L" rect=" + std::to_wstring(window.bounds.left) + L","
            + std::to_wstring(window.bounds.top) + L"-"
            + std::to_wstring(window.bounds.right) + L","
            + std::to_wstring(window.bounds.bottom)
            + L" dpi=" + std::to_wstring(window.dpi)
            + L" style="
            + hexadecimalValue(static_cast<ULONG_PTR>(window.style))
            + L" exStyle="
            + hexadecimalValue(static_cast<ULONG_PTR>(window.exStyle)));
        if (window.handle == applicationWindow || !haveVirtualBounds)
            continue;
        if (intersectionArea(window.bounds, virtualBounds) == 0)
            continue;
        // Ignore the connection bar and MasterTerm's compact toolbar. The
        // actual RDP presentation window is at least a substantial fraction
        // of one local monitor, regardless of whether mstscax uses one
        // spanning HWND or one HWND per monitor.
        if (smallestMonitorArea > 0
            && rectangleArea(window.bounds) >= smallestMonitorArea / 3)
            presentationWindows.push_back(&window);
    }

    // Some mstscax builds correctly negotiate two remote monitors but still
    // size their control-owned UIMainClass fullscreen host to the primary
    // monitor. The remote 4480x1440 desktop then sits behind a 2560x1440
    // viewport. Native mstsc sizes this popup to the virtual monitor bounds;
    // do the same for the embedded control while preserving the negotiated
    // per-monitor topology.
    if (monitors.size() >= 2 && presentationWindows.size() == 1
        && presentationWindows.front()->className == L"UIMainClass") {
        WindowSnapshot *presentation = presentationWindows.front();
        bool coversAllMonitors = true;
        for (const MonitorSnapshot &monitor : monitors) {
            const long long monitorArea = rectangleArea(monitor.bounds);
            if (monitorArea <= 0
                || intersectionArea(monitor.bounds, presentation->bounds)
                    * 100 < monitorArea * 90) {
                coversAllMonitors = false;
                break;
            }
        }
        if (!coversAllMonitors && haveVirtualBounds) {
            const BOOL positioned = SetWindowPos(
                presentation->handle, HWND_TOP,
                virtualBounds.left, virtualBounds.top,
                virtualBounds.right - virtualBounds.left,
                virtualBounds.bottom - virtualBounds.top,
                SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            RECT adjustedBounds{};
            const BOOL readBack = GetWindowRect(
                presentation->handle, &adjustedBounds);
            if (readBack)
                presentation->bounds = adjustedBounds;
            writeLog(L"  native fullscreen host repair: hwnd="
                + hexadecimalValue(reinterpret_cast<ULONG_PTR>(
                    presentation->handle))
                + L" requested=" + std::to_wstring(virtualBounds.left)
                + L"," + std::to_wstring(virtualBounds.top) + L"-"
                + std::to_wstring(virtualBounds.right) + L","
                + std::to_wstring(virtualBounds.bottom)
                + L" positioned=" + (positioned ? L"TRUE" : L"FALSE")
                + L" actual=" + std::to_wstring(presentation->bounds.left)
                + L"," + std::to_wstring(presentation->bounds.top) + L"-"
                + std::to_wstring(presentation->bounds.right) + L","
                + std::to_wstring(presentation->bounds.bottom));
        }
    }

    bool monitorCoverageValid = !monitors.empty()
        && !presentationWindows.empty();
    for (const MonitorSnapshot &monitor : monitors) {
        long long coveredArea = 0;
        for (const WindowSnapshot *window : presentationWindows)
            coveredArea += intersectionArea(monitor.bounds, window->bounds);
        const long long monitorArea = rectangleArea(monitor.bounds);
        if (monitorArea <= 0 || coveredArea * 100 < monitorArea * 90) {
            monitorCoverageValid = false;
            break;
        }
    }

    const bool scrollBarsHidden = SUCCEEDED(horizontalResult)
        && SUCCEEDED(verticalResult)
        && horizontalScroll == 0 && verticalScroll == 0;
    const bool topologyValid = monitors.size() < 2
        || (SUCCEEDED(monitorCountResult)
            && remoteMonitorCount >= 2
            && SUCCEEDED(layoutResult)
            && layoutMatches == VARIANT_TRUE);
    const bool fullScreenValid = SUCCEEDED(fullScreenResult)
        && fullScreen == VARIANT_TRUE;
    const bool valid = fullScreenValid && topologyValid
        && scrollBarsHidden && monitorCoverageValid;
    writeLog(L"  native fullscreen validation: control="
        + m_controlProgId + L" version="
        + (m_controlVersion.empty() ? L"unknown" : m_controlVersion)
        + L" fullscreen="
        + (fullScreen == VARIANT_TRUE ? L"TRUE" : L"FALSE")
        + L" hscroll=" + std::to_wstring(horizontalScroll)
        + L" vscroll=" + std::to_wstring(verticalScroll)
        + L" localMonitors=" + std::to_wstring(monitors.size())
        + L" remoteMonitors=" + std::to_wstring(remoteMonitorCount)
        + L" layoutMatches="
        + (layoutMatches == VARIANT_TRUE ? L"TRUE" : L"FALSE")
        + L" presentationWindows="
        + std::to_wstring(presentationWindows.size())
        + L" coverage="
        + (monitorCoverageValid ? L"TRUE" : L"FALSE")
        + L" result=" + (valid ? L"PASS" : L"FAIL"));
    return valid;
}

void RdpSession::focusControlForKeyboard()
{
    HWND focusTarget = m_container ? m_container : m_controlWindow;
    if (!focusTarget || !IsWindow(focusTarget))
        return;
    HWND root = GetAncestor(focusTarget, GA_ROOT);
    if (root && GetForegroundWindow() != root)
        SetForegroundWindow(root);
    SetFocus(focusTarget);
    if (m_controlWindow && IsWindow(m_controlWindow))
        SetFocus(m_controlWindow);
    writeLog(L"  KeyboardFocus="
        + std::to_wstring(reinterpret_cast<ULONG_PTR>(GetFocus())));
}

UINT RdpSession::sendInputSequence(
    const WORD *virtualKeys, const DWORD *flags, UINT count)
{
    if (!virtualKeys || !flags || count == 0 || count > 256)
        return 0;
    focusControlForKeyboard();
    std::vector<INPUT> inputs(count);
    for (UINT index = 0; index < count; ++index) {
        INPUT &input = inputs[index];
        input.type = INPUT_KEYBOARD;
        const UINT scanCode = MapVirtualKeyW(
            virtualKeys[index], MAPVK_VK_TO_VSC);
        if (scanCode != 0) {
            input.ki.wScan = static_cast<WORD>(scanCode & 0xff);
            input.ki.dwFlags = KEYEVENTF_SCANCODE | flags[index];
        } else {
            input.ki.wVk = virtualKeys[index];
            input.ki.dwFlags = flags[index];
        }
    }
    // Let focus activation settle, then inject each transition separately.
    // RDP's keyboard hook can collapse a complete modifier sequence delivered
    // in one SendInput call, especially inside a virtualized desktop.
    Sleep(20);
    UINT sent = 0;
    DWORD error = ERROR_SUCCESS;
    for (UINT index = 0; index < count; ++index) {
        SetLastError(ERROR_SUCCESS);
        const UINT current = SendInput(
            1, &inputs[index], static_cast<int>(sizeof(INPUT)));
        sent += current;
        if (current != 1 && error == ERROR_SUCCESS)
            error = GetLastError();
        if (index + 1 < count)
            Sleep(15);
    }
    writeLog(L"  SendInput(staged)=" + std::to_wstring(sent) + L"/"
        + std::to_wstring(count) + L" error="
        + std::to_wstring(error));
    return sent;
}

void RdpSession::launchRemoteUtility(const std::wstring &command)
{
    if (!m_connected)
        return;

    // Use the remote Run dialog instead of Start search.  Start search can
    // treat commands such as "control.exe" or "ms-settings:" as search text,
    // while Win+R invokes the command directly inside the remote session.
    const WORD runKeys[] = {VK_LWIN, L'R', L'R', VK_LWIN};
    const DWORD runFlags[] = {
        KEYEVENTF_EXTENDEDKEY,
        0,
        KEYEVENTF_KEYUP,
        KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP};
    if (sendInputSequence(runKeys, runFlags, ARRAYSIZE(runKeys))
        != ARRAYSIZE(runKeys)) {
        writeLog(L"  LaunchRemoteUtility failed to open Run: " + command);
        return;
    }
    if (command.empty())
        return;
    Sleep(300);

    std::vector<WORD> keys;
    std::vector<DWORD> flags;
    keys.reserve(command.size() * 4);
    flags.reserve(command.size() * 4);
    const auto appendKey = [&keys, &flags](WORD key, DWORD keyFlags = 0) {
        keys.push_back(key);
        flags.push_back(keyFlags);
    };
    const auto appendPress = [&appendKey](WORD key, DWORD keyFlags = 0) {
        appendKey(key, keyFlags);
        appendKey(key, keyFlags | KEYEVENTF_KEYUP);
    };
    const HKL keyboardLayout = GetKeyboardLayout(0);
    for (const wchar_t character : command) {
        const SHORT mapped = VkKeyScanExW(character, keyboardLayout);
        if (mapped == -1) {
            writeLog(L"  LaunchRemoteUtility cannot map command: " + command);
            return;
        }
        const BYTE modifiers = HIBYTE(mapped);
        const WORD key = LOBYTE(mapped);
        if (modifiers & 1)
            appendKey(VK_LSHIFT);
        if (modifiers & 2)
            appendKey(VK_LCONTROL);
        if (modifiers & 4)
            appendKey(VK_LMENU);
        appendPress(key);
        if (modifiers & 4)
            appendKey(VK_LMENU, KEYEVENTF_KEYUP);
        if (modifiers & 2)
            appendKey(VK_LCONTROL, KEYEVENTF_KEYUP);
        if (modifiers & 1)
            appendKey(VK_LSHIFT, KEYEVENTF_KEYUP);
    }
    if (sendInputSequence(
            keys.data(), flags.data(), static_cast<UINT>(keys.size()))
        != keys.size()) {
        writeLog(L"  LaunchRemoteUtility failed to type command: " + command);
        return;
    }
    Sleep(150);

    const WORD enterKeys[] = {VK_RETURN, VK_RETURN};
    const DWORD enterFlags[] = {0, KEYEVENTF_KEYUP};
    const UINT sent = sendInputSequence(
        enterKeys, enterFlags, ARRAYSIZE(enterKeys));
    writeLog(L"  LaunchRemoteUtility=" + command + L" -> "
        + std::to_wstring(sent));
}

void RdpSession::sendCtrlAltDelete()
{
    const WORD keys[] = {
        VK_LCONTROL, VK_LMENU, VK_END,
        VK_END, VK_LMENU, VK_LCONTROL};
    const DWORD flags[] = {
        0, 0, KEYEVENTF_EXTENDEDKEY,
        KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP,
        KEYEVENTF_KEYUP, KEYEVENTF_KEYUP};
    const UINT sent = sendInputSequence(keys, flags, ARRAYSIZE(keys));
    writeLog(L"  Inject(Ctrl+Alt+Del as Ctrl+Alt+End) -> "
        + std::to_wstring(sent));
}

void RdpSession::sendCtrlAltEnd()
{
    const WORD keys[] = {
        VK_LCONTROL, VK_LMENU, VK_END,
        VK_END, VK_LMENU, VK_LCONTROL};
    const DWORD flags[] = {
        0, 0, KEYEVENTF_EXTENDEDKEY,
        KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP,
        KEYEVENTF_KEYUP, KEYEVENTF_KEYUP};
    const UINT sent = sendInputSequence(keys, flags, ARRAYSIZE(keys));
    writeLog(L"  Inject(Ctrl+Alt+End) -> " + std::to_wstring(sent));
}

void RdpSession::sendWindowsKey()
{
    const WORD keys[] = {VK_LWIN, VK_LWIN};
    const DWORD flags[] = {
        KEYEVENTF_EXTENDEDKEY,
        KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP};
    const UINT sent = sendInputSequence(keys, flags, ARRAYSIZE(keys));
    writeLog(L"  Inject(Windows) -> " + std::to_wstring(sent));
}

void RdpSession::sendPrintScreen()
{
    const WORD keys[] = {VK_SNAPSHOT, VK_SNAPSHOT};
    const DWORD flags[] = {
        KEYEVENTF_EXTENDEDKEY,
        KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP};
    const UINT sent = sendInputSequence(keys, flags, ARRAYSIZE(keys));
    writeLog(L"  Inject(PrintScreen) -> " + std::to_wstring(sent));
}

LRESULT CALLBACK RdpSession::controlWindowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *session = reinterpret_cast<RdpSession *>(
        GetPropW(window, kControlSessionProperty));
    if (session && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        && (wParam == VK_F11 || wParam == VK_ESCAPE)) {
        if (wParam == VK_ESCAPE && session->m_modalDimmed) {
            if (session->m_dismissModalHandler)
                session->m_dismissModalHandler();
            return 0;
        }
        if (session->m_keyHandler) {
            session->m_keyHandler(static_cast<UINT>(wParam));
            return 0;
        }
    }
    if (session && message == WM_MOUSEMOVE && session->m_mouseHandler) {
        RECT client{};
        GetClientRect(window, &client);
        const int x = static_cast<short>(LOWORD(lParam));
        const int y = static_cast<short>(HIWORD(lParam));
        const int center = (client.right - client.left) / 2;
        session->m_mouseHandler(
            y <= 52 && std::abs(x - center) <= 210);
    }
    if (session && session->m_originalControlProcedure)
        return CallWindowProcW(
            session->m_originalControlProcedure,
            window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

void RdpSession::applyOcclusionRegion(
    int x, int y, int width, int height,
    const std::vector<RdpOcclusionRegion> &occlusionRects)
{
    if (!m_container || width <= 0 || height <= 0)
        return;
    if (occlusionRects.empty()) {
        clearOcclusionRegion();
        return;
    }

    const bool unchanged = m_occlusionStateValid
        && m_occlusionHostX == x && m_occlusionHostY == y
        && m_occlusionHostWidth == width
        && m_occlusionHostHeight == height
        && sameOcclusionRegions(m_appliedOcclusionRects, occlusionRects);
    if (unchanged) {
        // Visibility or z-order may have changed even though the geometry did
        // not. Keep the native gradient in sync without touching either HWND
        // region; redundant SetWindowRgn calls are a major source of flashes.
        updateShadowOverlay(x, y, width, height, occlusionRects);
        return;
    }

    // Prepare the nested ActiveX region first while the old outer container
    // region still clips it. The container is committed last, so its black
    // background is never exposed during a region transition.
    bool applied = true;
    if (m_controlWindow && m_controlWindow != m_container) {
        HRGN controlRegion = createVisibleRegion(
            x, y, width, height, occlusionRects);
        if (!controlRegion) {
            applied = false;
        } else if (!SetWindowRgn(m_controlWindow, controlRegion, FALSE)) {
            DeleteObject(controlRegion);
            applied = false;
        }
    }
    HRGN containerRegion = createVisibleRegion(
        x, y, width, height, occlusionRects);
    if (!containerRegion) {
        applied = false;
    } else if (!SetWindowRgn(m_container, containerRegion, FALSE)) {
        DeleteObject(containerRegion);
        applied = false;
    }

    if (applied) {
        m_occlusionHostX = x;
        m_occlusionHostY = y;
        m_occlusionHostWidth = width;
        m_occlusionHostHeight = height;
        m_appliedOcclusionRects = occlusionRects;
        m_occlusionStateValid = true;
    } else {
        // Retry the same geometry on the next layout pass if either native
        // window rejected its region update.
        m_occlusionStateValid = false;
    }
    redrawOcclusionSurface();
    updateShadowOverlay(x, y, width, height, occlusionRects);
}

void RdpSession::redrawOcclusionSurface()
{
    if (!m_visible || !m_hostedVisible)
        return;
    // Multimon mstscax owns several presentation surfaces; forcing all of
    // them through synchronous WM_PAINT can blank the native fullscreen host.
    // A normal invalidation is sufficient there. Single-monitor mode can
    // commit the already-prepared child first, then validate the container
    // without erasing its black class background over the desktop.
    if (m_useMultimon) {
        if (m_controlWindow)
            InvalidateRect(m_controlWindow, nullptr, FALSE);
        InvalidateRect(m_container, nullptr, FALSE);
        return;
    }
    if (m_controlWindow && m_controlWindow != m_container) {
        RedrawWindow(
            m_controlWindow, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
    }
    RedrawWindow(
        m_container, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN | RDW_NOERASE);
}

void RdpSession::updateShadowOverlay(
    int x, int y, int width, int height,
    const std::vector<RdpOcclusionRegion> &occlusionRects)
{
    m_shadowBoundsValid = false;
    if (width <= 0 || height <= 0)
        return;
    const RECT hostBounds{x, y, x + width, y + height};
    for (const RdpOcclusionRegion &region : occlusionRects) {
        if (!region.nativeGradientShadow)
            continue;
        RECT clipped{};
        if (IntersectRect(&clipped, &hostBounds, &region.bounds)
            && clipped.right > clipped.left
            && clipped.bottom > clipped.top) {
            m_shadowBounds = clipped;
            m_shadowBoundsValid = true;
            break;
        }
    }
    updateShadowOverlay();
}

void RdpSession::updateShadowOverlay()
{
    if (!m_shadowOverlay)
        return;
    if (!m_shadowBoundsValid || !m_visible || !m_hostedVisible || !m_parent) {
        ShowWindow(m_shadowOverlay, SW_HIDE);
        return;
    }

    const int width = m_shadowBounds.right - m_shadowBounds.left;
    const int height = m_shadowBounds.bottom - m_shadowBounds.top;
    if (width <= 0 || height <= 0) {
        ShowWindow(m_shadowOverlay, SW_HIDE);
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    if (!screenDc || !memoryDc) {
        if (memoryDc)
            DeleteDC(memoryDc);
        if (screenDc)
            ReleaseDC(nullptr, screenDc);
        ShowWindow(m_shadowOverlay, SW_HIDE);
        return;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        ShowWindow(m_shadowOverlay, SW_HIDE);
        return;
    }
    HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    struct Pixel {
        BYTE blue;
        BYTE green;
        BYTE red;
        BYTE alpha;
    };
    auto *pixels = static_cast<Pixel *>(bits);
    for (int column = 0; column < width; ++column) {
        const double position = width > 1
            ? static_cast<double>(column) / static_cast<double>(width - 1)
            : 0.0;
        // A low-opacity black overlay is intentionally drawn per pixel.  A
        // translucent layered window lets the RDP desktop remain visible;
        // painting an RGBA CSS rectangle through WebView2 cannot do that
        // because its native clipping hole exposes the opaque page canvas.
        const double fade = std::pow(std::max(0.0, 1.0 - position), 1.35);
        const BYTE alpha = static_cast<BYTE>(std::lround(46.0 * fade));
        for (int row = 0; row < height; ++row) {
            Pixel &pixel = pixels[row * width + column];
            pixel.blue = 0;
            pixel.green = 0;
            pixel.red = 0;
            pixel.alpha = alpha;
        }
    }

    POINT clientOrigin{m_shadowBounds.left, m_shadowBounds.top};
    const BOOL originValid = ClientToScreen(m_parent, &clientOrigin);
    POINT screenPoint{clientOrigin.x, clientOrigin.y};
    SIZE size{width, height};
    POINT sourcePoint{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = originValid && UpdateLayeredWindow(
        m_shadowOverlay, screenDc, &screenPoint, &size, memoryDc,
        &sourcePoint, 0, &blend, ULW_ALPHA);
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    if (!updated) {
        ShowWindow(m_shadowOverlay, SW_HIDE);
        return;
    }
    SetWindowPos(
        m_shadowOverlay, HWND_TOP, screenPoint.x, screenPoint.y,
        width, height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void RdpSession::updateModalDimOverlay(
    int x, int y, int width, int height, bool dimmed,
    const std::vector<RdpOcclusionRegion> &occlusionRects)
{
    m_modalDimmed = dimmed && m_visible && m_hostedVisible;
    if (m_container) {
        EnableWindow(m_container, m_modalDimmed ? FALSE : TRUE);
    }
    if (!m_dimOverlay)
        return;
    if (!dimmed || !m_visible || !m_hostedVisible
        || width <= 0 || height <= 0) {
        ShowWindow(m_dimOverlay, SW_HIDE);
        return;
    }
    // mstscax owns nested native windows and can raise them above every child
    // in the container.  Use a no-activate popup owned by the main frame so
    // the dimmer is above both WebView2 and the RDP child. Its window region
    // punches out the HTML dialog rectangle, leaving the dialog itself fully
    // visible and interactive.
    POINT screenOrigin{x, y};
    ClientToScreen(m_parent, &screenOrigin);
    SetWindowPos(
        m_dimOverlay, HWND_TOP, screenOrigin.x, screenOrigin.y, width, height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    HRGN region = createVisibleRegion(
        x, y, width, height, occlusionRects);
    if (region && !SetWindowRgn(m_dimOverlay, region, FALSE))
        DeleteObject(region);
    SetWindowPos(
        m_dimOverlay, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
            | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    InvalidateRect(m_dimOverlay, nullptr, TRUE);
}

std::wstring RdpSession::logPath() const
{
    const std::filesystem::path directory =
        NativeDataDir::sessionLogDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error
        ? std::wstring(L"masterterm-rdp.log")
        : (directory / L"rdp.log").wstring();
}

void RdpSession::repaintDesktopSurface()
{
    if (!m_connected)
        return;
    HRESULT refreshResult = E_NOINTERFACE;
    const bool requestDisplayUpdate = m_surfaceRefreshNeedsDisplayUpdate;
    m_surfaceRefreshNeedsDisplayUpdate = false;
    if (requestDisplayUpdate && m_impl->control
        && m_displayWidth > 0 && m_displayHeight > 0) {
        if (m_useMultimon) {
            // UseMultimon is a connection-time setting. Both
            // UpdateSessionDisplaySettings and SyncSessionDisplaySettings
            // are wrong for this embedded control after Connect(): the
            // former flattens the monitors into one desktop, while the
            // latter is rejected by some mstscax builds (E_POINTER) and can
            // leave the surface blank. Keep the topology negotiated during
            // Connect and only repaint the existing ActiveX surface below.
            refreshResult = S_OK;
            writeLog(
                std::wstring(L"  LAN: keep multimon topology negotiated at Connect() (")
                + std::to_wstring(m_displayWidth) + L"x"
                + std::to_wstring(m_displayHeight) + L")");
        } else {
            IMsRdpClient9Ptr client9;
            if (SUCCEEDED(m_impl->control.QueryInterface(
                    __uuidof(IMsRdpClient9),
                    reinterpret_cast<void **>(&client9)))
                && client9) {
                refreshResult = client9->UpdateSessionDisplaySettings(
                    static_cast<unsigned long>(m_displayWidth),
                    static_cast<unsigned long>(m_displayHeight),
                    static_cast<unsigned long>(m_displayWidth),
                    static_cast<unsigned long>(m_displayHeight),
                    0, 100, 100);
            }
        }
    }
    HWND target = m_controlWindow ? m_controlWindow : m_container;
    if (!target)
        return;
    // Single-monitor sessions benefit from an explicit repaint after a
    // resize. In multimon mode mstscax owns several internal presentation
    // surfaces and RedrawWindow(...RDW_ALLCHILDREN) can clear them after the
    // connection bar is shown, leaving a white surface. Let the control's
    // normal compositor repaint those surfaces instead.
    if (m_useMultimon) {
        InvalidateRect(target, nullptr, FALSE);
        writeLog(L"  multimon: defer surface repaint to mstscax compositor");
    } else {
        RedrawWindow(
            target, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
    }
    m_quality.displayWidth = m_displayWidth;
    m_quality.displayHeight = m_displayHeight;
    ++m_quality.fullFrameRefreshCount;
    m_quality.lastOperation = requestDisplayUpdate
        ? L"完整画面刷新" : L"远程桌面重绘";
    if (requestDisplayUpdate) {
        m_quality.lastHresult = static_cast<unsigned long>(refreshResult);
        m_quality.lastHresultSucceeded = SUCCEEDED(refreshResult);
    }
    emitQuality();
    writeLog(std::wstring(L"  LAN: full frame refresh ")
        + std::to_wstring(m_displayWidth) + L"x"
        + std::to_wstring(m_displayHeight) + L" -> 0x"
        + std::to_wstring(static_cast<unsigned long>(refreshResult)));
}

void RdpSession::scheduleSurfaceRefresh(bool requestDisplayUpdate)
{
    if (!m_container || !m_connected || !m_lanConnection || m_useMultimon)
        return;
    m_surfaceRefreshNeedsDisplayUpdate =
        m_surfaceRefreshNeedsDisplayUpdate || requestDisplayUpdate;
    m_surfaceRefreshPass = 0;
    KillTimer(m_container, RdpInitialSurfaceRepaintTimer);
    SetTimer(m_container, RdpInitialSurfaceRepaintTimer, 350, nullptr);
}

void RdpSession::writeLog(const std::wstring &line)
{
    std::ofstream output(logPath(), std::ios::binary | std::ios::app);
    if (!output)
        return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    const std::string utf8 = [](const std::wstring &value) {
        const int length = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        std::string result(length > 0 ? static_cast<std::size_t>(length) : 0, '\0');
        if (length > 0)
            WideCharToMultiByte(
                CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                result.data(), length, nullptr, nullptr);
        return result;
    }(line);
    output << stamp << "  " << utf8 << "\n";
}

void RdpSession::emitState(const std::wstring &state)
{
    if (state == m_lastState) {
        writeLog(L"state duplicate ignored: " + state);
        // mstscax reports both transport connection and login completion as
        // connected.  Refresh again after login completion because that is
        // when the first fully decoded desktop frame is normally available.
        if (state == L"connected" && m_lanConnection && m_container) {
            scheduleSurfaceRefresh();
        }
        return;
    }
    m_lastState = state;
    writeLog(L"state: " + state);
    if (state == L"connecting") {
        m_connecting = true;
    } else if (state == L"connected") {
        m_connecting = false;
        m_connected = true;
        m_visible = true;
        if (m_useMultimon && m_impl->control) {
            IMsRdpClientNonScriptable5Ptr nonScriptable5;
            if (SUCCEEDED(m_impl->control.QueryInterface(
                    IID_IMsRdpClientNonScriptable5,
                    reinterpret_cast<void **>(&nonScriptable5)))
                && nonScriptable5) {
                unsigned long monitorCount = 0;
                const HRESULT countResult =
                    nonScriptable5->get_RemoteMonitorCount(&monitorCount);
                long left = 0;
                long top = 0;
                long right = 0;
                long bottom = 0;
                const HRESULT boundsResult =
                    nonScriptable5->GetRemoteMonitorsBoundingBox(
                        &left, &top, &right, &bottom);
                VARIANT_BOOL layoutMatches = VARIANT_FALSE;
                const HRESULT layoutResult =
                    nonScriptable5->get_RemoteMonitorLayoutMatchesLocal(
                        &layoutMatches);
                writeLog(L"  multimon diagnostics: monitors="
                    + std::to_wstring(monitorCount) + L" -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        countResult))
                    + L" bbox=" + std::to_wstring(left) + L","
                    + std::to_wstring(top) + L"-"
                    + std::to_wstring(right) + L","
                    + std::to_wstring(bottom) + L" -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        boundsResult))
                    + L" layoutMatchesLocal="
                    + std::wstring(layoutMatches == VARIANT_TRUE
                        ? L"TRUE" : L"FALSE") + L" -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        layoutResult)));
            } else {
                writeLog(L"  multimon diagnostics: NonScriptable5 QI FAILED");
            }
        }
        setHostedVisible(m_hostedVisible);
        if (m_container)
            KillTimer(m_container, RdpConnectTimeoutTimer);
        if (m_useMultimon && !m_containerHandledFullscreen) {
            // UseMultimon is negotiated by Connect(), while native fullscreen
            // must be entered after OnConnected. The control now creates one
            // presentation surface per monitor; the embedded child remains
            // only its owner and must not be stretched to the virtual bounds.
            // Leave the COM event callback before changing FullScreen. Some
            // mstscax builds create their native monitor windows through a
            // nested message loop and fail when invoked reentrantly from
            // OnConnected.
            if (m_container)
                SetTimer(m_container, RdpNativeFullscreenTimer, 1, nullptr);
        } else {
            scheduleSurfaceRefresh();
        }
    } else if (state == L"disconnected"
               || state.rfind(L"error:", 0) == 0) {
        m_connecting = false;
        m_connected = false;
        m_visible = false;
        m_surfaceRefreshNeedsDisplayUpdate = false;
        m_displaySettingsSynchronized = false;
        setHostedVisible(false);
        if (m_container) {
            KillTimer(m_container, RdpConnectTimeoutTimer);
            KillTimer(m_container, RdpInitialSurfaceRepaintTimer);
            KillTimer(m_container, RdpNativeFullscreenTimer);
        }
    }
    if (m_stateHandler)
        m_stateHandler(state);
}

void RdpSession::emitQuality()
{
    if (m_qualityHandler)
        m_qualityHandler(m_quality);
}

bool RdpSession::create()
{
    if (m_created)
        return m_container != nullptr;
    m_created = true;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam,
                        LPARAM lParam) -> LRESULT {
        RdpSession *session = reinterpret_cast<RdpSession *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!session)
            return DefWindowProcW(window, message, wParam, lParam);
        if (message == WM_SETFOCUS) {
            if (session->window())
                SetFocus(session->window());
            return 0;
        }
        if (message == WM_TIMER && wParam == RdpConnectTimeoutTimer) {
            KillTimer(window, RdpConnectTimeoutTimer);
            if (session->m_connecting) {
                session->writeLog(L"connect timeout after 30 seconds");
                session->m_connecting = false;
                session->emitState(L"error:远程桌面连接超时（30 秒）");
                if (session->m_impl->control)
                    session->m_impl->control->Disconnect();
            }
            return 0;
        }
        if (message == WM_TIMER && wParam == RdpNativeFullscreenTimer) {
            KillTimer(window, RdpNativeFullscreenTimer);
            if (session->m_connected && session->m_useMultimon
                && !session->setNativeFullscreen(true)) {
                session->writeLog(
                    L"  multimon: failed to enter native fullscreen");
                if (session->m_nativeFullscreenHandler)
                    session->m_nativeFullscreenHandler(false);
            }
            return 0;
        }
        if (message == WM_TIMER
            && wParam == RdpInitialSurfaceRepaintTimer) {
            KillTimer(window, RdpInitialSurfaceRepaintTimer);
            session->repaintDesktopSurface();
            if (session->m_lanConnection && session->m_connected
                && ++session->m_surfaceRefreshPass < 3) {
                const UINT delay = session->m_surfaceRefreshPass == 1
                    ? 450 : 750;
                SetTimer(
                    window, RdpInitialSurfaceRepaintTimer, delay, nullptr);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    };
    wc.hInstance = instance;
    wc.lpszClassName = kContainerClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOWFRAME);
    RegisterClassExW(&wc);

    WNDCLASSEXW dimClass{};
    dimClass.cbSize = sizeof(dimClass);
    dimClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam) -> LRESULT {
        if (message == WM_NCHITTEST)
            return HTCLIENT;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP
            || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
            auto *session = reinterpret_cast<RdpSession *>(
                GetPropW(window, kControlSessionProperty));
            if (session && session->m_dismissModalHandler) {
                session->m_dismissModalHandler();
            }
            return 0;
        }
        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
            return 0;
        if (message == WM_ERASEBKGND) {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client,
                     static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    };
    dimClass.hInstance = instance;
    dimClass.lpszClassName = kDimOverlayClass;
    dimClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dimClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&dimClass);

    WNDCLASSEXW shadowClass{};
    shadowClass.cbSize = sizeof(shadowClass);
    shadowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) -> LRESULT {
        if (message == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND)
            return 1;
        return DefWindowProcW(window, message, wParam, lParam);
    };
    shadowClass.hInstance = instance;
    shadowClass.lpszClassName = kShadowOverlayClass;
    shadowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&shadowClass);

    m_container = CreateWindowExW(
        0, kContainerClass, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 320, 240, m_parent, nullptr, instance, nullptr);
    if (!m_container)
        return false;
    SetWindowLongPtrW(m_container, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    m_dimOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kDimOverlayClass, L"", WS_POPUP,
        0, 0, 320, 240, m_parent, nullptr, instance, nullptr);
    if (m_dimOverlay) {
        SetPropW(m_dimOverlay, kControlSessionProperty, this);
        SetLayeredWindowAttributes(m_dimOverlay, 0, 170, LWA_ALPHA);
    }
    m_shadowOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
            | WS_EX_TRANSPARENT,
        kShadowOverlayClass, L"", WS_POPUP,
        0, 0, 1, 1, m_parent, nullptr, instance, nullptr);

    RECT client{};
    GetClientRect(m_container, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    // Prefer the newest registered control. The old order selected version 9
    // on every modern Windows installation because it also remains
    // registered there. Version 9 can negotiate a correct multi-monitor
    // topology yet place the combined desktop in a one-monitor fullscreen
    // viewport when resolutions or DPI values differ.
    HRESULT result = REGDB_E_CLASSNOTREG;
    constexpr std::array<const wchar_t *, 6> controlProgIds{
        L"MsTscAx.MsTscAx.13",
        L"MsTscAx.MsTscAx.12",
        L"MsTscAx.MsTscAx.11",
        L"MsTscAx.MsTscAx.10",
        L"MsTscAx.MsTscAx.9",
        L"MsTscAx.MsTscAx.3"};
    for (const wchar_t *progId : controlProgIds) {
        result = m_impl->control.CreateInstance(progId);
        if (SUCCEEDED(result)) {
            m_controlProgId = progId;
            break;
        }
    }
    if (FAILED(result))
        return false;
    BSTR version = nullptr;
    const HRESULT versionResult = m_impl->control->get_Version(&version);
    if (SUCCEEDED(versionResult) && version) {
        m_controlVersion.assign(version, SysStringLen(version));
        SysFreeString(version);
    }
    writeLog(L"ActiveX control: progId=" + m_controlProgId
        + L" version=" + (m_controlVersion.empty()
            ? L"unknown" : m_controlVersion)
        + L" versionResult="
        + hexadecimalValue(static_cast<unsigned long>(versionResult)));

    // Attach the control to our container through the OLE interfaces.
    IOleObject *oleObject = nullptr;
    result = m_impl->control.QueryInterface(IID_IOleObject,
                                            reinterpret_cast<void **>(&oleObject));
    if (FAILED(result) || !oleObject)
        return false;
    m_impl->site = new ActiveXSite(m_container);
    result = oleObject->SetClientSite(m_impl->site);
    if (SUCCEEDED(result)) {
        RECT position{0, 0, width, height};
        oleObject->DoVerb(OLEIVERB_INPLACEACTIVATE, nullptr, m_impl->site,
                          -1, m_container, &position);
    }
    IOleInPlaceObject *inPlace = nullptr;
    if (SUCCEEDED(m_impl->control.QueryInterface(
            IID_IOleInPlaceObject, reinterpret_cast<void **>(&inPlace)))
        && inPlace) {
        m_impl->site->attachObject(inPlace);
        RECT position{0, 0, width, height};
        inPlace->SetObjectRects(&position, &position);
        // The site holds a reference; drop our temporary one.
        inPlace->Release();
    }
    // Make sure the control's own window is visible inside the container.
    IOleWindow *oleWindow = nullptr;
    if (SUCCEEDED(m_impl->control.QueryInterface(
            IID_IOleWindow, reinterpret_cast<void **>(&oleWindow)))
        && oleWindow) {
        HWND controlWindow = nullptr;
        if (SUCCEEDED(oleWindow->GetWindow(&controlWindow))
            && controlWindow)
            m_controlWindow = controlWindow;
        if (m_controlWindow) {
            SetPropW(m_controlWindow, kControlSessionProperty, this);
            m_originalControlProcedure = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(
                    m_controlWindow, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(
                        &RdpSession::controlWindowProcedure)));
        }
        if (m_controlWindow)
            ShowWindow(m_controlWindow, SW_SHOW);
        oleWindow->Release();
    }
    oleObject->Release();

    // Advise connection events so connect() progress reaches the frontend.
    if (m_stateHandler) {
        IConnectionPointContainer *container = nullptr;
        if (SUCCEEDED(m_impl->control.QueryInterface(
                IID_IConnectionPointContainer,
                reinterpret_cast<void **>(&container)))
            && container) {
            if (SUCCEEDED(container->FindConnectionPoint(
                    __uuidof(IMsTscAxEvents), &m_impl->connectionPoint))) {
                RdpSession *self = this;
                m_impl->eventSink = new RdpEventSink(
                    [self](const std::wstring &state) {
                        self->emitState(state);
                    },
                    [self](bool entered) {
                        self->writeLog(std::wstring(
                            L"  native fullscreen event: ")
                            + (entered ? L"entered" : L"left"));
                        if (self->m_nativeFullscreenHandler)
                            self->m_nativeFullscreenHandler(entered);
                    });
                const HRESULT advise = m_impl->connectionPoint->Advise(
                    m_impl->eventSink, &m_impl->adviseCookie);
                if (FAILED(advise)) {
                    emitState(L"error:无法订阅 RDP 控件事件（0x"
                        + std::to_wstring(advise) + L"）");
                    m_impl->connectionPoint->Release();
                    m_impl->connectionPoint = nullptr;
                } else {
                    writeLog(L"event sink advised, cookie="
                        + std::to_wstring(m_impl->adviseCookie));
                }
            } else {
                emitState(L"error:无法订阅 RDP 控件事件");
            }
            container->Release();
        }
    }
    return true;
}

void RdpSession::connect(const std::wstring &host, int port,
                         const std::wstring &user,
                         const std::wstring &password,
                         const RdpConnectionOptions &options)
{
    if (!m_impl->control)
        return;
    writeLog(L"connect: host=" + host + L" port=" + std::to_wstring(port)
        + L" user=" + user);
    m_visible = false;
    m_surfaceRefreshNeedsDisplayUpdate = false;
    m_displaySettingsSynchronized = false;
    m_quality = RdpQualitySnapshot{};
    m_quality.lastOperation = L"连接参数准备";
    if (m_container)
        ShowWindow(m_container, SW_HIDE);
    if (m_controlWindow && m_controlWindow != m_container)
        ShowWindow(m_controlWindow, SW_HIDE);
    emitState(L"connecting");
    IMsTscAxPtr &control = m_impl->control;
    // AdvancedSettings2 lives on IMsRdpClient and newer; query the highest
    // interface the control implements.
    IMsRdpClientPtr client;
    if (FAILED(control.QueryInterface(IID_IMsRdpClient,
                                      reinterpret_cast<void **>(&client))))
        client = nullptr;
    writeLog(L"  IMsRdpClient: "
        + std::wstring(client ? L"ok" : L"FAILED"));
    _bstr_t server(host.c_str());
    _bstr_t userName(user.c_str());
    control->put_Server(server);
    control->put_UserName(userName);
    m_loopbackConnection = isLoopbackHost(host);
    m_lanConnection = isLanHost(host);
    m_quality.networkConnectionType = m_lanConnection ? 6 : 0;
    writeLog(L"  network locality: loopback="
        + std::wstring(m_loopbackConnection ? L"TRUE" : L"FALSE")
        + L" LAN=" + std::wstring(m_lanConnection ? L"TRUE" : L"FALSE"));
    if (m_lanConnection) {
        // Frame-buffer redirection is a loopback optimization. It is not a
        // generic LAN quality switch; applying it to a VMware/PVE peer can
        // select a different compositor path from the one used by mstsc.
        IMsRdpExtendedSettingsPtr extendedSettings;
        if (m_loopbackConnection && SUCCEEDED(control.QueryInterface(
                IID_IMsRdpExtendedSettings,
                reinterpret_cast<void **>(&extendedSettings)))
            && extendedSettings) {
            VARIANT enabled;
            VariantInit(&enabled);
            enabled.vt = VT_BOOL;
            enabled.boolVal = VARIANT_TRUE;
            _bstr_t propertyName(L"EnableFrameBufferRedirection");
            const HRESULT frameBufferResult =
                extendedSettings->put_Property(propertyName, &enabled);
            writeLog(L"  loopback: EnableFrameBufferRedirection=TRUE -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    frameBufferResult)));
        } else if (m_loopbackConnection) {
            writeLog(L"  loopback: IMsRdpExtendedSettings QI FAILED");
        } else {
            writeLog(L"  LAN: EnableFrameBufferRedirection not enabled (loopback only)");
        }
    }
    IMsRdpClientNonScriptable3Ptr nonScriptable;
    if (SUCCEEDED(control.QueryInterface(
            IID_IMsRdpClientNonScriptable3,
            reinterpret_cast<void **>(&nonScriptable)))
        && nonScriptable) {
        const HRESULT credSspResult =
            nonScriptable->put_EnableCredSspSupport(VARIANT_TRUE);
        writeLog(L"  EnableCredSspSupport(TRUE) -> 0x"
            + std::to_wstring(static_cast<unsigned long>(credSspResult)));
        const HRESULT negotiateSecurityResult =
            nonScriptable->put_NegotiateSecurityLayer(VARIANT_TRUE);
        writeLog(L"  NegotiateSecurityLayer(TRUE) -> 0x"
            + std::to_wstring(
                static_cast<unsigned long>(negotiateSecurityResult)));
        if (m_useMultimon && !m_containerHandledFullscreen) {
            // Microsoft requires the connection-bar text to be assigned
            // before FullScreen is enabled. FullScreen itself is deliberately
            // deferred until OnConnected: setting it before Connect() creates
            // native presentation windows behind the embedded container on
            // some Windows 11 mstscax builds, producing a white desktop.
            const HRESULT connectionBarTextResult =
                nonScriptable->put_ConnectionBarText(
                    _bstr_t(L"MasterTerm RDP"));
            writeLog(L"  multimon: ConnectionBarText -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    connectionBarTextResult)));
            writeLog(L"  multimon: FullScreen deferred until connected");
        } else if (m_useMultimon) {
            writeLog(L"  multimon: MasterTerm container owns fullscreen");
        }
    } else {
        writeLog(L"  IMsRdpClientNonScriptable3: QI FAILED");
    }
    if (client && m_useMultimon) {
        IMsTscAdvancedSettingsPtr fullscreenSettings;
        if (SUCCEEDED(client->get_AdvancedSettings(&fullscreenSettings))
            && fullscreenSettings) {
            // MasterTerm's borderless native window can span the exact local
            // monitor topology. Keep mstscax embedded in that window so it
            // does not create a second UIMainClass whose viewport remains
            // primary-monitor sized on mixed-resolution systems.
            const long containerHandled =
                m_containerHandledFullscreen ? 1 : 0;
            const HRESULT containerResult =
                fullscreenSettings->put_ContainerHandledFullScreen(
                    containerHandled);
            long actualContainerHandled = 1;
            const HRESULT containerReadResult =
                fullscreenSettings->get_ContainerHandledFullScreen(
                    &actualContainerHandled);
            writeLog(L"  multimon: ContainerHandledFullScreen="
                + std::to_wstring(containerHandled) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    containerResult)) + L" readback="
                + std::to_wstring(actualContainerHandled) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    containerReadResult)));
        } else {
            writeLog(L"  multimon: IMsTscAdvancedSettings QI FAILED");
        }
    }
    // UseMultimon is applied by WebViewWindow only for the temporary
    // container-hosted all-monitor session.  The ordinary connection remains
    // a normal single-monitor embedded ActiveX surface.
    if (client) {
        // The embedded control otherwise falls back to a low-color desktop
        // and the frontend's resize-driven desktop dimensions make
        // SmartSizing blur the image.  Use the same sensible defaults as a
        // normal modern RDP client; profiles can still override them in the
        // advanced connection settings.
        const int colorDepth = options.colorDepth == 16
            || options.colorDepth == 24 || options.colorDepth == 32
            ? options.colorDepth : 32;
        client->put_ColorDepth(colorDepth);
        if (m_lanConnection) {
            // The stock ActiveX control may use compressed bitmap updates
            // even when the profile says LAN. That can leave different
            // regions of a mostly static desktop at different sharpness
            // levels until a later repaint. Prefer lossless bitmap delivery
            // for local/VM traffic; WAN profiles retain the normal codec.
            IMsTscAdvancedSettingsPtr legacySettings;
            if (SUCCEEDED(client->get_AdvancedSettings(&legacySettings))
                && legacySettings) {
                const HRESULT compressResult =
                    legacySettings->put_Compress(0);
                const HRESULT persistenceResult =
                    legacySettings->put_BitmapPeristence(1);
                writeLog(L"  LAN: Compress=0 -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        compressResult))
                    + L" BitmapPersistence=1 -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        persistenceResult)));
                long actualCompress = 0;
                long actualBitmapPersistence = 0;
                const HRESULT compressReadResult =
                    legacySettings->get_Compress(&actualCompress);
                const HRESULT persistenceReadResult =
                    legacySettings->get_BitmapPeristence(
                        &actualBitmapPersistence);
                writeLog(L"  LAN readback: Compress="
                    + std::to_wstring(actualCompress) + L" -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        compressReadResult))
                    + L" BitmapPersistence="
                    + std::to_wstring(actualBitmapPersistence) + L" -> 0x"
                    + std::to_wstring(static_cast<unsigned long>(
                        persistenceReadResult)));
            } else {
                writeLog(L"  LAN: IMsTscAdvancedSettings QI FAILED");
            }
        }
        IMsRdpClientSecuredSettingsPtr securedSettings;
        if (SUCCEEDED(client->get_SecuredSettings2(&securedSettings))
            && securedSettings) {
            const int keyboardHookMode = options.keyboardHookMode >= 0
                && options.keyboardHookMode <= 2
                ? options.keyboardHookMode : 1;
            const HRESULT hookResult =
                securedSettings->put_KeyboardHookMode(keyboardHookMode);
            writeLog(L"  KeyboardHookMode="
                + std::to_wstring(keyboardHookMode) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(hookResult)));
        } else {
            writeLog(L"  get_SecuredSettings2 FAILED");
        }
        IMsRdpClientAdvancedSettingsPtr baseSettings;
        IMsRdpClientAdvancedSettings8Ptr settings;
        if (SUCCEEDED(client->get_AdvancedSettings2(&baseSettings))
            && baseSettings) {
            // ContainerHandled tells the control the container manages the
            // window; required for embedded (non-fullscreen) usage.  The
            // property is missing from the type library, so set it through
            // IDispatch by name.
            IDispatch *settingsDispatch = nullptr;
            if (SUCCEEDED(baseSettings.QueryInterface(
                    IID_IDispatch,
                    reinterpret_cast<void **>(&settingsDispatch)))
                && settingsDispatch) {
                DISPID propertyId = DISPID_UNKNOWN;
                OLECHAR propertyName[] = L"ContainerHandled";
                LPOLESTR names[] = {propertyName};
                if (SUCCEEDED(settingsDispatch->GetIDsOfNames(
                        IID_NULL, names, 1, LOCALE_USER_DEFAULT,
                        &propertyId))) {
                    VARIANT value;
                    VariantInit(&value);
                    value.vt = VT_BOOL;
                    value.boolVal = VARIANT_TRUE;
                    DISPPARAMS params{};
                    params.cArgs = 1;
                    params.rgvarg = &value;
                    settingsDispatch->Invoke(
                        propertyId, IID_NULL, LOCALE_USER_DEFAULT,
                        DISPATCH_PROPERTYPUT, &params, nullptr, nullptr,
                        nullptr);
                    writeLog(L"  ContainerHandled=TRUE (via IDispatch)");
                }
                settingsDispatch->Release();
            }
            if (FAILED(baseSettings.QueryInterface(
                    IID_IMsRdpClientAdvancedSettings8,
                    reinterpret_cast<void **>(&settings))))
                settings = nullptr;
            writeLog(L"  AdvancedSettings8: "
                + std::wstring(settings ? L"ok" : L"QI FAILED"));
        } else {
            writeLog(L"  get_AdvancedSettings2 FAILED");
        }
        if (settings) {
            const bool smartSizing = options.smartSizing < 0
                ? false : options.smartSizing != 0;
            client->put_ColorDepth(colorDepth);
            settings->put_SmartSizing(
                smartSizing ? VARIANT_TRUE : VARIANT_FALSE);
            // FullMode plus an explicit connection profile keeps sessions out
            // of the adaptive low-bandwidth graphics path from the first
            // frame. Remote connections retain automatic bandwidth tuning.
            const HRESULT protocolResult =
                settings->put_ClientProtocolSpec(FullMode);
            const int configuredNetworkType =
                options.networkConnectionType >= 1
                && options.networkConnectionType <= 6
                ? options.networkConnectionType
                : -1;
            const int networkType = configuredNetworkType >= 1
                ? configuredNetworkType : (m_lanConnection ? 6 : -1);
            const HRESULT bandwidthResult =
                settings->put_BandwidthDetection(
                    networkType >= 1 ? VARIANT_FALSE : VARIANT_TRUE);
            HRESULT connectionTypeResult = S_FALSE;
            if (networkType >= 1)
                connectionTypeResult = settings->put_NetworkConnectionType(
                    networkType);
            writeLog(L"  quality: ClientProtocolSpec=FullMode -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    protocolResult))
                + L" BandwidthDetection="
                + std::wstring(networkType >= 1 ? L"FALSE" : L"TRUE")
                + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    bandwidthResult))
                + L" NetworkConnectionType="
                + (networkType >= 1
                    ? std::to_wstring(networkType) : std::wstring(L"auto"))
                + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    connectionTypeResult)));
            settings->put_HotKeyCtrlAltDel(VK_END);
            settings->put_EnableWindowsKey(1);
            settings->put_GrabFocusOnConnect(VARIANT_TRUE);
            int performanceFlags = options.performanceFlags >= 0
                ? options.performanceFlags : 0;
            const auto setPerformanceFlag = [&performanceFlags](
                int option, int bit, bool enabledBit) {
                if (option < 0)
                    return;
                if (enabledBit ? option != 0 : option == 0)
                    performanceFlags |= bit;
                else
                    performanceFlags &= ~bit;
            };
            const auto defaultVisualOption = [](int option) {
                return option < 0 ? 1 : option;
            };
            setPerformanceFlag(defaultVisualOption(options.desktopBackground),
                0x001, false);
            setPerformanceFlag(defaultVisualOption(options.fullWindowDrag),
                0x002, false);
            setPerformanceFlag(defaultVisualOption(options.menuAnimations),
                0x004, false);
            setPerformanceFlag(defaultVisualOption(options.visualStyles),
                0x008, false);
            setPerformanceFlag(defaultVisualOption(options.cursorShadow),
                0x020, false);
            setPerformanceFlag(defaultVisualOption(options.cursorSettings),
                0x040, false);
            setPerformanceFlag(defaultVisualOption(options.fontSmoothing),
                0x080, true);
            setPerformanceFlag(defaultVisualOption(options.desktopComposition),
                0x100, true);
            settings->put_PerformanceFlags(performanceFlags);
            writeLog(L"  PerformanceFlags="
                + std::to_wstring(performanceFlags));
            long actualColorDepth = 0;
            VARIANT_BOOL actualSmartSizing = VARIANT_FALSE;
            long actualPerformanceFlags = 0;
            unsigned int actualNetworkType = 0;
            VARIANT_BOOL actualBandwidthDetection = VARIANT_FALSE;
            ClientSpec actualProtocol = FullMode;
            const HRESULT colorReadResult =
                client->get_ColorDepth(&actualColorDepth);
            const HRESULT smartSizingReadResult =
                settings->get_SmartSizing(&actualSmartSizing);
            const HRESULT performanceReadResult =
                settings->get_PerformanceFlags(&actualPerformanceFlags);
            const HRESULT networkTypeReadResult =
                settings->get_NetworkConnectionType(&actualNetworkType);
            const HRESULT bandwidthReadResult =
                settings->get_BandwidthDetection(&actualBandwidthDetection);
            const HRESULT protocolReadResult =
                settings->get_ClientProtocolSpec(&actualProtocol);
            writeLog(L"  quality readback: ColorDepth="
                + std::to_wstring(actualColorDepth) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    colorReadResult))
                + L" SmartSizing="
                + std::wstring(actualSmartSizing == VARIANT_TRUE
                    ? L"TRUE" : L"FALSE") + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    smartSizingReadResult))
                + L" PerformanceFlags="
                + std::to_wstring(actualPerformanceFlags) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    performanceReadResult))
                + L" NetworkConnectionType="
                + std::to_wstring(actualNetworkType) + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    networkTypeReadResult))
                + L" BandwidthDetection="
                + std::wstring(actualBandwidthDetection == VARIANT_TRUE
                    ? L"TRUE" : L"FALSE") + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    bandwidthReadResult))
                + L" ClientProtocolSpec="
                + std::to_wstring(static_cast<int>(actualProtocol))
                + L" -> 0x"
                + std::to_wstring(static_cast<unsigned long>(
                    protocolReadResult)));
            m_quality.colorDepth = static_cast<int>(actualColorDepth);
            m_quality.smartSizing = actualSmartSizing == VARIANT_TRUE;
            m_quality.performanceFlags = static_cast<int>(actualPerformanceFlags);
            m_quality.networkConnectionType =
                static_cast<int>(actualNetworkType);
            m_quality.bandwidthDetection =
                actualBandwidthDetection == VARIANT_TRUE;
            m_quality.clientProtocolSpec = static_cast<int>(actualProtocol);
            m_quality.lastOperation = L"连接参数回读";
            m_quality.lastHresult = static_cast<unsigned long>(
                protocolReadResult);
            m_quality.lastHresultSucceeded = SUCCEEDED(protocolReadResult);
            emitQuality();
            if (options.redirectClipboard >= 0)
                settings->put_RedirectClipboard(
                    options.redirectClipboard != 0
                        ? VARIANT_TRUE : VARIANT_FALSE);
            if (options.audioRedirectionMode >= 0
                && options.audioRedirectionMode <= 2) {
                settings->put_AudioRedirectionMode(
                    static_cast<unsigned int>(options.audioRedirectionMode));
            }
            if (options.redirectDrives >= 0)
                settings->put_RedirectDrives(
                    options.redirectDrives != 0
                        ? VARIANT_TRUE : VARIANT_FALSE);
            if (options.redirectPrinters >= 0)
                settings->put_RedirectPrinters(
                    options.redirectPrinters != 0
                        ? VARIANT_TRUE : VARIANT_FALSE);
            if (options.autoReconnect >= 0) {
                settings->put_EnableAutoReconnect(
                    options.autoReconnect != 0
                        ? VARIANT_TRUE : VARIANT_FALSE);
                if (options.autoReconnect != 0)
                    settings->put_MaxReconnectAttempts(std::clamp(
                        options.maxReconnectAttempts, 1, 20));
            }
            if (port != 3389)
                settings->put_RDPPort(port);
            if (!password.empty()) {
                _bstr_t clearPassword(password.c_str());
                settings->put_ClearTextPassword(clearPassword);
            }
        }
    }
    RECT clientRect{};
    GetClientRect(m_container, &clientRect);
    m_displayWidth = clientRect.right - clientRect.left;
    m_displayHeight = clientRect.bottom - clientRect.top;
    m_quality.displayWidth = m_displayWidth;
    m_quality.displayHeight = m_displayHeight;
    emitQuality();
    // DesktopWidth/DesktopHeight describe the initial desktop size. They are
    // not the virtual bounding box in true multimon mode; passing 4480x1440
    // here makes several mstscax builds negotiate a single stretched surface
    // and then paint a blank frame. Use the primary monitor's dimensions for
    // the initial values and let UseMultimon negotiate the monitor topology.
    const int initialDesktopWidth = m_useMultimon
        ? std::max(1, GetSystemMetrics(SM_CXSCREEN)) : m_displayWidth;
    const int initialDesktopHeight = m_useMultimon
        ? std::max(1, GetSystemMetrics(SM_CYSCREEN)) : m_displayHeight;
    control->put_DesktopWidth(static_cast<LONG>(initialDesktopWidth));
    control->put_DesktopHeight(static_cast<LONG>(initialDesktopHeight));
    writeLog(L"  DesktopSize="
        + std::to_wstring(initialDesktopWidth) + L"x"
        + std::to_wstring(initialDesktopHeight)
        + (m_useMultimon ? L" (primary monitor; multimon topology)"
                         : L" (embedded surface)"));
    HRESULT result = control->Connect();
    writeLog(L"  Connect() -> 0x"
        + std::to_wstring(static_cast<unsigned long>(result)));
    m_quality.lastOperation = L"Connect()";
    m_quality.lastHresult = static_cast<unsigned long>(result);
    m_quality.lastHresultSucceeded = SUCCEEDED(result);
    emitQuality();
    if (FAILED(result)) {
        m_connecting = false;
        emitState(L"error:无法启动 RDP 连接");
        return;
    }
    // Connect() is asynchronous.  The connected flag is set only after the
    // control raises OnConnected or OnLoginComplete.
    m_connecting = true;
    if (m_container)
        SetTimer(m_container, RdpConnectTimeoutTimer, 30000, nullptr);
}

void RdpSession::disconnect()
{
    if (!m_impl->control)
        return;
    if (m_connected || m_connecting) {
        if (m_container) {
            KillTimer(m_container, RdpConnectTimeoutTimer);
            KillTimer(m_container, RdpInitialSurfaceRepaintTimer);
            KillTimer(m_container, RdpNativeFullscreenTimer);
        }
        // Do not call Disconnect() and immediately release the ActiveX
        // control.  Disconnect is synchronous and can dispatch
        // OnDisconnected while the caller is already tearing down the event
        // sink/site, which is a reliable way to trigger heap corruption.
        // Releasing the control during the normal destructor sequence tears
        // down the RDP session without re-entering the partially destroyed
        // host.
        m_connected = false;
        m_connecting = false;
    }
}

void RdpSession::resize(
    int x, int y, int width, int height, bool updateDisplaySettings,
    const std::vector<RdpOcclusionRegion> &occlusionRects, bool modalDimmed)
{
    if (!m_container || width <= 0 || height <= 0)
        return;
    const bool boundsChanged = x != m_hostX || y != m_hostY
        || width != m_hostWidth || height != m_hostHeight;
    const bool controlSizeChanged = width != m_hostWidth
        || height != m_hostHeight;
    // Connected background sessions remain below WebView2 instead of being
    // hidden.  Also avoid moving/re-sizing the HWND when its bounds did not
    // change; redundant SetWindowPos/SetObjectRects calls make mstscax repaint
    // a black frame during an otherwise size-neutral tab switch.
    if (boundsChanged) {
        SetWindowPos(
            m_container, m_hostedVisible ? HWND_TOP : HWND_BOTTOM,
            x, y, width, height,
            SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        m_hostX = x;
        m_hostY = y;
        m_hostWidth = width;
        m_hostHeight = height;
    }
    // The RDP ActiveX child is above WebView2. Subtract HTML dialog/menu
    // rectangles from the native host region instead of hiding the desktop,
    // allowing the WebView2 content to show through those precise holes.
    applyOcclusionRegion(x, y, width, height, occlusionRects);
    updateModalDimOverlay(
        x, y, width, height, modalDimmed, occlusionRects);
    if (m_impl->control) {
        // The terminal tab bar and all RDP lifecycle actions are owned by
        // the WebView frontend. The native control therefore fills the
        // content rectangle without reserving a second status strip.
        const int desktopHeight = std::max(1, height);
        // These properties are consumed by Connect() for the initial remote
        // desktop size. Keep them updated for single-monitor sessions. In
        // true multimon mode the monitor topology was negotiated at Connect()
        // and must not be overwritten with the virtual bounding rectangle.
        const bool displaySizeChanged = width != m_displayWidth
            || desktopHeight != m_displayHeight;
        if (updateDisplaySettings) {
            if (!m_useMultimon) {
                m_impl->control->put_DesktopWidth(static_cast<LONG>(width));
                m_impl->control->put_DesktopHeight(static_cast<LONG>(desktopHeight));
            }
            const bool shouldUpdateDisplaySettings = m_connected
                && (displaySizeChanged || !m_displaySettingsSynchronized);
            if (shouldUpdateDisplaySettings) {
                if (m_useMultimon) {
                    // The monitor topology is fixed by UseMultimon before
                    // Connect(). Do not synchronize it with the
                    // single-rectangle resize API after the session starts.
                    m_quality.displayWidth = width;
                    m_quality.displayHeight = desktopHeight;
                    m_quality.lastOperation = L"多显示器布局保留";
                    m_quality.lastHresult = S_OK;
                    m_quality.lastHresultSucceeded = true;
                    emitQuality();
                    writeLog(
                        std::wstring(L"  multimon: keep monitor topology negotiated at Connect() (")
                        + std::to_wstring(width) + L"x"
                        + std::to_wstring(desktopHeight) + L")");
                } else {
                    IMsRdpClient9Ptr client9;
                    if (SUCCEEDED(m_impl->control.QueryInterface(
                            __uuidof(IMsRdpClient9),
                            reinterpret_cast<void **>(&client9)))
                        && client9) {
                    const HRESULT result = client9->UpdateSessionDisplaySettings(
                            static_cast<unsigned long>(width),
                            static_cast<unsigned long>(desktopHeight),
                            static_cast<unsigned long>(width),
                            static_cast<unsigned long>(desktopHeight),
                            0, 100, 100);
                    m_quality.displayWidth = width;
                    m_quality.displayHeight = desktopHeight;
                    m_quality.lastOperation = L"分辨率刷新";
                    m_quality.lastHresult = static_cast<unsigned long>(result);
                    m_quality.lastHresultSucceeded = SUCCEEDED(result);
                    emitQuality();
                    writeLog(std::wstring(L"  UpdateSessionDisplaySettings(")
                        + std::to_wstring(width) + L"x"
                        + std::to_wstring(desktopHeight) + L") -> 0x"
                        + std::to_wstring(
                            static_cast<unsigned long>(result)));
                }
            }
            }
            if (displaySizeChanged || m_connected) {
                m_displayWidth = width;
                m_displayHeight = desktopHeight;
                m_quality.displayWidth = width;
                m_quality.displayHeight = desktopHeight;
            }
            if (shouldUpdateDisplaySettings)
                m_displaySettingsSynchronized = true;
            if (m_connected && m_lanConnection && displaySizeChanged)
                scheduleSurfaceRefresh(false);
        }
        if (controlSizeChanged) {
            IOleInPlaceObject *inPlace = nullptr;
            if (SUCCEEDED(m_impl->control.QueryInterface(
                    IID_IOleInPlaceObject,
                    reinterpret_cast<void **>(&inPlace)))
                && inPlace) {
                RECT position{0, 0, width, height};
                inPlace->SetObjectRects(&position, &position);
                inPlace->Release();
            }
        }
    }
}
