#include "WebViewWindow.h"

#include "WebViewBackend.h"
#include "WebViewHost.h"
#include "RdpSession.h"
#include "DiagnosticLog.h"

#include <algorithm>
#include <commctrl.h>
#include <gdiplus.h>
#include <iomanip>
#include <objidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <tlhelp32.h>
#include <cstring>
#include <string>

struct WebViewWindow::RdpHostedSession {
    std::unique_ptr<RdpSession> session;
    int profileIndex = -1;
    std::wstring host;
    std::wstring user;
    std::wstring password;
    int port = 3389;
    RdpConnectionOptions options;
    bool connectPending = false;
    unsigned long long failureCleanupToken = 0;
    bool failureCleanupQueued = false;
};

namespace {

constexpr wchar_t WindowClassName[] = L"MasterTerm.WebView2.Window";
constexpr wchar_t RdpFullscreenOverlayClassName[] =
    L"MasterTerm.RdpFullscreenOverlay";
constexpr wchar_t NativePopupMenuClassName[] =
    L"MasterTerm.NativePopupMenu";
constexpr wchar_t WindowPlacementRegistryKey[] =
    L"Software\\MasterTerm";
constexpr wchar_t WindowPlacementRegistryValue[] = L"WindowPlacement";
constexpr int MasterTermIconResourceId = 101;
constexpr int MasterTermSplashLogoResourceId = 102;

struct VirtualMonitorLayout {
    RECT bounds{};
    unsigned int count = 0;
};

BOOL CALLBACK collectVirtualMonitorBounds(
    HMONITOR, HDC, LPRECT monitorBounds, LPARAM parameter)
{
    auto *layout = reinterpret_cast<VirtualMonitorLayout *>(parameter);
    if (!layout || !monitorBounds)
        return FALSE;
    if (layout->count == 0)
        layout->bounds = *monitorBounds;
    else
        UnionRect(&layout->bounds, &layout->bounds, monitorBounds);
    ++layout->count;
    return TRUE;
}

bool queryVirtualMonitorLayout(VirtualMonitorLayout &layout)
{
    layout = {};
    return EnumDisplayMonitors(
        nullptr, nullptr, collectVirtualMonitorBounds,
        reinterpret_cast<LPARAM>(&layout))
        && layout.count > 0
        && layout.bounds.right > layout.bounds.left
        && layout.bounds.bottom > layout.bounds.top;
}

void setTaskbarTabVisible(HWND window, bool visible)
{
    ITaskbarList *taskbar = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITaskbarList, reinterpret_cast<void **>(&taskbar)))
        || !taskbar)
        return;
    if (SUCCEEDED(taskbar->HrInit())) {
        if (visible)
            taskbar->AddTab(window);
        else
            taskbar->DeleteTab(window);
    }
    taskbar->Release();
}

// Shutdown watchdog fallback: when graceful teardown misses its deadline
// (e.g. a WebView2 teardown or a session thread that will not join), the
// process would otherwise hang forever.  Kill any leftover SFTP worker
// processes (only when no other MasterTerm instance is running, so another
// instance's transfers are untouched) and then terminate the process itself.
void terminateSftpWorkerProcesses()
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

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"MasterTermSftpWorker.exe") != 0)
                continue;
            const HANDLE process = OpenProcess(
                PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (process) {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        length = MultiByteToWideChar(
            CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), result.data(), length);
        return result;
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
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
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

int scaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

bool loadSavedWindowPlacement(WINDOWPLACEMENT &placement)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER, WindowPlacementRegistryKey, 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD size = sizeof(placement);
    const LONG result = RegQueryValueExW(
        key, WindowPlacementRegistryValue, nullptr, &type,
        reinterpret_cast<BYTE *>(&placement), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_BINARY
        || size != sizeof(placement))
        return false;
    if (placement.showCmd == SW_SHOWMINIMIZED)
        placement.showCmd = SW_SHOWNORMAL;
    if (placement.showCmd != SW_SHOWNORMAL
        && placement.showCmd != SW_SHOWMAXIMIZED)
        placement.showCmd = SW_SHOWNORMAL;
    if (placement.rcNormalPosition.right <= placement.rcNormalPosition.left
        || placement.rcNormalPosition.bottom <= placement.rcNormalPosition.top)
        return false;
    // A monitor may have been disconnected since the previous run.  In that
    // case Windows' default placement is safer than restoring an invisible
    // window.
    return MonitorFromRect(
               &placement.rcNormalPosition, MONITOR_DEFAULTTONULL)
        != nullptr;
}

void writeSavedWindowPlacement(const WINDOWPLACEMENT &placement)
{
    if (placement.showCmd == SW_SHOWMINIMIZED
        || placement.rcNormalPosition.right <= placement.rcNormalPosition.left
        || placement.rcNormalPosition.bottom <= placement.rcNormalPosition.top)
        return;
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, WindowPlacementRegistryKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS)
        return;
    RegSetValueExW(
        key, WindowPlacementRegistryValue, 0, REG_BINARY,
        reinterpret_cast<const BYTE *>(&placement), sizeof(placement));
    RegCloseKey(key);
}

void rdpFullscreenOverlayButtonRect(int index, UINT dpi, RECT &rect)
{
    const auto px = [dpi](int value) {
        return scaleForDpi(value, dpi);
    };
    constexpr int left[] = {54, 104, 154, 204, 254, 304};
    constexpr int width = 42;
    if (index < 0 || index >= 6) {
        rect = {};
        return;
    }
    rect.left = px(left[index]);
    rect.top = px(8);
    rect.right = px(left[index] + width);
    rect.bottom = px(44);
}

int rdpFullscreenOverlayMenuItemCount(int menu)
{
    if (menu == 1)
        return 3;
    if (menu == 2)
        return 3;
    if (menu == 3)
        return 6;
    return 0;
}

const wchar_t *rdpFullscreenOverlayTooltip(int index, bool pinned)
{
    if (index == 0)
        return pinned ? L"取消固定控制栏" : L"固定控制栏";
    if (index == 1)
        return L"显示设置";
    if (index == 2)
        return L"连接设置";
    if (index == 3)
        return L"远程工具";
    if (index == 4)
        return L"实际质量";
    if (index == 5)
        return L"关闭连接";
    return L"";
}

struct RdpFullscreenOverlayPalette {
    COLORREF background;
    COLORREF border;
    COLORREF text;
    COLORREF button;
    COLORREF buttonHover;
    COLORREF buttonActive;
    COLORREF buttonBorder;
    COLORREF buttonHoverBorder;
    COLORREF closeText;
};

RdpFullscreenOverlayPalette rdpFullscreenOverlayPalette(int theme)
{
    if (theme == 1) {
        return {
            RGB(248, 250, 252), RGB(148, 163, 184), RGB(30, 41, 59),
            RGB(226, 232, 240), RGB(37, 99, 235), RGB(219, 234, 254),
            RGB(148, 163, 184), RGB(96, 165, 250), RGB(220, 38, 38)};
    }
    if (theme == 2) {
        return {
            RGB(9, 35, 60), RGB(40, 97, 140), RGB(204, 229, 255),
            RGB(13, 44, 75), RGB(30, 94, 139), RGB(20, 76, 116),
            RGB(40, 97, 140), RGB(96, 165, 250), RGB(248, 113, 113)};
    }
    return {
        RGB(15, 23, 42), RGB(51, 65, 85), RGB(226, 232, 240),
        RGB(30, 41, 59), RGB(37, 99, 235), RGB(30, 64, 175),
        RGB(71, 85, 105), RGB(96, 165, 250), RGB(248, 113, 113)};
}

struct NativePopupMenuPalette {
    COLORREF background;
    COLORREF border;
    COLORREF text;
    COLORREF hover;
    COLORREF hoverText;
    COLORREF disabled;
    COLORREF separator;
};

NativePopupMenuPalette nativePopupMenuPalette(int theme)
{
    if (theme == 1) {
        return {
            RGB(255, 255, 255), RGB(185, 200, 218), RGB(51, 65, 85),
            RGB(231, 239, 251), RGB(29, 78, 216), RGB(148, 163, 184),
            RGB(215, 224, 235)};
    }
    if (theme == 2) {
        return {
            RGB(9, 35, 60), RGB(40, 97, 140), RGB(219, 228, 240),
            RGB(51, 65, 85), RGB(255, 255, 255), RGB(100, 116, 139),
            RGB(30, 78, 116)};
    }
    return {
        RGB(30, 41, 59), RGB(71, 85, 105), RGB(219, 228, 240),
        RGB(51, 65, 85), RGB(255, 255, 255), RGB(100, 116, 139),
        RGB(59, 75, 95)};
}

// Use the Win32 popup-menu window for menus that can be opened while the
// embedded MSTSC ActiveX control is visible.  A WebView2 element can be
// painted above HTML, but it cannot outrank the native RDP child; a real
// popup menu gets the system menu shadow and remains above that child.
void appendThemedMenuItem(
    HMENU menu, UINT command, const wchar_t *label, bool disabled = false)
{
    if (!menu || !label)
        return;
    UINT flags = MF_OWNERDRAW;
    if (disabled)
        flags |= MF_DISABLED | MF_GRAYED;
    AppendMenuW(
        menu, flags, command,
        reinterpret_cast<LPCWSTR>(label));
}

void setThemedMenuBackground(HMENU menu, COLORREF color, HBRUSH &brush)
{
    if (!menu)
        return;
    brush = CreateSolidBrush(color);
    if (!brush)
        return;
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = brush;
    SetMenuInfo(menu, &info);
}

bool isMasterTermOwnerDrawMenuItem(const MEASUREITEMSTRUCT *measure)
{
    return measure && measure->CtlType == ODT_MENU && measure->itemData;
}

bool isMasterTermOwnerDrawMenuItem(const DRAWITEMSTRUCT *draw)
{
    return draw && draw->CtlType == ODT_MENU && draw->itemData;
}

void measureThemedMenuItem(MEASUREITEMSTRUCT &measure, UINT dpi, int width)
{
    measure.itemWidth = static_cast<UINT>(scaleForDpi(width, dpi));
    measure.itemHeight = static_cast<UINT>(scaleForDpi(30, dpi));
}

COLORREF themePresetColor(UINT command)
{
    switch (command) {
    case WebViewWindow::ThemePresetDarkCustom: return RGB(11, 18, 32);
    case WebViewWindow::ThemePresetDarkOneDark: return RGB(40, 44, 52);
    case WebViewWindow::ThemePresetDarkDracula: return RGB(40, 42, 54);
    case WebViewWindow::ThemePresetDarkNord: return RGB(46, 52, 64);
    case WebViewWindow::ThemePresetDarkMonokai: return RGB(45, 42, 46);
    case WebViewWindow::ThemePresetDarkSolarizedDark: return RGB(0, 43, 54);
    case WebViewWindow::ThemePresetDarkCatppuccinMocha: return RGB(30, 30, 46);
    case WebViewWindow::ThemePresetDarkGithubDark: return RGB(13, 17, 23);

    case WebViewWindow::ThemePresetLightCustom: return RGB(255, 255, 255);
    case WebViewWindow::ThemePresetLightGithubLight: return RGB(255, 255, 255);
    case WebViewWindow::ThemePresetLightOneLight: return RGB(250, 250, 250);
    case WebViewWindow::ThemePresetLightSolarizedLight: return RGB(253, 246, 227);

    case WebViewWindow::ThemePresetBlueCustom: return RGB(6, 27, 48);
    default: return CLR_INVALID;
    }
}

void drawThemedMenuItem(
    const DRAWITEMSTRUCT &draw,
    const NativePopupMenuPalette &palette, UINT dpi)
{
    if (!draw.hDC)
        return;
    const RECT &rect = draw.rcItem;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool checked = (draw.itemState & ODS_CHECKED) != 0;
    HBRUSH brush = CreateSolidBrush(
        selected ? palette.hover : palette.background);
    if (brush) {
        FillRect(draw.hDC, &rect, brush);
        DeleteObject(brush);
    }
    HFONT font = CreateFontW(
        -scaleForDpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ previousFont = font
        ? SelectObject(draw.hDC, font) : nullptr;
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(
        draw.hDC,
        disabled ? palette.disabled
                 : selected ? palette.hoverText : palette.text);
    RECT text = rect;
    text.left += scaleForDpi(12, dpi);
    text.right -= scaleForDpi(12, dpi);

    const COLORREF dotColor = themePresetColor(draw.itemID);
    if (dotColor != CLR_INVALID) {
        const int dotSize = scaleForDpi(10, dpi);
        const int dotX = text.left;
        const int dotY = text.top + (text.bottom - text.top - dotSize) / 2;
        HBRUSH dotBrush = CreateSolidBrush(dotColor);
        HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(148, 163, 184));
        HGDIOBJ oldBrush = SelectObject(draw.hDC, dotBrush);
        HGDIOBJ oldPen = SelectObject(draw.hDC, dotPen);
        Ellipse(draw.hDC, dotX, dotY, dotX + dotSize, dotY + dotSize);
        SelectObject(draw.hDC, oldPen);
        SelectObject(draw.hDC, oldBrush);
        DeleteObject(dotPen);
        DeleteObject(dotBrush);
        text.left += dotSize + scaleForDpi(8, dpi);
    }

    const auto *rawLabel = reinterpret_cast<const wchar_t *>(draw.itemData);
    if (rawLabel) {
        const wchar_t *tab = wcschr(rawLabel, L'\t');
        if (tab) {
            DrawTextW(
                draw.hDC,
                rawLabel, static_cast<int>(tab - rawLabel), &text,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SetTextColor(
                draw.hDC,
                disabled ? palette.disabled
                         : selected ? palette.hoverText : palette.disabled);
            DrawTextW(
                draw.hDC,
                tab + 1, -1, &text,
                DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        } else {
            DrawTextW(
                draw.hDC,
                rawLabel, -1, &text,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            if (checked) {
                SetTextColor(
                    draw.hDC,
                    selected ? palette.hoverText : RGB(96, 165, 250));
                DrawTextW(
                    draw.hDC,
                    L"✓", -1, &text,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
        }
    }
    if (previousFont)
        SelectObject(draw.hDC, previousFont);
    if (font)
        DeleteObject(font);
}

void fillVerticalGradient(
    HDC dc, const RECT &bounds, COLORREF top, COLORREF bottom)
{
    const int height = bounds.bottom - bounds.top;
    if (height <= 0)
        return;
    const int topR = GetRValue(top), topG = GetGValue(top), topB = GetBValue(top);
    const int bottomR = GetRValue(bottom), bottomG = GetGValue(bottom),
              bottomB = GetBValue(bottom);
    const int step = std::max(1, height / 180);
    for (int y = 0; y < height; y += step) {
        const double t = static_cast<double>(y) / (height - 1);
        const HBRUSH brush = CreateSolidBrush(RGB(
            topR + static_cast<int>((bottomR - topR) * t),
            topG + static_cast<int>((bottomG - topG) * t),
            topB + static_cast<int>((bottomB - topB) * t)));
        RECT line{
            bounds.left, bounds.top + y, bounds.right,
            std::min(bounds.bottom, bounds.top + y + step)};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

void addRoundedRectPath(
    Gdiplus::GraphicsPath &path, const Gdiplus::RectF &rect, float radius)
{
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(
        rect.X + rect.Width - diameter, rect.Y, diameter, diameter,
        270.0f, 90.0f);
    path.AddArc(
        rect.X + rect.Width - diameter,
        rect.Y + rect.Height - diameter, diameter, diameter,
        0.0f, 90.0f);
    path.AddArc(
        rect.X, rect.Y + rect.Height - diameter, diameter, diameter,
        90.0f, 90.0f);
    path.CloseFigure();
}

void drawFallbackSplashLogo(
    HDC dc, int centerX, int top, int size, int pixelHeight)
{
    const HGDIOBJ previousBrush =
        SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int margin = std::max(2, size / 14);
    // Soft outer glow ring.
    const HPEN glowPen = CreatePen(
        PS_SOLID, std::max(2, size / 9), RGB(37, 99, 235));
    // Bright main ring.
    const HPEN ringPen = CreatePen(
        PS_SOLID, std::max(2, size / 24), RGB(96, 165, 250));
    // Inner accent ring.
    const HPEN accentPen = CreatePen(
        PS_SOLID, std::max(2, size / 30), RGB(34, 211, 238));

    SelectObject(dc, glowPen);
    Ellipse(dc, centerX - size / 2, top, centerX + size / 2, top + size);
    SelectObject(dc, ringPen);
    Ellipse(
        dc, centerX - size / 2 + margin, top + margin,
        centerX + size / 2 - margin, top + size - margin);

    // Disc inside the rings to create the ring gap, then the "M" glyph.
    const HBRUSH disc = CreateSolidBrush(RGB(15, 23, 42));
    SelectObject(dc, disc);
    SelectObject(dc, GetStockObject(NULL_PEN));
    const int innerSize = size - margin * 5;
    Ellipse(
        dc, centerX - innerSize / 2, top + (size - innerSize) / 2,
        centerX + innerSize / 2, top + (size + innerSize) / 2);
    // Thin accent ring just inside the disc.
    const int accentInset = std::max(2, size / 22);
    SelectObject(dc, accentPen);
    Ellipse(
        dc, centerX - innerSize / 2 + accentInset,
        top + (size - innerSize) / 2 + accentInset,
        centerX + innerSize / 2 - accentInset,
        top + (size + innerSize) / 2 - accentInset);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    DeleteObject(disc);

    const HFONT letterFont = CreateFontW(
        -MulDiv(34, pixelHeight, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HGDIOBJ previousFont = SelectObject(dc, letterFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(96, 207, 250));
    RECT letterRect{
        centerX - size / 2, top, centerX + size / 2, top + size};
    DrawTextW(
        dc, L"M", -1, &letterRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previousFont);
    DeleteObject(letterFont);

    SelectObject(dc, previousBrush);
    DeleteObject(glowPen);
    DeleteObject(ringPen);
    DeleteObject(accentPen);
}

void drawSplashLogo(
    HDC dc, Gdiplus::Bitmap *logo, int centerX, int top, int size,
    int pixelHeight)
{
    if (!logo) {
        drawFallbackSplashLogo(dc, centerX, top, size, pixelHeight);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.DrawImage(
        logo, Gdiplus::Rect(centerX - size / 2, top, size, size),
        0, 0, static_cast<INT>(logo->GetWidth()),
        static_cast<INT>(logo->GetHeight()), Gdiplus::UnitPixel);
}

std::unique_ptr<Gdiplus::Bitmap> loadSplashLogo(HINSTANCE instance)
{
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(MasterTermSplashLogoResourceId), RT_RCDATA);
    if (!resource)
        return nullptr;
    const HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD resourceSize = SizeofResource(instance, resource);
    if (!loaded || resourceSize == 0)
        return nullptr;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!memory)
        return nullptr;
    void *destination = GlobalLock(memory);
    const void *source = LockResource(loaded);
    if (!destination || !source) {
        if (destination)
            GlobalUnlock(memory);
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(destination, source, resourceSize);
    GlobalUnlock(memory);

    IStream *stream = nullptr;
    const HRESULT streamResult =
        CreateStreamOnHGlobal(memory, TRUE, &stream);
    if (FAILED(streamResult) || !stream) {
        GlobalFree(memory);
        return nullptr;
    }
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(stream);
    stream->Release();
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        return nullptr;
    return bitmap;
}

// Render the brand wordmark as a native vector-like path. This keeps it
// visible before WebView2 starts and gives the startup screen a real logo
// treatment instead of plain GDI text.
void drawSplashTitle(HDC dc, int centerX, int top, int pixelHeight)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

    auto family = std::make_unique<Gdiplus::FontFamily>(
        L"Segoe UI Variable Display");
    if (family->GetLastStatus() != Gdiplus::Ok)
        family = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    const Gdiplus::REAL fontSize = static_cast<Gdiplus::REAL>(
        MulDiv(34, pixelHeight, 72));
    Gdiplus::GraphicsPath wordmark;
    wordmark.AddString(
        L"MasterTerm", -1, family.get(), Gdiplus::FontStyleBold, fontSize,
        Gdiplus::PointF(0.0f, 0.0f), &format);
    Gdiplus::RectF bounds;
    wordmark.GetBounds(&bounds);
    Gdiplus::Matrix placement;
    placement.Translate(
        static_cast<Gdiplus::REAL>(centerX)
            - (bounds.X + bounds.Width / 2.0f),
        static_cast<Gdiplus::REAL>(top) - bounds.Y);
    wordmark.Transform(&placement);
    wordmark.GetBounds(&bounds);

    // Multiple translucent strokes produce a subtle electric-blue halo while
    // keeping the wordmark crisp at normal window sizes.
    for (int glow = 4; glow >= 1; --glow) {
        Gdiplus::Pen glowPen(
            Gdiplus::Color(static_cast<BYTE>(12 * glow), 43, 132, 255),
            static_cast<Gdiplus::REAL>(glow * 2 + 1));
        graphics.DrawPath(&glowPen, &wordmark);
    }
    Gdiplus::LinearGradientBrush fillBrush(
        Gdiplus::PointF(bounds.X, bounds.Y),
        Gdiplus::PointF(bounds.X + bounds.Width, bounds.Y),
        Gdiplus::Color(255, 245, 249, 255),
        Gdiplus::Color(255, 60, 220, 232));
    graphics.FillPath(&fillBrush, &wordmark);
    Gdiplus::Pen edgePen(Gdiplus::Color(230, 142, 220, 255), 1.0f);
    graphics.DrawPath(&edgePen, &wordmark);
}

// Static part of the splash: gradient, logo, title and subtitle/error text.
// Rendered once into a cached bitmap so redraws never flicker.
void drawSplashStatic(
    HDC dc, const RECT &client, const wchar_t *message, bool showBar,
    Gdiplus::Bitmap *logo)
{
    const int pixelHeight = GetDeviceCaps(dc, LOGPIXELSY);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return;

    // Modern deep-blue vertical gradient background.
    fillVerticalGradient(
        dc, client, RGB(13, 23, 41), RGB(7, 16, 29));

    const double dpiScale =
        static_cast<double>(pixelHeight) / USER_DEFAULT_SCREEN_DPI;
    const int centerX = width / 2;
    const int logoSize = static_cast<int>(104 * dpiScale);
    const int logoTop = MulDiv(height, 27, 100) - logoSize / 2;
    drawSplashLogo(dc, logo, centerX, logoTop, logoSize, pixelHeight);

    const HFONT textFont = CreateFontW(
        -MulDiv(12, pixelHeight, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HGDIOBJ previousFont = SelectObject(dc, textFont);
    SetBkMode(dc, TRANSPARENT);

    const int titleY =
        logoTop + logoSize + MulDiv(20, pixelHeight, 96);
    drawSplashTitle(dc, centerX, titleY, pixelHeight);

    const int subtitleY =
        titleY + MulDiv(44, pixelHeight, 96);
    if (showBar) {
        SetTextColor(dc, RGB(130, 147, 172));
        const wchar_t subtitle[] = L"Secure Connections, Unified.";
        RECT subtitleRect{
            centerX - 240, subtitleY, centerX + 240,
            subtitleY + MulDiv(26, pixelHeight, 96)};
        DrawTextW(
            dc, subtitle, -1, &subtitleRect,
            DT_CENTER | DT_TOP | DT_SINGLELINE);
    } else {
        SetTextColor(dc, RGB(252, 165, 165));
        RECT errorRect{
            centerX - 280, subtitleY, centerX + 280,
            subtitleY + MulDiv(80, pixelHeight, 96)};
        DrawTextW(
            dc, message, -1, &errorRect,
            DT_CENTER | DT_TOP | DT_WORDBREAK);
    }

    const HFONT footerFont = CreateFontW(
        -MulDiv(10, pixelHeight, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HGDIOBJ previousFooterFont = SelectObject(dc, footerFont);
    SetTextColor(dc, RGB(98, 116, 141));
    const int footerY = height - MulDiv(26, pixelHeight, 96);
    const int footerLeft = MulDiv(20, pixelHeight, 96);
    const int footerRight = width - MulDiv(20, pixelHeight, 96);
    const int dotSize = std::max(4, MulDiv(6, pixelHeight, 96));
    const HBRUSH dotBrush = CreateSolidBrush(RGB(72, 215, 176));
    const HGDIOBJ previousFooterBrush = SelectObject(dc, dotBrush);
    Ellipse(
        dc, footerLeft, footerY + MulDiv(5, pixelHeight, 96),
        footerLeft + dotSize, footerY + MulDiv(5, pixelHeight, 96) + dotSize);
    SelectObject(dc, previousFooterBrush);
    DeleteObject(dotBrush);
    RECT engineRect{
        footerLeft + dotSize + MulDiv(8, pixelHeight, 96), footerY,
        width / 2, footerY + MulDiv(18, pixelHeight, 96)};
    DrawTextW(
        dc, L"Secure connection engine", -1, &engineRect,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT versionRect{
        width / 2, footerY, footerRight,
        footerY + MulDiv(18, pixelHeight, 96)};
    DrawTextW(
        dc, L"v" MASTERTERM_VERSION_W, -1, &versionRect,
        DT_RIGHT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, previousFooterFont);
    DeleteObject(footerFont);

    SelectObject(dc, previousFont);
    DeleteObject(textFont);
}

// Dynamic part of the splash: the progress bar and status text below it.
void drawSplashProgress(
    HDC dc, const RECT &client, unsigned int ticks, bool barFull,
    const wchar_t *status)
{
    const int pixelHeight = GetDeviceCaps(dc, LOGPIXELSY);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return;
    const double dpiScale =
        static_cast<double>(pixelHeight) / USER_DEFAULT_SCREEN_DPI;
    const int centerX = width / 2;
    const int logoSize = static_cast<int>(104 * dpiScale);
    const int logoTop = MulDiv(height, 27, 100) - logoSize / 2;
    const int titleY =
        logoTop + logoSize + MulDiv(20, pixelHeight, 96);
    const int subtitleY =
        titleY + MulDiv(44, pixelHeight, 96);
    const int barWidth = static_cast<int>(260 * dpiScale);
    const int barHeight = std::max(6, static_cast<int>(6 * dpiScale));
    const int barX = centerX - barWidth / 2;
    const int barY = subtitleY + MulDiv(42, pixelHeight, 96);
    // There is no reliable percentage from WebView2 for this phase, so use
    // an indeterminate bar until the frontend signals its first valid frame.
    const int fillWidth = barFull
        ? barWidth
        : std::max(barHeight, static_cast<int>(barWidth * 0.38));
    const int cycle = std::max(1, barWidth + fillWidth);
    const int offset = barFull
        ? 0
        : static_cast<int>((ticks * 3u) % static_cast<unsigned int>(cycle));
    const int fillX = barFull ? barX : barX - fillWidth + offset;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    const Gdiplus::RectF trackRect(
        static_cast<Gdiplus::REAL>(barX), static_cast<Gdiplus::REAL>(barY),
        static_cast<Gdiplus::REAL>(barWidth),
        static_cast<Gdiplus::REAL>(barHeight));
    const float radius = std::max(2.0f, static_cast<float>(barHeight) / 2.0f);
    Gdiplus::GraphicsPath trackPath;
    addRoundedRectPath(trackPath, trackRect, radius);
    Gdiplus::SolidBrush trackBrush(Gdiplus::Color(235, 20, 31, 52));
    graphics.FillPath(&trackBrush, &trackPath);
    Gdiplus::Pen trackEdge(Gdiplus::Color(150, 51, 83, 118), 1.0f);
    graphics.DrawPath(&trackEdge, &trackPath);

    const Gdiplus::RectF fillRect(
        static_cast<Gdiplus::REAL>(fillX), static_cast<Gdiplus::REAL>(barY),
        static_cast<Gdiplus::REAL>(fillWidth),
        static_cast<Gdiplus::REAL>(barHeight));
    Gdiplus::GraphicsPath fillPath;
    addRoundedRectPath(fillPath, fillRect, radius);
    const Gdiplus::GraphicsState clipState = graphics.Save();
    graphics.SetClip(&trackPath, Gdiplus::CombineModeIntersect);

    // Layered translucent strokes provide a compact glow without painting
    // outside the track or requiring a transparent top-level window.
    for (int glow = 3; glow >= 1; --glow) {
        Gdiplus::Pen glowPen(
            Gdiplus::Color(static_cast<BYTE>(18 * glow), 52, 163, 255),
            static_cast<Gdiplus::REAL>(barHeight + glow * 3));
        graphics.DrawPath(&glowPen, &fillPath);
    }
    Gdiplus::LinearGradientBrush fillBrush(
        Gdiplus::PointF(fillRect.X, fillRect.Y),
        Gdiplus::PointF(fillRect.X + fillRect.Width, fillRect.Y),
        Gdiplus::Color(255, 48, 125, 255),
        Gdiplus::Color(255, 64, 224, 226));
    graphics.FillPath(&fillBrush, &fillPath);
    Gdiplus::Pen highlightPen(Gdiplus::Color(190, 155, 250, 255), 1.0f);
    graphics.DrawPath(&highlightPen, &fillPath);
    graphics.Restore(clipState);

    const HFONT statusFont = CreateFontW(
        -MulDiv(12, pixelHeight, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HGDIOBJ previousFont = SelectObject(dc, statusFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(130, 147, 170));
    const int statusY = barY + barHeight + MulDiv(18, pixelHeight, 96);
    RECT statusRect{
        centerX - 240, statusY, centerX + 240,
        statusY + MulDiv(24, pixelHeight, 96)};
    DrawTextW(
        dc, status ? status : L"正在启动 MasterTerm", -1, &statusRect,
        DT_CENTER | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, previousFont);
    DeleteObject(statusFont);
}

} // namespace

WebViewWindow::WebViewWindow()
    : m_backgroundBrush(CreateSolidBrush(RGB(17, 24, 39))),
      m_backend(std::make_unique<WebViewBackend>())
{
}

WebViewWindow::~WebViewWindow()
{
    shutdown();
    if (m_window && IsWindow(m_window))
        DestroyWindow(m_window);
    destroySplashCache();
    m_splashLogo.reset();
    if (m_gdiplusToken)
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
    if (m_backgroundBrush)
        DeleteObject(m_backgroundBrush);
}

bool WebViewWindow::ensureSplashCache(
    HDC dc, int width, int height, const RECT &client,
    bool showBar, const wchar_t *message)
{
    if (m_splashBackground && m_splashBackgroundDc
        && m_splashBackgroundWidth == width
        && m_splashBackgroundHeight == height
        && m_splashCacheShowsBar == showBar
        && m_splashCacheMessage == (message ? message : L""))
        return true;
    destroySplashCache();
    m_splashBackgroundDc = CreateCompatibleDC(dc);
    m_splashBackground = CreateCompatibleBitmap(dc, width, height);
    if (!m_splashBackgroundDc || !m_splashBackground) {
        destroySplashCache();
        return false;
    }
    SelectObject(m_splashBackgroundDc, m_splashBackground);
    drawSplashStatic(
        m_splashBackgroundDc, client, message, showBar, m_splashLogo.get());
    m_splashBackgroundWidth = width;
    m_splashBackgroundHeight = height;
    m_splashCacheShowsBar = showBar;
    m_splashCacheMessage = message ? message : L"";
    return true;
}

void WebViewWindow::destroySplashCache()
{
    if (m_splashBackgroundDc) {
        DeleteDC(m_splashBackgroundDc);
        m_splashBackgroundDc = nullptr;
    }
    if (m_splashBackground) {
        DeleteObject(m_splashBackground);
        m_splashBackground = nullptr;
    }
    m_splashBackgroundWidth = 0;
    m_splashBackgroundHeight = 0;
    m_splashCacheShowsBar = false;
    m_splashCacheMessage.clear();
}

bool WebViewWindow::registerWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    if (GetClassInfoExW(instance, WindowClassName, &windowClass))
        return true;

    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WebViewWindow::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(
        instance, MAKEINTRESOURCEW(MasterTermIconResourceId));
    if (!windowClass.hIcon)
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = m_backgroundBrush;
    windowClass.lpszClassName = WindowClassName;
    return RegisterClassExW(&windowClass) != 0;
}

WebViewWindow::RdpHostedSession *WebViewWindow::findRdpSession(
    const std::string &sessionId)
{
    const auto found = m_rdpSessions.find(sessionId);
    return found == m_rdpSessions.end() ? nullptr : found->second.get();
}

bool WebViewWindow::create(HINSTANCE instance, int showCommand)
{
    DiagnosticLog::setPhase("window-create");
    if (!m_gdiplusToken) {
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(
                &m_gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
            m_gdiplusToken = 0;
    }
    m_splashLogo = loadSplashLogo(instance);
    if (!registerWindowClass(instance))
        return false;

    const UINT dpi = GetDpiForSystem();
    RECT bounds{0, 0, scaleForDpi(1280, dpi), scaleForDpi(800, dpi)};
    AdjustWindowRectExForDpi(
        &bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

    m_window = CreateWindowExW(
        0, WindowClassName, L"MasterTerm", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);
    if (!m_window)
        return false;

    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    if (loadSavedWindowPlacement(savedPlacement)) {
        SetWindowPlacement(m_window, &savedPlacement);
        showCommand = savedPlacement.showCmd == SW_SHOWMAXIMIZED
            ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }

    const HICON applicationIcon = LoadIconW(
        instance, MAKEINTRESOURCEW(MasterTermIconResourceId));
    if (applicationIcon) {
        SendMessageW(
            m_window, WM_SETICON, ICON_BIG,
            reinterpret_cast<LPARAM>(applicationIcon));
        SendMessageW(
            m_window, WM_SETICON, ICON_SMALL,
            reinterpret_cast<LPARAM>(applicationIcon));
    }
    addTrayIcon();

    m_host = std::make_unique<WebViewHost>(m_window, m_backend.get());
    m_backend->setUiReadyHandler([this] {
        if (m_host) {
            m_startupReady = true;
            m_startupReadyTicks = m_startupTicks;
            DiagnosticLog::setPhase("webview-ready");
            DiagnosticLog::write("webview-ready");
            InvalidateRect(m_window, nullptr, FALSE);
        }
    });
    m_backend->setNotifyHandler([this](
        const std::string &title, const std::string &message) {
        showTrayNotification(utf8ToWide(title), utf8ToWide(message));
    });
    m_backend->setThemeHandler([this](const std::string &theme) {
        setApplicationTheme(theme);
    });
    m_backend->setThemeMenuHandler([this](
        int x, int y,
        const std::string &currentTheme,
        const std::string &currentPreset) {
        showThemeMenu(x, y, currentTheme, currentPreset);
    });
    m_backend->setToolsMenuHandler([this](int x, int y, bool broadcastActive) {
        showToolsMenu(x, y, broadcastActive);
    });
    m_backend->setHelpMenuHandler([this](int x, int y) {
        showHelpMenu(x, y);
    });
    m_backend->setRdpKeyMenuHandler([this](int x, int y) {
        showRdpKeyMenu(x, y);
    });
    m_backend->setServerContextMenuHandler(
        [this](int profileIndex, int x, int y,
               bool canMoveUp, bool canMoveDown) {
            showServerContextMenu(
                profileIndex, x, y, canMoveUp, canMoveDown);
        });
    m_backend->setExistingConnectionsMenuHandler(
        [this](int x, int y,
               const std::vector<std::pair<int, std::string>> &items) {
            showExistingConnectionsMenu(x, y, items);
        });
    m_backend->setCloseDecisionHandler([this](const std::string &decision) {
        m_closeDialogPending = false;
        if (decision == "exit") {
            m_backend->setCloseBehavior("exit");
            m_exitRequested = true;
            PostMessageW(m_window, WM_CLOSE, 0, 0);
            return;
        }
        m_backend->setCloseBehavior("tray");
        hideToTray();
    });
    m_backend->setRdpHandlers(
        [this](const std::string &sessionId, int profileIndex,
               const std::string &host, int port,
               const std::string &user, const std::string &password,
               const RdpConnectionOptions &options) {
            openRdpSession(sessionId, profileIndex, utf8ToWide(host), port,
                           utf8ToWide(user), utf8ToWide(password), options);
        },
        [this](const std::string &sessionId) { closeRdpSession(sessionId); });
    m_backend->setRdpLayoutHandler(
        [this](const std::string &sessionId, bool visible, bool refresh,
               bool clearOcclusion, bool reflowAll) {
        if (reflowAll)
            reflowAllRdpSessions(clearOcclusion);
        else
            layoutRdpSession(sessionId, visible, refresh, clearOcclusion);
        });
    m_backend->setRdpAdvancedEditorTransitionHandler(
        [this](const std::string &sessionId, bool expanded,
               std::function<void(bool)> completion) {
            transitionRdpAdvancedEditor(
                sessionId, expanded, std::move(completion));
        });
    m_backend->setRdpContextMenuHandler(
        [this](const std::string &sessionId, int x, int y) {
            showRdpContextMenu(sessionId, x, y);
        });
    m_backend->setRdpTabsContextMenuHandler(
        [this](int x, int y, bool canCloseSplit) {
            showRdpTabsContextMenu(x, y, canCloseSplit);
        });
    m_backend->setRdpFullscreenHandler(
        [this](const std::string &sessionId, bool enabled) {
            setRdpFullscreen(sessionId, enabled);
        });
    m_backend->setRdpFullscreenActionHandler(
        [this](const std::string &sessionId, const std::string &action) {
        RdpHostedSession *hosted = findRdpSession(sessionId);
        RdpSession *session = hosted ? hosted->session.get() : nullptr;
        if (action == "display-fit") {
            if (session)
                session->setSmartSizing(true);
            layoutRdpSession(sessionId, true, true);
        } else if (action == "display-refresh") {
            layoutRdpSession(sessionId, true, true);
        } else if (action == "display-original") {
            if (session)
                session->setSmartSizing(false);
            layoutRdpSession(sessionId, true, true);
        } else if (action == "reconnect" || action == "close") {
            m_backend->notifyRdpContextAction(sessionId, action);
        } else if (action == "exit-fullscreen") {
            setRdpFullscreen(sessionId, false);
        } else if ((action == "keyboard-ctrl-alt-del" || action == "tool-cad" || action == "cad") && session) {
            session->sendCtrlAltDelete();
        } else if ((action == "keyboard-ctrl-alt-end" || action == "tool-cae" || action == "cae") && session) {
            session->sendCtrlAltEnd();
        } else if ((action == "keyboard-windows" || action == "tool-win" || action == "win") && session) {
            session->sendWindowsKey();
        } else if ((action == "keyboard-print-screen" || action == "tool-print-screen" || action == "print-screen") && session) {
            session->sendPrintScreen();
        } else if ((action == "tool-task-manager" || action == "task-manager") && session) {
            session->launchRemoteUtility(L"taskmgr.exe");
        } else if ((action == "tool-control-panel" || action == "control-panel") && session) {
            session->launchRemoteUtility(L"control.exe");
        } else if ((action == "tool-registry-editor" || action == "registry-editor") && session) {
            session->launchRemoteUtility(L"regedit.exe");
        } else if ((action == "tool-windows-settings" || action == "windows-settings") && session) {
            session->launchRemoteUtility(L"ms-settings:");
        } else if ((action == "tool-services" || action == "services") && session) {
            session->launchRemoteUtility(L"services.msc");
        } else if ((action == "tool-device-manager" || action == "device-manager") && session) {
            session->launchRemoteUtility(L"devmgmt.msc");
        } else if ((action == "tool-run" || action == "run") && session) {
            session->launchRemoteUtility(L"");
        } else if ((action == "tool-explorer" || action == "explorer") && session) {
            session->launchRemoteUtility(L"explorer.exe");
        } else if ((action == "tool-cmd" || action == "cmd") && session) {
            session->launchRemoteUtility(L"cmd.exe");
        }
    });
    SetTimer(m_window, BackendPollTimer, 10, nullptr);
    ShowWindow(m_window, showCommand);
    UpdateWindow(m_window);
    // Paint the native splash before WebView2 initialization starts. This
    // removes the blank interval after launching while retaining the existing
    // hidden-WebView2/no-white-flash transition.
    DiagnosticLog::setPhase("webview-prewarm");
    WebViewHost::prewarm();
    m_host->initialize();
    DiagnosticLog::write("window-created");
    return true;
}

void WebViewWindow::saveWindowPlacement()
{
    if (!m_window || m_rdpFullscreenTransition)
        return;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (m_rdpFullscreen
        && m_rdpNormalPlacement.length == sizeof(WINDOWPLACEMENT)) {
        // RDP fullscreen temporarily changes the main window to a monitor
        // sized borderless window. Persist the layout from before entering
        // that mode instead of turning fullscreen into the next startup's
        // window placement.
        placement = m_rdpNormalPlacement;
    } else if (!GetWindowPlacement(m_window, &placement)) {
        return;
    }
    placement.length = sizeof(WINDOWPLACEMENT);
    if (placement.showCmd == SW_SHOWMINIMIZED)
        return;
    if (placement.showCmd != SW_SHOWMAXIMIZED)
        placement.showCmd = SW_SHOWNORMAL;
    writeSavedWindowPlacement(placement);
}

void WebViewWindow::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    DiagnosticLog::setPhase("shutdown");
    DiagnosticLog::write("shutdown-begin");
    saveWindowPlacement();
    if (m_window) {
        KillTimer(m_window, BackendPollTimer);
        KillTimer(m_window, ResizeSettledTimer);
        KillTimer(m_window, RdpFullscreenOverlayTimer);
        KillTimer(m_window, RdpFullscreenValidationTimer);
    }
    // Timeout fallback: if the graceful teardown below exceeds the deadline
    // (a hung WebView2 teardown or a session thread that never joins), the
    // watchdog terminates leftover SFTP workers and the process itself so
    // the app cannot hang at exit.  The abnormal-exit marker left behind
    // makes the next startup clean up orphaned WebView2 processes.
    m_shutdownDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_shutdownWatchdog = std::thread([doneEvent = m_shutdownDoneEvent] {
        DiagnosticLog::registerThread("shutdown-watchdog");
        if (WaitForSingleObject(doneEvent, ShutdownTimeoutMs) == WAIT_TIMEOUT) {
            DiagnosticLog::writeCrashLine(
                "watchdog-fired", "graceful teardown exceeded deadline");
            terminateSftpWorkerProcesses();
            TerminateProcess(GetCurrentProcess(), 1);
        }
    });
    // Order: backend enters its shutdown state first so no new SSH/serial/
    // ConPTY/SFTP work can start while WebView2 is being torn down; then the
    // WebView2 host stops the frontend; then the backend releases sessions
    // (SSH -> serial -> ConPTY) and terminates SFTP worker processes.
    if (m_backend) {
        DiagnosticLog::setPhase("shutdown-backend");
        m_backend->beginShutdown();
    }
    if (m_rdpFullscreen)
        setRdpFullscreen(m_rdpFullscreenSessionId, false);
    closeNativePopupMenu();
    if (m_rdpFullscreenOverlay) {
        DestroyWindow(m_rdpFullscreenOverlay);
        m_rdpFullscreenOverlay = nullptr;
    }
    if (m_rdpFullscreenOverlayPopup) {
        DestroyWindow(m_rdpFullscreenOverlayPopup);
        m_rdpFullscreenOverlayPopup = nullptr;
    }
    DiagnosticLog::setPhase("shutdown-rdp");
    m_rdpFailedCleanupTokens.clear();
    for (auto &entry : m_rdpSessions) {
        if (!entry.second || !entry.second->session)
            continue;
        entry.second->session->setHostedVisible(false);
        entry.second->session->disconnect();
        DiagnosticLog::write("rdp-close", entry.first);
    }
    m_rdpSessions.clear();
    m_activeRdpSessionId.clear();
    m_presentedRdpSessionId.clear();
    DiagnosticLog::setPhase("shutdown-webview");
    if (m_host)
        m_host->shutdown();
    m_host.reset();
    DiagnosticLog::setPhase("shutdown-sessions");
    m_backend.reset();
    removeTrayIcon();
    DiagnosticLog::write("shutdown-complete");
    SetEvent(m_shutdownDoneEvent);
    if (m_shutdownWatchdog.joinable())
        m_shutdownWatchdog.join();
    m_shutdownWatchdog = {};
    if (m_shutdownDoneEvent) {
        CloseHandle(m_shutdownDoneEvent);
        m_shutdownDoneEvent = nullptr;
    }
}

void WebViewWindow::addTrayIcon()
{
    if (!m_trayMessage)
        m_trayMessage = RegisterWindowMessageW(L"MasterTerm.TrayIcon");
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = TrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = m_trayMessage;
    data.hIcon = LoadIconW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(MasterTermIconResourceId));
    if (!data.hIcon)
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"MasterTerm");
    if (!Shell_NotifyIconW(NIM_ADD, &data))
        return;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void WebViewWindow::removeTrayIcon()
{
    if (!m_window)
        return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = TrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void WebViewWindow::hideToTray()
{
    if (!m_window || m_trayHidden)
        return;
    m_trayRestorePlacement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(m_window, &m_trayRestorePlacement)) {
        m_trayRestorePlacement.length = sizeof(WINDOWPLACEMENT);
        m_trayRestorePlacement.flags = 0;
        m_trayRestorePlacement.showCmd = SW_SHOWNORMAL;
        GetWindowRect(m_window, &m_trayRestorePlacement.rcNormalPosition);
    }
    if (m_trayRestorePlacement.showCmd == SW_SHOWMINIMIZED)
        m_trayRestorePlacement.showCmd = SW_SHOWNORMAL;
    if (m_trayRestorePlacement.showCmd != SW_SHOWMAXIMIZED
        && m_trayRestorePlacement.showCmd != SW_SHOWNORMAL)
        m_trayRestorePlacement.showCmd = SW_SHOWNORMAL;
    m_trayHidden = true;
    ShowWindow(m_window, SW_HIDE);
}

void WebViewWindow::restoreFromTray()
{
    if (!m_window)
        return;
    // A single tray click can produce both a mouse event and NIN_SELECT.
    // Ignore the duplicate after the first restore so a second event cannot
    // apply a different show state while Windows is still activating us.
    if (!m_trayHidden && IsWindowVisible(m_window)) {
        SetForegroundWindow(m_window);
        SetActiveWindow(m_window);
        return;
    }

    WINDOWPLACEMENT placement = m_trayRestorePlacement;
    placement.length = sizeof(WINDOWPLACEMENT);
    if (placement.showCmd != SW_SHOWMAXIMIZED)
        placement.showCmd = SW_SHOWNORMAL;
    const int showCommand = placement.showCmd == SW_SHOWMAXIMIZED
        ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    // SetWindowPlacement restores the saved normal rectangle, while the
    // explicit ShowWindow call is required to reliably make a hidden window
    // visible in the same maximized state on all Windows builds.
    SetWindowPlacement(m_window, &placement);
    ShowWindow(m_window, showCommand);
    m_trayHidden = false;
    SetForegroundWindow(m_window);
    SetActiveWindow(m_window);
}

void WebViewWindow::showTrayNotification(
    const std::wstring &title, const std::wstring &message)
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = TrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(
        data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void WebViewWindow::showTrayMenu(const POINT &cursor)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, TrayCommandRestore, L"显示主窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayCommandExit, L"退出");
    SetForegroundWindow(m_window);
    const UINT command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, m_window, nullptr));
    DestroyMenu(menu);
    if (command == TrayCommandRestore)
        restoreFromTray();
    else if (command == TrayCommandExit)
        requestExit();
}

bool WebViewWindow::ensureNativePopupMenuClass()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    if (GetClassInfoExW(
            instance, NativePopupMenuClassName, &windowClass))
        return true;
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    windowClass.lpfnWndProc = &WebViewWindow::nativePopupMenuProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = NativePopupMenuClassName;
    return RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int WebViewWindow::nativePopupMenuHeight(UINT dpi) const
{
    int height = scaleForDpi(10, dpi);
    for (const auto &item : m_nativePopupMenuItems)
        height += scaleForDpi(item.separator ? 9 : 30, dpi);
    return height;
}

int WebViewWindow::nativePopupMenuItemAt(POINT point) const
{
    if (!m_window || !m_nativePopupMenuWindow)
        return -1;
    const UINT dpi = std::max<UINT>(
        96, GetDpiForWindow(m_nativePopupMenuWindow));
    const int width = scaleForDpi(150, dpi);
    const int itemPadding = scaleForDpi(5, dpi);
    const int itemWidth = width - itemPadding;
    int top = itemPadding;
    for (size_t index = 0; index < m_nativePopupMenuItems.size(); ++index) {
        const auto &item = m_nativePopupMenuItems[index];
        if (item.separator) {
            top += scaleForDpi(9, dpi);
            continue;
        }
        RECT itemRect{
            itemPadding, top, itemWidth,
            top + scaleForDpi(30, dpi)};
        if (PtInRect(&itemRect, point))
            return static_cast<int>(index);
        top += scaleForDpi(30, dpi);
    }
    return -1;
}

void WebViewWindow::paintNativePopupMenu(HDC dc) const
{
    if (!dc || !m_nativePopupMenuWindow)
        return;
    const UINT dpi = std::max<UINT>(
        96, GetDpiForWindow(m_nativePopupMenuWindow));
    RECT client{};
    GetClientRect(m_nativePopupMenuWindow, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const NativePopupMenuPalette palette = nativePopupMenuPalette(
        m_rdpFullscreenOverlayTheme);

    HBRUSH background = CreateSolidBrush(palette.background);
    HPEN border = CreatePen(PS_SOLID, scaleForDpi(1, dpi), palette.border);
    HGDIOBJ oldBrush = SelectObject(dc, background);
    HGDIOBJ oldPen = SelectObject(dc, border);
    RoundRect(
        dc, 0, 0, width, height,
        scaleForDpi(6, dpi), scaleForDpi(6, dpi));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);
    DeleteObject(background);

    HFONT font = CreateFontW(
        -scaleForDpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const int itemPadding = scaleForDpi(5, dpi);
    const int itemWidth = width - itemPadding;
    int top = itemPadding;
    for (size_t index = 0; index < m_nativePopupMenuItems.size(); ++index) {
        const auto &item = m_nativePopupMenuItems[index];
        if (item.separator) {
            HPEN separator = CreatePen(
                PS_SOLID, scaleForDpi(1, dpi), palette.separator);
            HGDIOBJ oldSeparator = SelectObject(dc, separator);
            const int lineY = top + scaleForDpi(4, dpi);
            MoveToEx(dc, scaleForDpi(8, dpi), lineY, nullptr);
            LineTo(dc, width - scaleForDpi(8, dpi), lineY);
            SelectObject(dc, oldSeparator);
            DeleteObject(separator);
            top += scaleForDpi(9, dpi);
            continue;
        }
        RECT itemRect{
            itemPadding, top, itemWidth,
            top + scaleForDpi(30, dpi)};
        if (m_nativePopupMenuHover == static_cast<int>(index) && !item.disabled) {
            HBRUSH hover = CreateSolidBrush(palette.hover);
            HPEN hoverPen = CreatePen(PS_SOLID, 0, palette.hover);
            HGDIOBJ oldHoverBrush = SelectObject(dc, hover);
            HGDIOBJ oldHoverPen = SelectObject(dc, hoverPen);
            RoundRect(
                dc, itemRect.left, itemRect.top, itemRect.right,
                itemRect.bottom, scaleForDpi(4, dpi), scaleForDpi(4, dpi));
            SelectObject(dc, oldHoverPen);
            SelectObject(dc, oldHoverBrush);
            DeleteObject(hoverPen);
            DeleteObject(hover);
        }
        SetTextColor(
            dc,
            item.disabled ? palette.disabled
                          : m_nativePopupMenuHover == static_cast<int>(index)
                              ? palette.hoverText : palette.text);
        RECT textRect = itemRect;
        textRect.left += scaleForDpi(11, dpi);
        textRect.right -= scaleForDpi(8, dpi);
        DrawTextW(
            dc, item.label.c_str(), -1, &textRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        top += scaleForDpi(30, dpi);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void WebViewWindow::closeNativePopupMenu()
{
    if (m_nativePopupMenuWindow) {
        const HWND window = m_nativePopupMenuWindow;
        m_nativePopupMenuClosing = true;
        DestroyWindow(window);
        if (m_nativePopupMenuWindow == window)
            m_nativePopupMenuWindow = nullptr;
        m_nativePopupMenuClosing = false;
    }
    m_nativePopupMenuHover = -1;
}

UINT WebViewWindow::showNativePopupMenu(
    int screenX, int screenY,
    std::vector<NativePopupMenuItem> items, int width)
{
    if (!m_window || items.empty())
        return 0;
    closeNativePopupMenu();
    m_nativePopupMenuItems = std::move(items);
    m_nativePopupMenuCommand = 0;
    m_nativePopupMenuHover = -1;
    m_nativePopupMenuWidth = std::max(150, width);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        m_nativePopupMenuItems.clear();
        m_nativePopupMenuWidth = 150;
        return 0;
    }
    for (const auto &item : m_nativePopupMenuItems) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        appendThemedMenuItem(
            menu, item.command, item.label.c_str(), item.disabled);
    }
    HBRUSH background = nullptr;
    setThemedMenuBackground(
        menu, nativePopupMenuPalette(m_rdpFullscreenOverlayTheme).background,
        background);
    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_NOANIMATION,
        screenX, screenY, m_window, nullptr);
    DestroyMenu(menu);
    if (background)
        DeleteObject(background);
    m_nativePopupMenuItems.clear();
    m_nativePopupMenuWidth = 150;
    return command;
}

LRESULT CALLBACK WebViewWindow::nativePopupMenuProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *self = reinterpret_cast<WebViewWindow *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = static_cast<WebViewWindow *>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE
            && !self->m_nativePopupMenuClosing)
            self->closeNativePopupMenu();
        return 0;
    case WM_KILLFOCUS:
        if (!self->m_nativePopupMenuClosing)
            self->closeNativePopupMenu();
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        const int hover = self->nativePopupMenuItemAt(point);
        if (hover != self->m_nativePopupMenuHover) {
            self->m_nativePopupMenuHover = hover;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{
            sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->m_nativePopupMenuHover != -1) {
            self->m_nativePopupMenuHover = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        const int index = self->nativePopupMenuItemAt(point);
        if (index >= 0
            && index < static_cast<int>(self->m_nativePopupMenuItems.size())) {
            const auto &item = self->m_nativePopupMenuItems[index];
            if (!item.separator && !item.disabled) {
                self->m_nativePopupMenuCommand = item.command;
                self->closeNativePopupMenu();
            }
        }
        return 0;
    }
    case WM_RBUTTONUP:
        self->closeNativePopupMenu();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            self->closeNativePopupMenu();
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        self->paintNativePopupMenu(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_NCDESTROY:
        if (self->m_nativePopupMenuWindow == window)
            self->m_nativePopupMenuWindow = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void WebViewWindow::showThemeMenu(
    int x, int y,
    const std::string &currentTheme,
    const std::string &currentPreset)
{
    if (!m_window || !m_host)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);

    closeNativePopupMenu();
    m_nativePopupMenuWidth = 200;

    HMENU rootMenu = CreatePopupMenu();
    if (!rootMenu) {
        m_nativePopupMenuWidth = 150;
        return;
    }

    HMENU darkMenu = CreatePopupMenu();
    HMENU lightMenu = CreatePopupMenu();
    HMENU blueMenu = CreatePopupMenu();

    const NativePopupMenuPalette palette = nativePopupMenuPalette(
        m_rdpFullscreenOverlayTheme);

    HBRUSH rootBrush = nullptr;
    setThemedMenuBackground(rootMenu, palette.background, rootBrush);
    HBRUSH darkBrush = nullptr;
    setThemedMenuBackground(darkMenu, palette.background, darkBrush);
    HBRUSH lightBrush = nullptr;
    setThemedMenuBackground(lightMenu, palette.background, lightBrush);
    HBRUSH blueBrush = nullptr;
    setThemedMenuBackground(blueMenu, palette.background, blueBrush);

    const std::string activeTheme = currentTheme.empty() ? "dark" : currentTheme;
    const std::string activePreset = currentPreset.empty() ? "custom" : currentPreset;

    struct PresetItem {
        UINT command;
        const wchar_t *label;
        const char *theme;
        const char *preset;
    };

    static const PresetItem darkPresets[] = {
        {ThemePresetDarkCustom, L"默认深色", "dark", "custom"},
        {ThemePresetDarkOneDark, L"One Dark", "dark", "one-dark"},
        {ThemePresetDarkDracula, L"Dracula", "dark", "dracula"},
        {ThemePresetDarkNord, L"Nord", "dark", "nord"},
        {ThemePresetDarkMonokai, L"Monokai Pro", "dark", "monokai"},
        {ThemePresetDarkSolarizedDark, L"Solarized Dark", "dark", "solarized-dark"},
        {ThemePresetDarkCatppuccinMocha, L"Catppuccin Mocha", "dark", "catppuccin-mocha"},
        {ThemePresetDarkGithubDark, L"GitHub Dark", "dark", "github-dark"},
    };

    static const PresetItem lightPresets[] = {
        {ThemePresetLightCustom, L"默认浅色", "light", "custom"},
        {ThemePresetLightGithubLight, L"GitHub Light", "light", "github-light"},
        {ThemePresetLightOneLight, L"One Light", "light", "one-light"},
        {ThemePresetLightSolarizedLight, L"Solarized Light", "light", "solarized-light"},
    };

    static const PresetItem bluePresets[] = {
        {ThemePresetBlueCustom, L"经典海蓝", "blue", "custom"},
    };

    auto populateSubMenu = [&](HMENU subMenu, const PresetItem *items, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            UINT flags = MF_OWNERDRAW;
            if (activeTheme == items[i].theme && activePreset == items[i].preset)
                flags |= MF_CHECKED;
            AppendMenuW(
                subMenu, flags, items[i].command,
                reinterpret_cast<LPCWSTR>(items[i].label));
        }
    };

    populateSubMenu(darkMenu, darkPresets, sizeof(darkPresets) / sizeof(darkPresets[0]));
    populateSubMenu(lightMenu, lightPresets, sizeof(lightPresets) / sizeof(lightPresets[0]));
    populateSubMenu(blueMenu, bluePresets, sizeof(bluePresets) / sizeof(bluePresets[0]));

    static const wchar_t darkCategoryLabel[] = L"🌙 深色主题\t▶";
    static const wchar_t lightCategoryLabel[] = L"☀️ 浅色主题\t▶";
    static const wchar_t blueCategoryLabel[] = L"🌊 蓝色主题\t▶";

    AppendMenuW(
        rootMenu, MF_POPUP | MF_OWNERDRAW,
        reinterpret_cast<UINT_PTR>(darkMenu),
        reinterpret_cast<LPCWSTR>(darkCategoryLabel));
    AppendMenuW(
        rootMenu, MF_POPUP | MF_OWNERDRAW,
        reinterpret_cast<UINT_PTR>(lightMenu),
        reinterpret_cast<LPCWSTR>(lightCategoryLabel));
    AppendMenuW(
        rootMenu, MF_POPUP | MF_OWNERDRAW,
        reinterpret_cast<UINT_PTR>(blueMenu),
        reinterpret_cast<LPCWSTR>(blueCategoryLabel));

    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        rootMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_NOANIMATION,
        point.x, point.y, m_window, nullptr);

    DestroyMenu(rootMenu);

    if (rootBrush) DeleteObject(rootBrush);
    if (darkBrush) DeleteObject(darkBrush);
    if (lightBrush) DeleteObject(lightBrush);
    if (blueBrush) DeleteObject(blueBrush);

    m_nativePopupMenuWidth = 150;

    if (!command)
        return;

    const char *selectedTheme = nullptr;
    const char *selectedPreset = nullptr;

    for (const auto &item : darkPresets) {
        if (item.command == command) {
            selectedTheme = item.theme;
            selectedPreset = item.preset;
            break;
        }
    }
    if (!selectedTheme) {
        for (const auto &item : lightPresets) {
            if (item.command == command) {
                selectedTheme = item.theme;
                selectedPreset = item.preset;
                break;
            }
        }
    }
    if (!selectedTheme) {
        for (const auto &item : bluePresets) {
            if (item.command == command) {
                selectedTheme = item.theme;
                selectedPreset = item.preset;
                break;
            }
        }
    }
    if (!selectedTheme) {
        if (command == ThemeMenuDark) {
            selectedTheme = "dark";
            selectedPreset = "custom";
        } else if (command == ThemeMenuLight) {
            selectedTheme = "light";
            selectedPreset = "custom";
        } else if (command == ThemeMenuBlue) {
            selectedTheme = "blue";
            selectedPreset = "custom";
        }
    }

    if (!selectedTheme || !selectedPreset)
        return;

    setApplicationTheme(selectedTheme);
    m_host->sendJsonToWebView(
        L"{\"event\":\"app.nativeThemeSelected\",\"payload\":{\"theme\":\""
        + utf8ToWide(selectedTheme)
        + L"\",\"preset\":\""
        + utf8ToWide(selectedPreset)
        + L"\"}}");
}

void WebViewWindow::showRdpKeyMenu(int x, int y)
{
    if (!m_window || !m_host)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);

    closeNativePopupMenu();
    m_nativePopupMenuWidth = 240;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        m_nativePopupMenuWidth = 150;
        return;
    }

    const NativePopupMenuPalette palette = nativePopupMenuPalette(
        m_rdpFullscreenOverlayTheme);

    HBRUSH bgBrush = nullptr;
    setThemedMenuBackground(menu, palette.background, bgBrush);

    appendThemedMenuItem(menu, RdpKeyCmdCad, L"Ctrl + Alt + Del\tCAD");
    appendThemedMenuItem(menu, RdpKeyCmdWin, L"Windows 徽标键\tWin");
    appendThemedMenuItem(menu, RdpKeyCmdRun, L"运行 (Win + R)\tRun");
    appendThemedMenuItem(menu, RdpKeyCmdTaskMgr, L"任务管理器\tTaskMgr");
    appendThemedMenuItem(menu, RdpKeyCmdExplorer, L"资源管理器\tExplorer");
    appendThemedMenuItem(menu, RdpKeyCmdCmd, L"命令提示符 (Cmd)\tCmd");
    appendThemedMenuItem(menu, RdpKeyCmdPrtScn, L"截取屏幕 (PrintScreen)\tPrtScn");

    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_NOANIMATION,
        point.x, point.y, m_window, nullptr);

    DestroyMenu(menu);
    if (bgBrush) DeleteObject(bgBrush);
    m_nativePopupMenuWidth = 150;

    if (!command)
        return;

    RdpHostedSession *hosted = findRdpSession(m_activeRdpSessionId);
    RdpSession *session = hosted ? hosted->session.get() : nullptr;
    if (!session)
        return;

    switch (command) {
    case RdpKeyCmdCad:
        session->sendCtrlAltDelete();
        break;
    case RdpKeyCmdWin:
        session->sendWindowsKey();
        break;
    case RdpKeyCmdRun:
        session->launchRemoteUtility(L"");
        break;
    case RdpKeyCmdTaskMgr:
        session->launchRemoteUtility(L"taskmgr.exe");
        break;
    case RdpKeyCmdExplorer:
        session->launchRemoteUtility(L"explorer.exe");
        break;
    case RdpKeyCmdCmd:
        session->launchRemoteUtility(L"cmd.exe");
        break;
    case RdpKeyCmdPrtScn:
        session->sendPrintScreen();
        break;
    default:
        break;
    }
}

void WebViewWindow::showToolsMenu(int x, int y, bool broadcastActive)
{
    if (!m_window || !m_host)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);

    closeNativePopupMenu();
    m_nativePopupMenuWidth = 220;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        m_nativePopupMenuWidth = 150;
        return;
    }

    const NativePopupMenuPalette palette = nativePopupMenuPalette(
        m_rdpFullscreenOverlayTheme);

    HBRUSH bgBrush = nullptr;
    setThemedMenuBackground(menu, palette.background, bgBrush);

    appendThemedMenuItem(
        menu, ToolsCmdBroadcast,
        broadcastActive ? L"📻 广播输入 (已开启)\tAlt+B" : L"📻 广播输入\tAlt+B");
    if (broadcastActive) {
        CheckMenuItem(menu, ToolsCmdBroadcast, MF_BYCOMMAND | MF_CHECKED);
    }
    appendThemedMenuItem(menu, ToolsCmdTunnel, L"🔀 端口转发");
    appendThemedMenuItem(menu, ToolsCmdBatchCmd, L"⚡ 批量执行…");
    appendThemedMenuItem(menu, ToolsCmdCloudSync, L"☁️ 云同步");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendThemedMenuItem(menu, ToolsCmdDiagnostics, L"🩺 诊断日志");

    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_NOANIMATION,
        point.x, point.y, m_window, nullptr);

    DestroyMenu(menu);
    if (bgBrush) DeleteObject(bgBrush);
    m_nativePopupMenuWidth = 150;

    if (!command)
        return;

    const wchar_t *action = nullptr;
    switch (command) {
    case ToolsCmdBroadcast: action = L"broadcast"; break;
    case ToolsCmdTunnel: action = L"tunnel"; break;
    case ToolsCmdBatchCmd: action = L"batch-cmd"; break;
    case ToolsCmdCloudSync: action = L"cloud-sync"; break;
    case ToolsCmdDiagnostics: action = L"diagnostics"; break;
    default: break;
    }

    if (action) {
        m_host->sendJsonToWebView(
            std::wstring(L"{\"event\":\"app.nativeToolsAction\",\"payload\":{\"action\":\"")
            + action + L"\"}}");
    }
}

void WebViewWindow::showHelpMenu(int x, int y)
{
    if (!m_window || !m_host)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);

    closeNativePopupMenu();
    m_nativePopupMenuWidth = 200;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        m_nativePopupMenuWidth = 150;
        return;
    }

    const NativePopupMenuPalette palette = nativePopupMenuPalette(
        m_rdpFullscreenOverlayTheme);

    HBRUSH bgBrush = nullptr;
    setThemedMenuBackground(menu, palette.background, bgBrush);

    appendThemedMenuItem(menu, HelpCmdShortcuts, L"⌨️ 快捷键速查\tCtrl+/");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendThemedMenuItem(menu, HelpCmdCheckUpdates, L"🔄 检查更新…");
    appendThemedMenuItem(menu, HelpCmdAbout, L"ℹ️ 关于 MasterTerm");

    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_NOANIMATION,
        point.x, point.y, m_window, nullptr);

    DestroyMenu(menu);
    if (bgBrush) DeleteObject(bgBrush);
    m_nativePopupMenuWidth = 150;

    if (!command)
        return;

    const wchar_t *action = nullptr;
    switch (command) {
    case HelpCmdShortcuts: action = L"shortcuts"; break;
    case HelpCmdCheckUpdates: action = L"check-updates"; break;
    case HelpCmdAbout: action = L"about"; break;
    default: break;
    }

    if (action) {
        m_host->sendJsonToWebView(
            std::wstring(L"{\"event\":\"app.nativeHelpAction\",\"payload\":{\"action\":\"")
            + action + L"\"}}");
    }
}

void WebViewWindow::requestExit()
{
    m_exitRequested = true;
    PostMessageW(m_window, WM_CLOSE, 0, 0);
}

int WebViewWindow::showCloseDialog()
{
    // TaskDialogIndirect lives in comctl32.dll and is not available on every
    // system (its import would otherwise make the app fail to load with a
    // missing-ordinal error).  Load it at runtime and fall back to a plain
    // message box when the system comctl32 is too old.
    using TaskDialogIndirectFn = HRESULT(WINAPI *)(
        const TASKDIALOGCONFIG *, int *, int *, BOOL *);
    const HMODULE commonControls = LoadLibraryW(L"comctl32.dll");
    const auto taskDialogIndirect = commonControls
        ? reinterpret_cast<TaskDialogIndirectFn>(
            GetProcAddress(commonControls, "TaskDialogIndirect"))
        : nullptr;
    if (!taskDialogIndirect) {
        const int answer = MessageBoxW(
            m_window,
            L"关闭主窗口时：\n\n是 = 最小化到系统托盘（后台继续运行）\n"
            L"否 = 直接退出\n\n所选行为将成为默认，可在“系统设置”中修改。",
            L"MasterTerm", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        return answer == IDNO ? CloseChoiceExit : CloseChoiceTray;
    }
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = m_window;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"MasterTerm";
    config.pszMainInstruction = L"关闭主窗口时";
    config.pszContent =
        L"最小化到系统托盘后，SSH / 串口 / 本地终端会话会继续在后台保持连接。"
        L"\n\n所选行为将成为默认，之后可在“系统设置”中修改。";
    const TASKDIALOG_BUTTON buttons[] = {
        {CloseChoiceTray, L"最小化到托盘"},
        {CloseChoiceExit, L"直接退出"},
    };
    config.cButtons = 2;
    config.pButtons = buttons;
    int clicked = 0;
    if (FAILED(taskDialogIndirect(&config, &clicked, nullptr, nullptr)))
        return 0;
    return clicked;
}

LRESULT CALLBACK WebViewWindow::windowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    WebViewWindow *self = reinterpret_cast<WebViewWindow *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = static_cast<WebViewWindow *>(create->lpCreateParams);
        self->m_window = window;
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self
        ? self->handleMessage(message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT WebViewWindow::handleMessage(
    UINT message, WPARAM wParam, LPARAM lParam)
{
    if (m_trayMessage && message == m_trayMessage) {
        // NOTIFYICON_VERSION_4 packs the event code into LOWORD(lParam),
        // the icon ID into HIWORD(lParam), and the mouse anchor coordinates
        // into wParam.  Fall back to GetCursorPos when coordinates are zero
        // (legacy version 0/3 behaviour or keyboard-generated events).
        const UINT event = static_cast<UINT>(LOWORD(lParam));
        if (event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP
            || event == NIN_SELECT
            || event == NIN_KEYSELECT) {
            restoreFromTray();
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            POINT cursor{static_cast<LONG>(LOWORD(wParam)),
                         static_cast<LONG>(HIWORD(wParam))};
            if (cursor.x == 0 && cursor.y == 0)
                GetCursorPos(&cursor);
            showTrayMenu(cursor);
        }
        return 0;
    }
    if (message == RdpNativeFullscreenChangedMessage) {
        const bool entered = wParam != 0;
        DiagnosticLog::write(
            "rdp-native-fullscreen-event",
            entered ? "entered" : "left");
        if (entered && m_rdpFullscreen && m_rdpFullscreenUseMultimon) {
            // mstscax owns the native presentation window. Remove the normal
            // MasterTerm frame from Alt+Tab/the taskbar and keep it behind
            // the presentation. It must remain alive and visible because it
            // is the OLE owner that receives fullscreen/keyboard events.
            LONG_PTR exStyle = GetWindowLongPtrW(m_window, GWL_EXSTYLE);
            exStyle = (exStyle | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
            SetWindowLongPtrW(m_window, GWL_EXSTYLE, exStyle);
            setTaskbarTabVisible(m_window, false);
            SetWindowPos(
                m_window, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                    | SWP_FRAMECHANGED);
            m_rdpMainWindowDemotedForNativeFullscreen = true;
            // On affected mstscax builds the native fullscreen host is born
            // at primary-monitor size even though the remote monitor layout
            // is already correct. Repair it immediately, then validate once
            // more after the control has settled.
            RdpHostedSession *hosted = findRdpSession(
                m_rdpFullscreenSessionId);
            if (hosted)
                hosted->session->validateNativeFullscreenPresentation();
            KillTimer(m_window, RdpFullscreenValidationTimer);
            SetTimer(
                m_window, RdpFullscreenValidationTimer, 600, nullptr);
            updateRdpFullscreenOverlay();
        } else if (!entered && m_rdpFullscreen
                   && m_rdpFullscreenUseMultimon
                   && !m_rdpFullscreenTransition) {
            // The native connection bar / Ctrl+Alt+Break can leave
            // fullscreen without going through the WebView command path.
            setRdpFullscreen(m_rdpFullscreenSessionId, false);
        }
        return 0;
    }
    if (message == RdpFailedCleanupMessage) {
        cleanupFailedRdpSessions();
        return 0;
    }
    switch (message) {
    case WM_MEASUREITEM: {
        const auto *measure = reinterpret_cast<const MEASUREITEMSTRUCT *>(lParam);
        if (!isMasterTermOwnerDrawMenuItem(measure))
            break;
        auto *mutableMeasure = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
        measureThemedMenuItem(
            *mutableMeasure,
            std::max<UINT>(96, GetDpiForWindow(m_window)),
            m_nativePopupMenuWidth);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
        if (!isMasterTermOwnerDrawMenuItem(draw))
            break;
        drawThemedMenuItem(
            *draw,
            nativePopupMenuPalette(m_rdpFullscreenOverlayTheme),
            std::max<UINT>(96, GetDpiForWindow(m_window)));
        return TRUE;
    }
    case WM_KEYDOWN:
        // Esc remains a keyboard shortcut for closing the embedded RDP tab;
        // the visible close action is the terminal tab's own close button.
        if (wParam == VK_ESCAPE && m_rdpFullscreen) {
            setRdpFullscreen(m_rdpFullscreenSessionId, false);
            return 0;
        }
        if (wParam == VK_ESCAPE && m_rdpModalDimmed) {
            dismissActiveModal();
            return 0;
        }
        if (wParam == VK_ESCAPE && !m_activeRdpSessionId.empty()) {
            if (m_backend)
                m_backend->notifyRdpContextAction(
                    m_activeRdpSessionId, "close");
            else
                closeRdpSession(m_activeRdpSessionId);
            return 0;
        }
        break;
    case WebViewHost::sendJsonMessage():
        // Delivers backend worker-thread JSON (cloud sync, update check)
        // to the WebView2 controller on the UI thread, where
        // PostWebMessageAsJson is legal.
        if (lParam) {
            auto *json = reinterpret_cast<std::wstring *>(lParam);
            if (m_host)
                m_host->sendJsonToWebView(*json);
            delete json;
        }
        return 0;
    case WM_ERASEBKGND:
        // WM_PAINT always covers the complete client area. Suppressing the
        // intermediate erase prevents a visible dark flash before the
        // off-screen splash frame is copied to the window.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(m_window, &paint);
        RECT client{};
        GetClientRect(m_window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (m_host && m_host->hasStartupError()) {
            if (ensureSplashCache(
                    dc, width, height, client, false,
                    m_host->startupError().c_str())
                && m_splashBackgroundDc)
                BitBlt(
                    dc, 0, 0, width, height,
                    m_splashBackgroundDc, 0, 0, SRCCOPY);
            else
                FillRect(dc, &client, m_backgroundBrush);
        } else if (showStartupSplash()) {
            const wchar_t *startupStatus = L"正在初始化 MasterTerm";
            if (m_host && m_host->isControllerReady()) {
                startupStatus = m_startupReady
                    ? L"正在准备主界面"
                    : L"正在加载主界面";
            } else if (m_startupTicks >= 20) {
                startupStatus = L"正在创建 WebView2 环境";
            }
            const bool cacheReady = ensureSplashCache(
                dc, width, height, client, true, L"正在启动…")
                && m_splashBackgroundDc;
            HDC frameDc = nullptr;
            HBITMAP frameBitmap = nullptr;
            HGDIOBJ previousBitmap = nullptr;
            if (cacheReady) {
                frameDc = CreateCompatibleDC(dc);
                frameBitmap = CreateCompatibleBitmap(dc, width, height);
                if (frameDc && frameBitmap) {
                    previousBitmap = SelectObject(frameDc, frameBitmap);
                    BitBlt(
                        frameDc, 0, 0, width, height,
                        m_splashBackgroundDc, 0, 0, SRCCOPY);
                    drawSplashProgress(
                        frameDc, client, m_startupTicks,
                        m_startupReady
                            && m_startupTicks >= m_startupReadyTicks + 5,
                        startupStatus);
                    BitBlt(dc, 0, 0, width, height, frameDc, 0, 0, SRCCOPY);
                    SelectObject(frameDc, previousBitmap);
                }
            }
            if (!frameDc || !frameBitmap) {
                FillRect(dc, &client, m_backgroundBrush);
                if (cacheReady)
                    BitBlt(
                        dc, 0, 0, width, height,
                        m_splashBackgroundDc, 0, 0, SRCCOPY);
                drawSplashProgress(
                    dc, client, m_startupTicks,
                    m_startupReady
                        && m_startupTicks >= m_startupReadyTicks + 5,
                    startupStatus);
            }
            if (frameDc)
                DeleteDC(frameDc);
            if (frameBitmap)
                DeleteObject(frameBitmap);
        } else {
            FillRect(dc, &client, m_backgroundBrush);
        }
        EndPaint(m_window, &paint);
        return 0;
    }
    case WM_SIZE:
        destroySplashCache();
        if (!m_rdpFullscreenTransition
            && !m_activeRdpSessionId.empty())
            layoutRdpSession(m_activeRdpSessionId, m_rdpHostedVisible, false);
        if (m_host) {
            m_host->resize();
            KillTimer(m_window, ResizeSettledTimer);
            SetTimer(m_window, ResizeSettledTimer, 120, nullptr);
        }
        if (wParam != SIZE_MINIMIZED)
            saveWindowPlacement();
        return 0;
    case WM_MOVE:
        saveWindowPlacement();
        return 0;
    case WM_EXITSIZEMOVE:
        saveWindowPlacement();
        return 0;
    case WM_TIMER:
        if (wParam == BackendPollTimer) {
            ++m_startupTicks;
            if (showStartupSplash()) {
                if (m_startupTicks % 4 == 0)
                    InvalidateRect(m_window, nullptr, FALSE);
                // Frontend rendered: complete the bar, then reveal the page.
                if (m_host && m_startupReady
                    && m_startupTicks - m_startupReadyTicks >= 5)
                    m_host->forceShow();
                // Safety net: reveal the WebView2 after ~5 s even if the
                // frontend-ready message is delayed, so the native splash
                // cannot remain on screen indefinitely.
                if (m_host && m_startupTicks >= 500)
                    m_host->forceShow();
            }
            if (m_backend)
                m_backend->poll();
            // Hidden exit self test (tools/connect-exit-stress.ps1): start
            // once the frontend is ready so the cycle always includes a full
            // WebView2 startup and teardown.  Fall back after ~10 s so a
            // stalled frontend cannot block the diagnostic run forever.
            if (m_exitSelfTestEnabled && !m_exitSelfTestStarted && m_backend
                && (m_startupReady || m_startupTicks >= 1000)) {
                m_exitSelfTestStarted = true;
                m_backend->startExitSelfTest([this](int exitCode) {
                    m_exitSelfTestDone = true;
                    m_exitSelfTestExitCode = exitCode;
                    requestExit();
                });
            }
            return 0;
        }
        if (wParam == ResizeSettledTimer) {
            KillTimer(m_window, ResizeSettledTimer);
            if (m_host)
                m_host->notifyFrontendResize();
            if (!m_activeRdpSessionId.empty())
                layoutRdpSession(
                    m_activeRdpSessionId, m_rdpHostedVisible, true);
            return 0;
        }
        if (wParam == RdpFullscreenOverlayTimer) {
            pollRdpFullscreenOverlay();
            return 0;
        }
        if (wParam == RdpFullscreenValidationTimer) {
            KillTimer(m_window, RdpFullscreenValidationTimer);
            if (!m_rdpFullscreen || !m_rdpFullscreenUseMultimon)
                return 0;
            RdpHostedSession *hosted = findRdpSession(
                m_rdpFullscreenSessionId);
            if (hosted
                && !hosted->session->validateNativeFullscreenPresentation()) {
                DiagnosticLog::write(
                    "rdp-native-fullscreen-validation",
                    "failed; leaving unusable multimon presentation");
                showTrayNotification(
                    L"RDP 多显示器全屏未正确呈现",
                    L"检测到滚动桌面或全屏窗口未覆盖全部显示器，已安全退出全屏。请将本次 RDP 日志发送给开发者。");
                const std::string sessionId = m_rdpFullscreenSessionId;
                setRdpFullscreen(sessionId, false);
            }
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        // A fullscreen RDP transition explicitly owns the top-level window
        // rectangle. Applying Windows' suggested DPI rectangle while the
        // borderless window spans monitors can turn the virtual desktop size
        // into the next persisted application size (for example 4480 px).
        if (m_rdpFullscreen || m_rdpFullscreenTransition)
            return 0;
        const RECT *suggested = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(
            m_window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *limits = reinterpret_cast<MINMAXINFO *>(lParam);
        const UINT dpi = GetDpiForWindow(m_window);
        limits->ptMinTrackSize.x = scaleForDpi(640, dpi);
        limits->ptMinTrackSize.y = scaleForDpi(420, dpi);
        return 0;
    }
    case WM_CLOSE:
        saveWindowPlacement();
        if (!m_exitRequested) {
            const std::string behavior =
                m_backend ? m_backend->closeBehavior() : std::string("ask");
            if (behavior == "tray") {
                hideToTray();
                return 0;
            }
            if (behavior == "exit") {
                m_exitRequested = true;
                break;
            }
            // Let the WebView frontend render the first-close confirmation so
            // it follows the selected application theme instead of using a
            // Windows-native TaskDialog.  WM_CLOSE is intentionally paused
            // until the frontend sends app.closeDecision.
            if (m_closeDialogPending)
                return 0;
            if (m_backend && m_backend->requestCloseConfirmation()) {
                m_closeDialogPending = true;
                return 0;
            }
            const int choice = showCloseDialog();
            if (choice == CloseChoiceExit) {
                if (m_backend)
                    m_backend->setCloseBehavior("exit");
                m_exitRequested = true;
                break;
            }
            if (choice == CloseChoiceTray && m_backend)
                m_backend->setCloseBehavior("tray");
            hideToTray();
            return 0;
        }
        shutdown();
        DestroyWindow(m_window);
        return 0;
    case WM_ENDSESSION:
        requestExit();
        return 0;
    case WM_DESTROY:
        shutdown();
        PostQuitMessage(
            m_exitSelfTestDone ? m_exitSelfTestExitCode : 0);
        return 0;
    case WM_NCDESTROY:
    {
        HWND window = m_window;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        m_window = nullptr;
        return result;
    }
    default:
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

bool WebViewWindow::showStartupSplash() const
{
    if (!m_host || m_host->hasStartupError())
        return false;
    // Stop drawing the native splash as soon as the WebView2 controller is
    // visible. Keeping both surfaces active during a grace period causes
    // repeated full-window repaints and visible flicker.
    return !m_host->isReady();
}

void WebViewWindow::dismissActiveModal(bool lightDismissOnly)
{
    if (m_host) {
        if (lightDismissOnly) {
            m_host->executeScript(
                L"window.dismissActiveModal && window.dismissActiveModal({ lightDismissOnly: true });");
        } else {
            m_host->executeScript(
                L"window.dismissActiveModal && window.dismissActiveModal();");
        }
        m_host->focus();
    }
}

void WebViewWindow::configureRdpSession(
    const std::string &sessionId, RdpSession *session)
{
    if (!session)
        return;
    session->setDismissModalHandler([this]() {
        dismissActiveModal(true);
    });
    session->setKeyHandler([this, sessionId](UINT key) {
        if (key == VK_F11) {
            setRdpFullscreen(
                sessionId,
                !m_rdpFullscreen || m_rdpFullscreenSessionId != sessionId);
        } else if (key == VK_ESCAPE) {
            if (m_rdpModalDimmed)
                dismissActiveModal();
            else if (m_rdpFullscreen && m_rdpFullscreenSessionId == sessionId)
                setRdpFullscreen(sessionId, false);
            else if (m_backend)
                m_backend->notifyRdpContextAction(sessionId, "close");
            else
                closeRdpSession(sessionId);
        }
    });
    session->setMouseHandler([this, sessionId](bool inTopControlZone) {
        if (!inTopControlZone || !m_rdpFullscreen
            || m_rdpFullscreenSessionId != sessionId)
            return;
        if (m_rdpFullscreenOverlay && !m_rdpFullscreenOverlayPinned)
            setRdpFullscreenOverlayVisible(true);
        else if (m_backend)
            m_backend->notifyRdpFullscreenBar(sessionId, true);
    });
    session->setNativeFullscreenHandler([this, sessionId](bool entered) {
        if (!m_window || sessionId != m_rdpFullscreenSessionId)
            return;
        // Do not destroy/recreate the ActiveX control from inside its COM
        // event callback. Defer the owner-window z-order/exit transition to
        // the normal window message queue.
        PostMessageW(
            m_window, RdpNativeFullscreenChangedMessage,
            entered ? 1 : 0, 0);
    });
    session->setStateHandler([this, sessionId](const std::wstring &state) {
        notifyRdpState(sessionId, state);
    });
    session->setQualityHandler(
        [this, sessionId](const RdpQualitySnapshot &snapshot) {
            notifyRdpQuality(sessionId, snapshot);
        });
}

void WebViewWindow::openRdpSession(
    const std::string &sessionId, int profileIndex,
    const std::wstring &host, int port,
    const std::wstring &user, const std::wstring &password,
    const RdpConnectionOptions &options)
{
    if (sessionId.empty())
        return;
    if (findRdpSession(sessionId))
        closeRdpSession(sessionId);
    auto hosted = std::make_unique<RdpHostedSession>();
    hosted->profileIndex = profileIndex;
    hosted->host = host;
    hosted->user = user;
    hosted->password = password;
    hosted->port = port;
    hosted->options = options;
    hosted->connectPending = true;
    hosted->session = std::make_unique<RdpSession>(m_window);
    RdpSession *session = hosted->session.get();
    configureRdpSession(sessionId, session);
    m_activeRdpSessionId = sessionId;
    if (!session->create()) {
        notifyRdpState(L"error:RDP 控件不可用，请以管理员运行：regsvr32 C:\\Windows\\System32\\mstscax.dll");
        DiagnosticLog::write("rdp-error", sessionId + " control-unavailable");
        hosted.reset();
        m_activeRdpSessionId.clear();
        return;
    }
    m_rdpSessions.emplace(sessionId, std::move(hosted));
    DiagnosticLog::setSession(sessionId);
    DiagnosticLog::setPhase("rdp-connect");
    DiagnosticLog::write(
        "rdp-open",
        sessionId + " host=" + wideToUtf8(host)
            + " port=" + std::to_string(port));
    m_activeRdpSessionId = sessionId;
    m_rdpHostedVisible = true;
    ++m_rdpLayoutGeneration;
    // Keep WebView2 visible so the connection list and terminal tabs remain
    // interactive. The native RDP control is positioned only over the active
    // terminal content rectangle on the right.
    if (m_host)
        m_host->setWebViewVisible(true);
    ShowWindow(m_window, SW_SHOW);
    layoutRdpSession(sessionId, true, true);
}

void WebViewWindow::closeRdpSession(const std::string &sessionId)
{
    auto found = m_rdpSessions.find(sessionId);
    if (found == m_rdpSessions.end()) {
        m_rdpFailedCleanupTokens.erase(sessionId);
        return;
    }
    // A user close/reconnect supersedes any deferred failure cleanup for this
    // id. The old RdpSession is destroyed below; a later cleanup message must
    // not affect a newly opened session with the same id.
    m_rdpFailedCleanupTokens.erase(sessionId);
    found->second->failureCleanupQueued = false;
    found->second->failureCleanupToken = 0;
    if (m_rdpFullscreen && m_rdpFullscreenSessionId == sessionId)
        setRdpFullscreen(sessionId, false);
    ++m_rdpLayoutGeneration;
    found->second->session->setHostedVisible(false);
    found->second->session->disconnect();
    m_rdpSessions.erase(found);
    DiagnosticLog::write("rdp-close", sessionId);
    if (m_presentedRdpSessionId == sessionId)
        m_presentedRdpSessionId.clear();
    if (m_activeRdpSessionId == sessionId) {
        m_activeRdpSessionId.clear();
        m_rdpHostedVisible = false;
        for (auto &entry : m_rdpSessions)
            entry.second->session->setHostedVisible(false);
    }
    if (m_host)
        m_host->setWebViewVisible(true);
    if (m_host)
        m_host->resize();
    InvalidateRect(m_window, nullptr, FALSE);
}

bool WebViewWindow::recreateRdpSession(
    const std::string &sessionId, bool useMultimon)
{
    auto found = m_rdpSessions.find(sessionId);
    if (found == m_rdpSessions.end() || !found->second
        || !found->second->session)
        return false;

    RdpHostedSession &current = *found->second;
    m_rdpFailedCleanupTokens.erase(sessionId);
    current.failureCleanupQueued = false;
    current.failureCleanupToken = 0;
    auto replacement = std::make_unique<RdpHostedSession>();
    replacement->profileIndex = current.profileIndex;
    replacement->host = current.host;
    replacement->user = current.user;
    replacement->password = current.password;
    replacement->port = current.port;
    replacement->options = current.options;
    replacement->options.useMultimon = useMultimon ? 1 : 0;
    replacement->connectPending = true;
    replacement->session = std::make_unique<RdpSession>(m_window);
    RdpSession *session = replacement->session.get();
    configureRdpSession(sessionId, session);
    if (!session->create()) {
        DiagnosticLog::write(
            "rdp-display-mode-reconnect",
            sessionId + " replacement control unavailable");
        return false;
    }

    // The layout query is asynchronous. Without a pre-sized replacement,
    // mstscax starts the restored single-monitor session with the container's
    // default 320x240 desktop and only later receives the real panel size.
    // That intermediate mode is visible as a second reconnect and makes the
    // remote desktop briefly collapse to a tiny resolution when leaving
    // fullscreen.
    if (!useMultimon && m_rdpNormalHostRectValid) {
        const RECT &normalHost = m_rdpNormalHostRect;
        session->resize(
            normalHost.left, normalHost.top,
            normalHost.right - normalHost.left,
            normalHost.bottom - normalHost.top,
            true);
        DiagnosticLog::write(
            "rdp-display-mode-reconnect",
            sessionId + " pre-sized single-monitor replacement to "
                + std::to_string(normalHost.right - normalHost.left)
                + "x" + std::to_string(normalHost.bottom - normalHost.top));
    }

    // The all-monitor replacement is always hosted by MasterTerm. This keeps
    // mstscax from creating a second UIMainClass window when the owner spans
    // the local monitor union. The ordinary replacement uses the default
    // embedded/single-monitor path.
    session->setContainerHandledFullscreen(useMultimon);
    if (useMultimon && !session->setUseMultimon(true)) {
        DiagnosticLog::write(
            "rdp-display-mode-reconnect",
            sessionId + " multimon negotiation failed");
        showTrayNotification(
            L"RDP 多显示器模式不可用",
            L"已保留当前 RDP 连接状态，无法进入所有显示器全屏。请检查 RDP 控件或远端显示器支持。");
        return false;
    }

    current.session->setHostedVisible(false);
    current.session->disconnect();
    if (m_presentedRdpSessionId == sessionId)
        m_presentedRdpSessionId.clear();
    found->second = std::move(replacement);
    ++m_rdpLayoutGeneration;
    DiagnosticLog::write(
        "rdp-display-mode-reconnect",
        sessionId + (useMultimon
            ? " reconnecting with multimon-container"
            : " reconnecting with single-monitor"));
    layoutRdpSession(sessionId, true, true);
    return true;
}

void WebViewWindow::layoutRdpSession(
    const std::string &sessionId, bool hostedVisible,
    bool refreshDisplaySettings, bool clearOcclusion)
{
    const std::string previousPresentedSessionId =
        m_presentedRdpSessionId;
    const bool switchingFromVisibleRdp =
        hostedVisible && !sessionId.empty() && m_rdpHostedVisible
        && !previousPresentedSessionId.empty()
        && previousPresentedSessionId != sessionId
        && findRdpSession(previousPresentedSessionId)
        && findRdpSession(previousPresentedSessionId)->session->isConnected();
    m_activeRdpSessionId = sessionId;
    m_rdpHostedVisible = hostedVisible && !sessionId.empty();
    RdpHostedSession *hosted = findRdpSession(sessionId);
    if (clearOcclusion) {
        for (auto &entry : m_rdpSessions)
            entry.second->session->clearOcclusionRegion();
    }

    // Connected RDP controls are kept alive as visible native children. An
    // inactive control sits at HWND_BOTTOM underneath WebView2 instead of
    // being hidden, preserving mstscax's rendered desktop between switches.
    // During an RDP-to-RDP handoff, leave the previous control on top until
    // the target has its current bounds and can be raised in one operation.
    //
    // Do not eagerly clear the active control's region here. Overlay bounds
    // are queried asynchronously; clearing first exposes the native desktop
    // above an HTML modal until ExecuteScript returns. resize() replaces the
    // old region with the new snapshot atomically below.
    if (!hosted || !m_rdpHostedVisible) {
        m_rdpModalDimmed = false;
        for (auto &entry : m_rdpSessions) {
            if (clearOcclusion)
                entry.second->session->clearOcclusionRegion();
            entry.second->session->setHostedVisible(false);
        }
        m_presentedRdpSessionId.clear();
        return;
    }
    if (!hosted->connectPending && !hosted->session->isConnected()
        && !hosted->session->isConnecting()) {
        // The user selected a stale RDP tab after its native connection was
        // interrupted. Do not treat that empty ActiveX control as the active
        // native surface: doing so leaves another parked RDP HWND above HTML
        // menus because occlusion updates are sent to the dead session.
        m_rdpHostedVisible = false;
        for (auto &entry : m_rdpSessions) {
            if (clearOcclusion)
                entry.second->session->clearOcclusionRegion();
            entry.second->session->setHostedVisible(false);
        }
        m_presentedRdpSessionId.clear();
        if (m_host) {
            m_host->setWebViewVisible(true);
            m_host->resize();
        }
        InvalidateRect(m_window, nullptr, FALSE);
        return;
    }
    if (switchingFromVisibleRdp) {
        const bool targetReady = hosted->session->isConnected();
        if (!targetReady) {
            // A newly selected RDP session has no desktop frame until its
            // asynchronous handshake completes. Never keep the previously
            // presented desktop above it: that makes the new tab appear to
            // show the old connection while it is still connecting.
            RdpHostedSession *previous =
                findRdpSession(previousPresentedSessionId);
            if (previous)
                previous->session->setHostedVisible(false);
            hosted->session->setHostedVisible(true);
            m_presentedRdpSessionId.clear();
            for (auto &entry : m_rdpSessions) {
                if (entry.first != sessionId
                    && entry.first != previousPresentedSessionId)
                    entry.second->session->setHostedVisible(false);
            }
        } else {
            for (auto &entry : m_rdpSessions) {
                if (entry.first != previousPresentedSessionId)
                    entry.second->session->setHostedVisible(false);
            }
        }
    } else {
        // Raise the target before lowering any former foreground surface, so
        // WebView2 is never exposed as a black/empty frame between the two.
        hosted->session->setHostedVisible(true);
        for (auto &entry : m_rdpSessions) {
            if (entry.first != sessionId)
                entry.second->session->setHostedVisible(false);
        }
        m_presentedRdpSessionId = hosted->session->isConnected()
            ? sessionId : std::string();
    }
    const unsigned long long generation = ++m_rdpLayoutGeneration;
    RECT client{};
    GetClientRect(m_window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return;
    if (m_rdpFullscreen && m_rdpFullscreenUseMultimon
        && sessionId == m_rdpFullscreenSessionId) {
        // Container-handled multimon fullscreen has no HTML layout surface:
        // the embedded control fills the monitor-spanning Win32 client area
        // directly. Querying the hidden WebView here returns its old panel
        // bounds (for example 1280x800), which recreates the scroll viewport
        // that this mode is intended to eliminate.
        hosted->session->setHostedVisible(true);
        hosted->session->resize(
            0, 0, width, height,
            refreshDisplaySettings || hosted->connectPending);
        if (hosted->connectPending) {
            hosted->connectPending = false;
            hosted->session->connect(
                hosted->host, hosted->port, hosted->user,
                hosted->password, hosted->options);
        }
        if (hosted->session->isConnected())
            m_presentedRdpSessionId = sessionId;
        updateRdpFullscreenOverlay();
        return;
    }
    if (m_host) {
        m_host->queryTerminalBounds(
            [this, generation, sessionId, refreshDisplaySettings,
             previousPresentedSessionId, switchingFromVisibleRdp](
                WebViewHost::RdpLayoutSnapshot snapshot) {
                RdpHostedSession *current = findRdpSession(sessionId);
                if (generation != m_rdpLayoutGeneration || !current
                    || m_activeRdpSessionId != sessionId
                    || !m_rdpHostedVisible)
                    return;
                m_rdpModalDimmed = snapshot.modalOpen;
                bool hasBounds = snapshot.width > 0 && snapshot.height > 0;
                // The frontend hides the normal shell before its fullscreen
                // grid has completed a layout pass. Do not hide the native
                // RDP child because of that transient zero-sized snapshot;
                // the client area is the correct stable fullscreen fallback.
                if (!hasBounds && m_rdpFullscreen) {
                    RECT fullscreenClient{};
                    GetClientRect(m_window, &fullscreenClient);
                    snapshot.x = 0;
                    snapshot.y = 0;
                    snapshot.width = fullscreenClient.right
                        - fullscreenClient.left;
                    snapshot.height = fullscreenClient.bottom
                        - fullscreenClient.top;
                    snapshot.occlusionRects.clear();
                    hasBounds = snapshot.width > 0 && snapshot.height > 0;
                }
                if (hasBounds) {
                    const bool targetReady = current->session->isConnected();
                    // DesktopWidth/DesktopHeight are connection-time inputs.
                    // A second RDP session can begin while another native
                    // surface remains on top, but it still must receive the
                    // current panel size before Connect() or mstscax falls
                    // back to its 320x240 creation size.
                    const bool initializeDisplaySize = current->connectPending;
                    if (switchingFromVisibleRdp && !targetReady) {
                        // The target is now the only visible native surface.
                        // Its WebView overlay shows the connecting state until
                        // mstscax delivers the first connected desktop frame.
                        current->session->setHostedVisible(true);
                        current->session->resize(
                            snapshot.x, snapshot.y, snapshot.width,
                            snapshot.height,
                            refreshDisplaySettings || initializeDisplaySize,
                            snapshot.occlusionRects, snapshot.modalOpen);
                        m_presentedRdpSessionId.clear();
                    } else if (switchingFromVisibleRdp) {
                        // Lay out the target underneath WebView2 while the old
                        // surface remains on top. Raising the target and then
                        // lowering the previous host is a z-order-only swap;
                        // neither connected ActiveX control is hidden.
                        current->session->setHostedVisible(false);
                        current->session->resize(
                            snapshot.x, snapshot.y, snapshot.width,
                            snapshot.height,
                            refreshDisplaySettings || initializeDisplaySize,
                            snapshot.occlusionRects, snapshot.modalOpen);
                        current->session->setHostedVisible(true);
                        if (previousPresentedSessionId != sessionId) {
                            RdpHostedSession *previous =
                                findRdpSession(previousPresentedSessionId);
                            if (previous)
                                previous->session->setHostedVisible(false);
                        }
                        m_presentedRdpSessionId = sessionId;
                    } else {
                        current->session->setHostedVisible(true);
                        current->session->resize(
                            snapshot.x, snapshot.y, snapshot.width,
                            snapshot.height,
                            refreshDisplaySettings || initializeDisplaySize,
                            snapshot.occlusionRects, snapshot.modalOpen);
                        if (targetReady)
                            m_presentedRdpSessionId = sessionId;
                    }
                }
                if (hasBounds && m_rdpFullscreen)
                    updateRdpFullscreenOverlay();
                if (current->connectPending) {
                    current->connectPending = false;
                    current->session->connect(
                        current->host, current->port, current->user,
                        current->password, current->options);
                }
            });
    } else if (hosted->connectPending) {
        hosted->session->setHostedVisible(true);
        hosted->session->resize(
            0, 0, width, height, true);
        if (m_rdpFullscreen)
            updateRdpFullscreenOverlay();
        hosted->connectPending = false;
        hosted->session->connect(
            hosted->host, hosted->port, hosted->user, hosted->password,
            hosted->options);
    }
}

void WebViewWindow::transitionRdpAdvancedEditor(
    const std::string &sessionId, bool expanded,
    std::function<void(bool)> completion)
{
    RdpHostedSession *hosted = findRdpSession(sessionId);
    if (!m_host || !hosted || !m_rdpHostedVisible
        || sessionId != m_activeRdpSessionId
        || (!hosted->session->isConnected()
            && !hosted->session->isConnecting())) {
        if (completion)
            completion(false);
        return;
    }

    const unsigned long long generation = ++m_rdpLayoutGeneration;
    m_host->waitForVisualCommit(
        [this, generation, sessionId, expanded,
         completion = std::move(completion)](bool committed) mutable {
            RdpHostedSession *current = findRdpSession(sessionId);
            if (!committed || generation != m_rdpLayoutGeneration || !current
                || sessionId != m_activeRdpSessionId || !m_rdpHostedVisible) {
                if (completion)
                    completion(false);
                return;
            }

            m_host->queryTerminalBounds(
                [this, generation, sessionId,
                 completion = std::move(completion)](
                    WebViewHost::RdpLayoutSnapshot snapshot) mutable {
                    RdpHostedSession *current = findRdpSession(sessionId);
                    if (generation != m_rdpLayoutGeneration || !current
                        || sessionId != m_activeRdpSessionId
                        || !m_rdpHostedVisible || snapshot.width <= 0
                        || snapshot.height <= 0) {
                        if (completion)
                            completion(false);
                        return;
                    }

                    // CapturePreview confirmed that WebView2 has real pixels
                    // for this geometry. Apply the native regions before the
                    // frontend releases its transition snapshot.
                    current->session->resize(
                        snapshot.x, snapshot.y,
                        snapshot.width, snapshot.height,
                        false, snapshot.occlusionRects, snapshot.modalOpen);
                    if (completion)
                        completion(true);
                },
                expanded ? 1 : 0);
        });
}

void WebViewWindow::notifyRdpState(
    const std::string &sessionId, const std::wstring &state)
{
    DiagnosticLog::setSession(sessionId);
    DiagnosticLog::write("rdp-state", sessionId + " " + wideToUtf8(state));
    const bool terminalState = state == L"disconnected"
        || state.rfind(L"error:", 0) == 0;
    if (terminalState) {
        if (m_rdpFullscreen && m_rdpFullscreenSessionId == sessionId)
            setRdpFullscreen(sessionId, false);
        // Cancel any pending bounds callback before it can raise a native
        // surface whose transport has just gone away.
        ++m_rdpLayoutGeneration;
        RdpHostedSession *hosted = findRdpSession(sessionId);
        if (hosted) {
            hosted->connectPending = false;
            hosted->session->setHostedVisible(false);
            scheduleFailedRdpCleanup(sessionId);
        }
        if (m_presentedRdpSessionId == sessionId)
            m_presentedRdpSessionId.clear();
        if (m_activeRdpSessionId == sessionId) {
            m_rdpHostedVisible = false;
            for (auto &entry : m_rdpSessions)
                entry.second->session->setHostedVisible(false);
        }
        if (m_host) {
            m_host->setWebViewVisible(true);
            m_host->resize();
        }
        InvalidateRect(m_window, nullptr, FALSE);
    }
    const bool connectedContainerFullscreen = state == L"connected"
        && sessionId == m_activeRdpSessionId && m_rdpHostedVisible
        && m_rdpFullscreen && m_rdpFullscreenUseMultimon
        && m_rdpFullscreenSessionId == sessionId;
    if (connectedContainerFullscreen) {
        layoutRdpSession(sessionId, true, false);
        KillTimer(m_window, RdpFullscreenValidationTimer);
        SetTimer(m_window, RdpFullscreenValidationTimer, 600, nullptr);
        SetForegroundWindow(m_window);
        updateRdpFullscreenOverlay();
    } else if (state == L"connected" && sessionId == m_activeRdpSessionId
        && m_rdpHostedVisible)
        // A maximize/restore during the asynchronous handshake can change
        // the host size after Connect() captured the initial desktop size.
        // Query the settled panel now that the session is connected and send
        // the current dimensions to UpdateSessionDisplaySettings().
        layoutRdpSession(sessionId, true, true);
    // Forward the RDP lifecycle to the frontend so tabs/UI can react.
    if (!m_backend)
        return;
    const std::string utf8 = wideToUtf8(state);
    NativeJsonDom::Object payload;
    payload.values.emplace("state", utf8);
    m_backend->notifyRdpState(sessionId, utf8);
}

void WebViewWindow::scheduleFailedRdpCleanup(const std::string &sessionId)
{
    if (m_shuttingDown || !m_window || sessionId.empty())
        return;
    RdpHostedSession *hosted = findRdpSession(sessionId);
    if (!hosted || !hosted->session || hosted->failureCleanupQueued)
        return;

    const unsigned long long token = ++m_rdpFailureCleanupToken;
    hosted->failureCleanupToken = token;
    hosted->failureCleanupQueued = true;
    const auto [pending, inserted] = m_rdpFailedCleanupTokens.emplace(
        sessionId, token);
    if (!inserted) {
        hosted->failureCleanupToken = pending->second;
        return;
    }
    if (!PostMessageW(m_window, RdpFailedCleanupMessage, 0, 0)) {
        m_rdpFailedCleanupTokens.erase(sessionId);
        hosted->failureCleanupQueued = false;
        hosted->failureCleanupToken = 0;
        DiagnosticLog::write(
            "rdp-failed-cleanup", sessionId + " queue-failed");
        return;
    }
    DiagnosticLog::write(
        "rdp-failed-cleanup", sessionId + " queued");
}

void WebViewWindow::cleanupFailedRdpSessions()
{
    if (m_shuttingDown) {
        m_rdpFailedCleanupTokens.clear();
        return;
    }
    std::unordered_map<std::string, unsigned long long> pending;
    pending.swap(m_rdpFailedCleanupTokens);
    for (const auto &entry : pending)
        cleanupFailedRdpSession(entry.first, entry.second);
}

void WebViewWindow::cleanupFailedRdpSession(
    const std::string &sessionId, unsigned long long token)
{
    auto found = m_rdpSessions.find(sessionId);
    if (found == m_rdpSessions.end() || !found->second
        || !found->second->session
        || !found->second->failureCleanupQueued
        || found->second->failureCleanupToken != token)
        return;

    const bool wasActive = m_activeRdpSessionId == sessionId;
    const bool wasPresented = m_presentedRdpSessionId == sessionId;
    found->second->session->setHostedVisible(false);
    found->second->session->disconnect();
    m_rdpSessions.erase(found);
    DiagnosticLog::write(
        "rdp-failed-cleanup", sessionId + " native-host-released");

    if (wasPresented)
        m_presentedRdpSessionId.clear();
    if (wasActive) {
        m_activeRdpSessionId.clear();
        m_rdpHostedVisible = false;
        if (m_host) {
            m_host->setWebViewVisible(true);
            m_host->resize();
        }
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

void WebViewWindow::notifyRdpQuality(
    const std::string &sessionId, const RdpQualitySnapshot &snapshot)
{
    if (m_backend)
        m_backend->notifyRdpQuality(sessionId, snapshot);
}

void WebViewWindow::reflowAllRdpSessions(bool clearOcclusion)
{
    if (!m_host || m_rdpSessions.empty())
        return;
    if (clearOcclusion) {
        for (auto &entry : m_rdpSessions)
            if (entry.second->session)
                entry.second->session->clearOcclusionRegion();
    }
    const unsigned long long generation = ++m_rdpLayoutGeneration;
    m_host->queryTerminalBounds(
        [this, generation, clearOcclusion](
            WebViewHost::RdpLayoutSnapshot snapshot) {
            if (generation != m_rdpLayoutGeneration
                || snapshot.width <= 0 || snapshot.height <= 0)
                return;
            for (auto &entry : m_rdpSessions) {
                RdpSession *session = entry.second->session.get();
                if (!session)
                    continue;
                const bool active = m_rdpHostedVisible
                    && entry.first == m_activeRdpSessionId;
                if (clearOcclusion)
                    session->clearOcclusionRegion();
                session->setHostedVisible(active);
                session->resize(
                    snapshot.x, snapshot.y, snapshot.width, snapshot.height,
                    true,
                    active ? snapshot.occlusionRects
                           : std::vector<RdpOcclusionRegion>{},
                    active && snapshot.modalOpen);
                if (entry.second->connectPending) {
                    entry.second->connectPending = false;
                    session->connect(
                        entry.second->host, entry.second->port,
                        entry.second->user, entry.second->password,
                        entry.second->options);
                }
            }
            if (m_rdpFullscreen)
                updateRdpFullscreenOverlay();
        });
}

void WebViewWindow::notifyRdpState(const std::wstring &state)
{
    notifyRdpState(m_activeRdpSessionId, state);
}

void WebViewWindow::setApplicationTheme(const std::string &theme)
{
    if (theme == "light")
        m_rdpFullscreenOverlayTheme = 1;
    else if (theme == "blue")
        m_rdpFullscreenOverlayTheme = 2;
    else
        m_rdpFullscreenOverlayTheme = 0;
    if (m_rdpFullscreenOverlay)
        InvalidateRect(m_rdpFullscreenOverlay, nullptr, FALSE);
    if (m_rdpFullscreenOverlayPopup)
        InvalidateRect(m_rdpFullscreenOverlayPopup, nullptr, FALSE);
}

void WebViewWindow::setRdpFullscreen(
    const std::string &sessionId, bool enabled)
{
    RdpHostedSession *hosted = findRdpSession(sessionId);
    if (!m_window || !hosted)
        return;
    if (enabled && m_rdpFullscreen
        && m_rdpFullscreenSessionId != sessionId) {
        setRdpFullscreen(m_rdpFullscreenSessionId, false);
    }
    if (enabled == m_rdpFullscreen
        && (!enabled || m_rdpFullscreenSessionId == sessionId)) {
        notifyRdpFullscreen();
        return;
    }

    bool layoutAlreadyQueued = false;
    if (enabled) {
        m_rdpFullscreenSessionId = sessionId;
        // 0.1.54 keeps the ordinary session single-monitor and applies the
        // selected multi-monitor topology only to the container-hosted
        // fullscreen session. This also keeps the normal tab content inside
        // the MasterTerm window instead of exposing the virtual desktop.
        m_rdpFullscreenUseMultimon = hosted->options.useMultimon != 0;
        m_rdpNormalHostRectValid = false;
        if (m_rdpFullscreenUseMultimon) {
            int hostX = 0;
            int hostY = 0;
            int hostWidth = 0;
            int hostHeight = 0;
            if (hosted->session->hostBounds(
                    hostX, hostY, hostWidth, hostHeight)) {
                m_rdpNormalHostRect = RECT{
                    hostX, hostY, hostX + hostWidth, hostY + hostHeight};
                m_rdpNormalHostRectValid = true;
            }
        }
        m_rdpNormalStyle = GetWindowLongPtrW(m_window, GWL_STYLE);
        m_rdpNormalExStyle = GetWindowLongPtrW(m_window, GWL_EXSTYLE);
        m_rdpNormalPlacement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(m_window, &m_rdpNormalPlacement))
            m_rdpNormalPlacement.showCmd = SW_SHOWNORMAL;
        m_rdpNormalWindowRectValid =
            GetWindowRect(m_window, &m_rdpNormalWindowRect) != FALSE;
        DiagnosticLog::write(
            "rdp-fullscreen-save-window",
            "show=" + std::to_string(m_rdpNormalPlacement.showCmd)
                + " rect=" + std::to_string(m_rdpNormalWindowRect.left)
                + "," + std::to_string(m_rdpNormalWindowRect.top)
                + "," + std::to_string(m_rdpNormalWindowRect.right)
                + "," + std::to_string(m_rdpNormalWindowRect.bottom));
        m_rdpFullscreenTransition = true;
        m_rdpFullscreen = true;

        const auto applyCurrentMonitorFullscreen = [this]() -> bool {
            HMONITOR monitor = MonitorFromWindow(
                m_window, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(monitor, &monitorInfo))
                return false;
            SetWindowLongPtrW(
                m_window, GWL_STYLE,
                m_rdpNormalStyle & ~(WS_CAPTION | WS_THICKFRAME
                                     | WS_MINIMIZE | WS_MAXIMIZE
                                     | WS_SYSMENU));
            SetWindowLongPtrW(
                m_window, GWL_EXSTYLE,
                m_rdpNormalExStyle
                    & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE
                        | WS_EX_STATICEDGE));
            SetWindowPos(
                m_window, HWND_TOP,
                monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return true;
        };

        const auto applyAllMonitorFullscreen = [this]() -> bool {
            VirtualMonitorLayout layout;
            if (!queryVirtualMonitorLayout(layout) || layout.count < 2)
                return false;
            if (m_host)
                m_host->setWebViewVisible(false);
            SetWindowLongPtrW(
                m_window, GWL_STYLE,
                m_rdpNormalStyle & ~(WS_CAPTION | WS_THICKFRAME
                                     | WS_MINIMIZE | WS_MAXIMIZE
                                     | WS_SYSMENU));
            SetWindowLongPtrW(
                m_window, GWL_EXSTYLE,
                (m_rdpNormalExStyle
                    & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE
                        | WS_EX_STATICEDGE | WS_EX_APPWINDOW))
                    | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
            setTaskbarTabVisible(m_window, false);
            m_rdpMainWindowDemotedForNativeFullscreen = true;
            const BOOL positioned = SetWindowPos(
                m_window, HWND_TOPMOST,
                layout.bounds.left, layout.bounds.top,
                layout.bounds.right - layout.bounds.left,
                layout.bounds.bottom - layout.bounds.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            DiagnosticLog::write(
                "rdp-fullscreen-container",
                "monitors=" + std::to_string(layout.count)
                    + " bounds=" + std::to_string(layout.bounds.left)
                    + "," + std::to_string(layout.bounds.top)
                    + "-" + std::to_string(layout.bounds.right)
                    + "," + std::to_string(layout.bounds.bottom)
                    + " positioned=" + (positioned ? "true" : "false"));
            return positioned != FALSE;
        };

        if (!m_rdpFullscreenUseMultimon
            && !applyCurrentMonitorFullscreen()) {
            m_rdpFullscreen = false;
            m_rdpFullscreenTransition = false;
            m_rdpFullscreenSessionId.clear();
            return;
        }

        bool displayModeApplied = true;
        if (m_rdpFullscreenUseMultimon) {
            // UseMultimon was negotiated before the session connected. Entering
            // fullscreen now only changes the local host window; recreating the
            // ActiveX control here would make Windows rebuild the remote shell
            // work areas and move existing applications between monitors.
            DiagnosticLog::write(
                "rdp-fullscreen-display-mode",
                sessionId + " multimon already negotiated; local fullscreen only");
            displayModeApplied = applyAllMonitorFullscreen();
        }
        if (!displayModeApplied) {
            // Keep the existing RDP session alive even if the local monitor
            // union cannot be presented. Fall back to the current monitor;
            // reconnecting here would undo the window-position guarantee.
            m_rdpFullscreenUseMultimon = false;
            if (m_rdpMainWindowDemotedForNativeFullscreen) {
                setTaskbarTabVisible(m_window, true);
                m_rdpMainWindowDemotedForNativeFullscreen = false;
            }
            if (m_host)
                m_host->setWebViewVisible(true);
            applyCurrentMonitorFullscreen();
            showTrayNotification(
                L"RDP 双屏全屏不可用",
                L"已保留当前连接并切换为当前显示器全屏，未重新连接远程桌面。");
        } else if (m_rdpFullscreenUseMultimon
                   && !recreateRdpSession(sessionId, true)) {
            // Restore the window if the replacement control cannot be
            // created. Do not leave the WebView hidden behind an unusable
            // fullscreen host.
            setRdpFullscreen(sessionId, false);
            return;
        } else if (m_rdpFullscreenUseMultimon) {
            layoutAlreadyQueued = true;
        }
        m_rdpFullscreenTransition = false;
        if (m_rdpFullscreenUseMultimon)
            DiagnosticLog::write(
                "rdp-fullscreen-host",
                "container-handled multimon; WebView hidden");
    } else {
        const WINDOWPLACEMENT normalPlacement = m_rdpNormalPlacement;
        const bool restoreWindowRect = m_rdpNormalWindowRectValid;
        const RECT normalWindowRect = m_rdpNormalWindowRect;
        const bool restoreSingleMonitor = m_rdpFullscreenUseMultimon;
        m_rdpFullscreenTransition = true;
        KillTimer(m_window, RdpFullscreenValidationTimer);
        // Keep the negotiated RDP session and its remote monitor topology
        // alive while restoring the MasterTerm window. This prevents Windows
        // from rebuilding the remote shell and moving open applications when
        // fullscreen is left.
        m_rdpFullscreenUseMultimon = false;
        m_rdpFullscreen = false;
        SetWindowLongPtrW(m_window, GWL_STYLE, m_rdpNormalStyle);
        SetWindowLongPtrW(m_window, GWL_EXSTYLE, m_rdpNormalExStyle);
        if (m_rdpMainWindowDemotedForNativeFullscreen) {
            setTaskbarTabVisible(m_window, true);
            m_rdpMainWindowDemotedForNativeFullscreen = false;
        }
        const bool restoreMaximized =
            normalPlacement.showCmd == SW_SHOWMAXIMIZED;
        const bool restoreMinimized =
            normalPlacement.showCmd == SW_SHOWMINIMIZED;
        if (normalPlacement.length == sizeof(WINDOWPLACEMENT)) {
            // First restore a normal placement. This moves the frame away from
            // the virtual desktop before a maximized state is reapplied.
            WINDOWPLACEMENT normal = normalPlacement;
            normal.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(m_window, &normal);
        }
        if (!restoreMaximized && !restoreMinimized && restoreWindowRect) {
            SetWindowPos(
                m_window, nullptr,
                normalWindowRect.left, normalWindowRect.top,
                normalWindowRect.right - normalWindowRect.left,
                normalWindowRect.bottom - normalWindowRect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        SetWindowPos(
            m_window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED
                | SWP_NOACTIVATE);
        const int showCommand = restoreMinimized
            ? SW_RESTORE : static_cast<int>(m_rdpNormalPlacement.showCmd);
        ShowWindow(m_window, showCommand);
        SetForegroundWindow(m_window);
        RECT restoredWindowRect{};
        if (GetWindowRect(m_window, &restoredWindowRect)) {
            DiagnosticLog::write(
                "rdp-fullscreen-restore-window",
                "show=" + std::to_string(showCommand)
                    + " rect=" + std::to_string(restoredWindowRect.left)
                    + "," + std::to_string(restoredWindowRect.top)
                    + "," + std::to_string(restoredWindowRect.right)
                    + "," + std::to_string(restoredWindowRect.bottom));
        }
        m_rdpNormalWindowRectValid = false;
        if (m_host) {
            m_host->setWebViewVisible(true);
            DiagnosticLog::write(
                "rdp-fullscreen-webview", "shown after fullscreen");
        }

        if (restoreSingleMonitor) {
            // A terminal disconnect can arrive while the native fullscreen
            // transition is unwinding. Recreating an already failed control
            // here starts a fresh connection behind the failure notice and
            // keeps the replacement alive until the next user reconnect. The
            // failed session is going to be released by the deferred failure
            // cleanup, so only recreate while the original transport is still
            // usable.
            RdpHostedSession *current = findRdpSession(sessionId);
            const bool canReconnect = current && current->session
                && (current->session->isConnected()
                    || current->session->isConnecting());
            if (!canReconnect) {
                DiagnosticLog::write(
                    "rdp-display-mode-reconnect",
                    sessionId + " skipped after terminal RDP state");
            } else if (!recreateRdpSession(sessionId, false))
                DiagnosticLog::write(
                    "rdp-display-mode-reconnect",
                    sessionId + " failed to restore single-monitor session");
            else
                layoutAlreadyQueued = true;
        }
        DiagnosticLog::write(
            "rdp-fullscreen-display-mode",
            restoreSingleMonitor
                ? "restored single-monitor session after fullscreen"
                : "current-monitor session kept after fullscreen");
        m_rdpFullscreenTransition = false;
        m_rdpNormalHostRectValid = false;
    }

    if (m_host)
        m_host->resize();
    if (!m_activeRdpSessionId.empty() && !layoutAlreadyQueued)
        layoutRdpSession(m_activeRdpSessionId, true, true);
    if (m_rdpFullscreen) {
        ensureRdpFullscreenOverlay();
        // A newly entered fullscreen session starts with a hidden toolbar;
        // moving to the top edge reveals it and pinning is explicit.
        m_rdpFullscreenOverlayPinned = false;
        m_rdpFullscreenOverlayHideAt = 0;
        m_rdpFullscreenOverlayMenu = 0;
        setRdpFullscreenOverlayVisible(false);
        SetTimer(m_window, RdpFullscreenOverlayTimer, 100, nullptr);
    } else {
        KillTimer(m_window, RdpFullscreenOverlayTimer);
        m_rdpFullscreenOverlayMenu = 0;
        setRdpFullscreenOverlayVisible(false);
    }
    notifyRdpFullscreen();
}

void WebViewWindow::notifyRdpFullscreen()
{
    if (m_backend)
        m_backend->notifyRdpFullscreen(
            m_rdpFullscreenSessionId, m_rdpFullscreen);
    if (!m_rdpFullscreen)
        m_rdpFullscreenSessionId.clear();
}

void WebViewWindow::ensureRdpFullscreenOverlay()
{
    if (m_rdpFullscreenOverlay || !m_window)
        return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    if (!GetClassInfoExW(
            instance, RdpFullscreenOverlayClassName, &windowClass)) {
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc =
            &WebViewWindow::rdpFullscreenOverlayProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = RdpFullscreenOverlayClassName;
        if (!RegisterClassExW(&windowClass))
            return;
    }
    m_rdpFullscreenOverlay = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        RdpFullscreenOverlayClassName, L"",
        WS_POPUP | WS_CLIPSIBLINGS,
        0, 0, 0, 0, m_window, nullptr, instance, this);
    m_rdpFullscreenOverlayPopup = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        RdpFullscreenOverlayClassName, L"",
        WS_POPUP | WS_CLIPSIBLINGS,
        0, 0, 0, 0, m_window, nullptr, instance, this);
    if (m_rdpFullscreenOverlay)
        m_rdpFullscreenOverlayHover = -1;
}

void WebViewWindow::updateRdpFullscreenOverlay()
{
    if (!m_rdpFullscreenOverlay || !m_window)
        return;
    HMONITOR monitor = m_rdpNormalWindowRectValid
        ? MonitorFromRect(&m_rdpNormalWindowRect, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;
    const UINT dpi = std::max<UINT>(
        96, GetDpiForWindow(m_rdpFullscreenOverlay));
    const int width = scaleForDpi(370, dpi);
    const int height = scaleForDpi(52, dpi);
    const int monitorWidth = static_cast<int>(
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left);
    const int x = monitorInfo.rcMonitor.left
        + std::max(0, (monitorWidth - width) / 2);
    const int y = monitorInfo.rcMonitor.top + scaleForDpi(12, dpi);
    SetWindowPos(
        m_rdpFullscreenOverlay, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | (m_rdpFullscreenOverlayVisible
            ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (m_rdpFullscreenOverlayVisible)
        InvalidateRect(m_rdpFullscreenOverlay, nullptr, FALSE);

    if (!m_rdpFullscreenOverlayPopup)
        return;
    const ULONGLONG now = GetTickCount64();
    const bool showMenu = m_rdpFullscreenOverlayMenu != 0;
    const bool showTooltip = !showMenu
        && m_rdpFullscreenOverlayHover >= 0
        && m_rdpFullscreenOverlayHover < 6
        && m_rdpFullscreenOverlayHoverAt != 0
        && now >= m_rdpFullscreenOverlayHoverAt + 450;
    const bool showPopup = m_rdpFullscreenOverlayVisible
        && (showMenu || showTooltip);
    RECT overlayRect{};
    GetWindowRect(m_rdpFullscreenOverlay, &overlayRect);
    int popupX = overlayRect.left;
    int popupY = overlayRect.bottom + scaleForDpi(6, dpi);
    const int popupWidth = scaleForDpi(
        m_rdpFullscreenOverlayMenu == 4 ? 330 : (showMenu ? 150 : 128), dpi);
    const int popupHeight = scaleForDpi(
        m_rdpFullscreenOverlayMenu == 4 ? 232 : (showMenu
            ? 8 + rdpFullscreenOverlayMenuItemCount(
                m_rdpFullscreenOverlayMenu) * 30 : 32), dpi);
    if (showMenu) {
        RECT menuButton{};
        rdpFullscreenOverlayButtonRect(
            m_rdpFullscreenOverlayMenu, dpi, menuButton);
        popupX += menuButton.left;
    } else {
        RECT hoveredButton{};
        rdpFullscreenOverlayButtonRect(
            m_rdpFullscreenOverlayHover, dpi, hoveredButton);
        const int center = (hoveredButton.left + hoveredButton.right) / 2;
        popupX += std::max(
            scaleForDpi(6, dpi),
            std::min(width - popupWidth - scaleForDpi(6, dpi),
                     center - popupWidth / 2));
    }
    SetWindowPos(
        m_rdpFullscreenOverlayPopup, HWND_TOPMOST,
        popupX, popupY, popupWidth, popupHeight,
        SWP_NOACTIVATE | (showPopup ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (showPopup)
        InvalidateRect(m_rdpFullscreenOverlayPopup, nullptr, FALSE);
}

void WebViewWindow::setRdpFullscreenOverlayVisible(bool visible)
{
    if (!m_rdpFullscreenOverlay)
        return;
    m_rdpFullscreenOverlayVisible = visible;
    if (!visible) {
        m_rdpFullscreenOverlayMenu = 0;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
        m_rdpFullscreenOverlayHideAt = 0;
    }
    else if (!m_rdpFullscreenOverlayPinned)
        m_rdpFullscreenOverlayHideAt = GetTickCount64() + 2800;
    updateRdpFullscreenOverlay();
    if (m_backend)
        m_backend->notifyRdpFullscreenNativeBar(
            m_rdpFullscreenSessionId, visible,
            m_rdpFullscreenOverlayPinned);
}

void WebViewWindow::pollRdpFullscreenOverlay()
{
    if (!m_rdpFullscreen || !m_rdpFullscreenOverlay)
        return;
    // RDP/ActiveX can raise its child again after focus changes. Reassert the
    // overlay's sibling Z-order while fullscreen so it remains clickable.
    updateRdpFullscreenOverlay();
    POINT cursor{};
    if (!GetCursorPos(&cursor))
        return;
    HMONITOR monitor = m_rdpNormalWindowRectValid
        ? MonitorFromRect(&m_rdpNormalWindowRect, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;
    const UINT dpi = std::max<UINT>(
        96, GetDpiForWindow(m_rdpFullscreenOverlay));
    const int topZoneHeight = scaleForDpi(72, dpi);
    const int topZoneHalfWidth = scaleForDpi(220, dpi);
    const int monitorCenter = monitorInfo.rcMonitor.left
        + (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) / 2;
    const bool inTopZone = cursor.y >= monitorInfo.rcMonitor.top
        && cursor.y <= monitorInfo.rcMonitor.top + topZoneHeight
        && std::abs(cursor.x - monitorCenter)
            <= topZoneHalfWidth;
    RECT overlayRect{};
    GetWindowRect(m_rdpFullscreenOverlay, &overlayRect);
    if (m_rdpFullscreenOverlayPopup
        && IsWindowVisible(m_rdpFullscreenOverlayPopup)) {
        RECT popupRect{};
        GetWindowRect(m_rdpFullscreenOverlayPopup, &popupRect);
        UnionRect(&overlayRect, &overlayRect, &popupRect);
    }
    const bool overOverlay = PtInRect(&overlayRect, cursor) != FALSE;
    // Treat leaving the toolbar as cancelling its open menu.  The RDP child
    // owns the rest of the screen, so this poll closes the menu even when the
    // outside click never reaches the toolbar window itself.
    if (!overOverlay && m_rdpFullscreenOverlayMenu != 0) {
        m_rdpFullscreenOverlayMenu = 0;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
        updateRdpFullscreenOverlay();
    }
    if (m_rdpFullscreenOverlayPinned)
        return;
    if (inTopZone || overOverlay) {
        m_rdpFullscreenOverlayHideAt = GetTickCount64() + 2800;
        if (!m_rdpFullscreenOverlayVisible)
            setRdpFullscreenOverlayVisible(true);
        return;
    }
    if (m_rdpFullscreenOverlayVisible
        && m_rdpFullscreenOverlayHideAt != 0
        && GetTickCount64() >= m_rdpFullscreenOverlayHideAt)
        setRdpFullscreenOverlayVisible(false);
}

void WebViewWindow::handleRdpFullscreenOverlayAction(int button)
{
    RdpHostedSession *hosted =
        findRdpSession(m_rdpFullscreenSessionId);
    RdpSession *session = hosted ? hosted->session.get() : nullptr;
    if (!m_rdpFullscreen || !session)
        return;
    if (button == 0) {
        m_rdpFullscreenOverlayPinned = !m_rdpFullscreenOverlayPinned;
        m_rdpFullscreenOverlayHideAt = m_rdpFullscreenOverlayPinned
            ? 0 : GetTickCount64() + 2800;
        updateRdpFullscreenOverlay();
        if (m_backend)
            m_backend->notifyRdpFullscreenNativeBar(
                m_rdpFullscreenSessionId, true,
                m_rdpFullscreenOverlayPinned);
        return;
    }
    if (button >= 100) {
        const int menu = button / 10 - 10;
        const int item = button % 10;
        m_rdpFullscreenOverlayMenu = 0;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
        // Hide the top-level popup before restoring focus and injecting a
        // keyboard sequence. In nested/virtualized desktops the focused RDP
        // control must own the input path before the first key-down event.
        updateRdpFullscreenOverlay();
        if (menu == 1) {
            if (item == 0) {
                session->setSmartSizing(true);
                layoutRdpSession(m_rdpFullscreenSessionId, true, true);
            } else if (item == 1) {
                layoutRdpSession(m_rdpFullscreenSessionId, true, true);
            } else if (item == 2) {
                session->setSmartSizing(false);
                layoutRdpSession(m_rdpFullscreenSessionId, true, true);
            }
        } else if (menu == 2) {
            if (item == 0 && m_backend)
                m_backend->notifyRdpContextAction(
                    m_rdpFullscreenSessionId, "reconnect");
            else if (item == 1)
                setRdpFullscreen(m_rdpFullscreenSessionId, false);
            else if (item == 2 && m_backend)
                m_backend->notifyRdpContextAction(
                    m_rdpFullscreenSessionId, "close");
        } else if (menu == 3) {
            if (item == 0)
                session->launchRemoteUtility(L"taskmgr.exe");
            else if (item == 1)
                session->launchRemoteUtility(L"control.exe");
            else if (item == 2)
                session->launchRemoteUtility(L"regedit.exe");
            else if (item == 3)
                session->launchRemoteUtility(L"ms-settings:");
            else if (item == 4)
                session->launchRemoteUtility(L"services.msc");
            else if (item == 5)
                session->launchRemoteUtility(L"devmgmt.msc");
        }
        updateRdpFullscreenOverlay();
        return;
    }
    if (button == 1 || button == 2 || button == 3) {
        showRdpFullscreenOverlayMenu(button);
        return;
    }
    if (button == 4) {
        if (m_rdpFullscreenOverlayMenu == 4)
            m_rdpFullscreenOverlayMenu = 0;
        else
            m_rdpFullscreenOverlayMenu = 4;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
        m_rdpFullscreenOverlayHideAt = GetTickCount64() + 5000;
        updateRdpFullscreenOverlay();
        return;
    }
    if (button == 5 && m_backend)
        m_backend->notifyRdpContextAction(
            m_rdpFullscreenSessionId, "close");
}

void WebViewWindow::showRdpFullscreenOverlayMenu(int menu)
{
    if (!m_window || !m_rdpFullscreenOverlay || menu < 1 || menu > 4)
        return;
    const int count = rdpFullscreenOverlayMenuItemCount(menu);
    if (count == 0 && menu != 4)
        return;
    if (m_rdpFullscreenOverlayMenu == menu) {
        m_rdpFullscreenOverlayMenu = 0;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
    } else {
        m_rdpFullscreenOverlayMenu = menu;
        m_rdpFullscreenOverlayHover = -1;
        m_rdpFullscreenOverlayHoverAt = 0;
        m_rdpFullscreenOverlayHideAt = GetTickCount64() + 2800;
    }
    updateRdpFullscreenOverlay();
}

int WebViewWindow::rdpFullscreenOverlayButtonAt(POINT point) const
{
    if (!m_window)
        return -1;
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(m_window));
    for (int index = 0; index < 6; ++index) {
        RECT button{};
        rdpFullscreenOverlayButtonRect(index, dpi, button);
        if (PtInRect(&button, point))
            return index;
    }
    return -1;
}

int WebViewWindow::rdpFullscreenOverlayPopupButtonAt(POINT point) const
{
    if (!m_window || m_rdpFullscreenOverlayMenu == 0
        || m_rdpFullscreenOverlayMenu == 4)
        return -1;
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(m_window));
    const int count = rdpFullscreenOverlayMenuItemCount(
        m_rdpFullscreenOverlayMenu);
    for (int item = 0; item < count; ++item) {
        RECT itemRect{
            scaleForDpi(6, dpi),
            scaleForDpi(4 + item * 30, dpi),
            scaleForDpi(144, dpi),
            scaleForDpi(30 + item * 30, dpi)};
        if (PtInRect(&itemRect, point))
            return 100 + m_rdpFullscreenOverlayMenu * 10 + item;
    }
    return -1;
}

void WebViewWindow::paintRdpFullscreenOverlay(HDC dc) const
{
    if (!dc || !m_window)
        return;
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(m_window));
    const RdpFullscreenOverlayPalette palette =
        rdpFullscreenOverlayPalette(m_rdpFullscreenOverlayTheme);
    const int width = scaleForDpi(370, dpi);
    const int height = scaleForDpi(52, dpi);
    RECT bounds{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(palette.background);
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    HPEN border = CreatePen(
        PS_SOLID, scaleForDpi(1, dpi), palette.border);
    HGDIOBJ previousPen = SelectObject(dc, border);
    HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(
        dc, 0, 0, width, height,
        scaleForDpi(10, dpi), scaleForDpi(10, dpi));
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(border);

    HFONT font = CreateFontW(
        -scaleForDpi(12, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette.text);
    RECT label{scaleForDpi(8, dpi), 0, scaleForDpi(48, dpi),
               scaleForDpi(52, dpi)};
    DrawTextW(dc, L"RDP", -1, &label,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    const wchar_t *icons[] = {L"📌", L"▣", L"↔", L"⚙", L"ⓘ", L"×"};
    for (int index = 0; index < 6; ++index) {
        RECT button{};
        rdpFullscreenOverlayButtonRect(index, dpi, button);
        const bool active = (index == 0 && m_rdpFullscreenOverlayPinned)
            || (index >= 1 && index <= 3
                && m_rdpFullscreenOverlayMenu == index)
            || (index == 4 && m_rdpFullscreenOverlayMenu == 4);
        const COLORREF fill = m_rdpFullscreenOverlayHover == index
            ? palette.buttonHover : active ? palette.buttonActive
                                           : palette.button;
        HBRUSH buttonBrush = CreateSolidBrush(fill);
        HPEN buttonPen = CreatePen(
            PS_SOLID, scaleForDpi(1, dpi),
            m_rdpFullscreenOverlayHover == index
                ? palette.buttonHoverBorder : palette.buttonBorder);
        HGDIOBJ oldBrush = SelectObject(dc, buttonBrush);
        HGDIOBJ oldPen = SelectObject(dc, buttonPen);
        RoundRect(
            dc, button.left, button.top, button.right, button.bottom,
            scaleForDpi(5, dpi), scaleForDpi(5, dpi));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(buttonPen);
        DeleteObject(buttonBrush);
        SetTextColor(dc, index == 5 ? palette.closeText : palette.text);
        DrawTextW(
            dc, icons[index], -1, &button,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, previousFont);
    DeleteObject(font);
}

void WebViewWindow::paintRdpFullscreenOverlayPopup(HDC dc) const
{
    if (!dc || !m_window)
        return;
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(m_window));
    const RdpFullscreenOverlayPalette palette =
        rdpFullscreenOverlayPalette(m_rdpFullscreenOverlayTheme);
    const bool menu = m_rdpFullscreenOverlayMenu != 0;
    const int count = rdpFullscreenOverlayMenuItemCount(
        m_rdpFullscreenOverlayMenu);
    const bool quality = m_rdpFullscreenOverlayMenu == 4;
    const int width = scaleForDpi(
        quality ? 330 : (menu ? 150 : 128), dpi);
    const int height = scaleForDpi(
        quality ? 232 : (menu ? 8 + count * 30 : 32), dpi);
    RECT bounds{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(palette.background);
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    HPEN border = CreatePen(
        PS_SOLID, scaleForDpi(1, dpi), palette.border);
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(
        dc, 0, 0, width, height,
        scaleForDpi(7, dpi), scaleForDpi(7, dpi));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    HFONT font = CreateFontW(
        -scaleForDpi(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    if (quality) {
        const RdpQualitySnapshot *snapshot = nullptr;
        const auto found = m_rdpSessions.find(m_rdpFullscreenSessionId);
        if (found != m_rdpSessions.end() && found->second
            && found->second->session)
            snapshot = &found->second->session->qualitySnapshot();
        RECT titleRect{scaleForDpi(14, dpi), scaleForDpi(10, dpi),
                       width - scaleForDpi(14, dpi), scaleForDpi(34, dpi)};
        SetTextColor(dc, palette.text);
        HFONT titleFont = CreateFontW(
            -scaleForDpi(13, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ previousTitleFont = SelectObject(dc, titleFont);
        DrawTextW(dc, L"RDP 实际质量", -1, &titleRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, previousTitleFont);
        DeleteObject(titleFont);
        const auto value = [](int number) {
            return std::to_wstring(number);
        };
        const auto boolValue = [](bool enabled) {
            return enabled ? std::wstring(L"开启") : std::wstring(L"关闭");
        };
        std::wstring rows[9] = {
            L"色深：" + (snapshot ? value(snapshot->colorDepth) + L" 位" : L"等待回读"),
            L"分辨率：" + (snapshot
                ? value(snapshot->displayWidth) + L" × "
                    + value(snapshot->displayHeight) : L"等待连接"),
            L"SmartSizing：" + (snapshot ? boolValue(snapshot->smartSizing) : L"等待回读"),
            L"性能标志：" + (snapshot ? value(snapshot->performanceFlags) : L"等待回读"),
            L"连接类型：" + (snapshot && snapshot->networkConnectionType > 0
                ? value(snapshot->networkConnectionType)
                    + (snapshot->networkConnectionType == 6 ? L"（LAN）" : L"")
                : L"自动"),
            L"带宽检测：" + (snapshot ? boolValue(snapshot->bandwidthDetection) : L"等待回读"),
            L"协议模式：" + (snapshot
                ? value(snapshot->clientProtocolSpec)
                    + (snapshot->clientProtocolSpec == 0 ? L"（FullMode）" : L"")
                : L"等待回读"),
            L"完整刷新：" + (snapshot ? value(snapshot->fullFrameRefreshCount) : L"0"),
            L"编码/AVC：" + (snapshot ? snapshot->avcStatus : L"等待检测")};
        HFONT rowFont = CreateFontW(
            -scaleForDpi(11, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ previousRowFont = SelectObject(dc, rowFont);
        for (int index = 0; index < 9; ++index) {
            RECT row{scaleForDpi(14, dpi), scaleForDpi(38 + index * 20, dpi),
                     width - scaleForDpi(14, dpi),
                     scaleForDpi(58 + index * 20, dpi)};
            SetTextColor(dc, palette.text);
            DrawTextW(dc, rows[index].c_str(), -1, &row,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        if (snapshot) {
            RECT footer{scaleForDpi(14, dpi), scaleForDpi(218, dpi),
                        width - scaleForDpi(14, dpi), scaleForDpi(230, dpi)};
            std::wostringstream hresultStream;
            hresultStream << L"0x" << std::uppercase << std::hex
                          << std::setw(8) << std::setfill(L'0')
                          << snapshot->lastHresult;
            const std::wstring hresult = L"最近 "
                + snapshot->lastOperation + L"："
                + hresultStream.str()
                + (snapshot->lastHresultSucceeded ? L"（成功）" : L"（失败）");
            SetTextColor(dc, snapshot->lastHresultSucceeded
                ? palette.text : palette.closeText);
            DrawTextW(dc, hresult.c_str(), -1, &footer,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        SelectObject(dc, previousRowFont);
        DeleteObject(rowFont);
    } else if (menu) {
        const wchar_t *labels[][6] = {
            {L"适应窗口", L"刷新分辨率", L"关闭智能缩放", L"", L"", L""},
            {L"重新连接", L"退出全屏", L"关闭连接", L"", L"", L""},
            {L"任务管理器", L"控制面板", L"注册表编辑器",
             L"Windows 设置", L"服务管理", L"设备管理器"}};
        for (int item = 0; item < count; ++item) {
            RECT itemRect{
                scaleForDpi(6, dpi),
                scaleForDpi(4 + item * 30, dpi),
                scaleForDpi(144, dpi),
                scaleForDpi(30 + item * 30, dpi)};
            const int encoded = 100 + m_rdpFullscreenOverlayMenu * 10 + item;
            if (m_rdpFullscreenOverlayHover == encoded) {
                HBRUSH hoverBrush = CreateSolidBrush(palette.buttonHover);
                FillRect(dc, &itemRect, hoverBrush);
                DeleteObject(hoverBrush);
            }
            RECT textRect = itemRect;
            textRect.left += scaleForDpi(8, dpi);
            SetTextColor(dc, palette.text);
            DrawTextW(
                dc, labels[m_rdpFullscreenOverlayMenu - 1][item], -1,
                &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    } else if (m_rdpFullscreenOverlayHover >= 0
               && m_rdpFullscreenOverlayHover < 6) {
        RECT textRect{0, 0, width, height};
        SetTextColor(dc, palette.text);
        DrawTextW(
            dc,
            rdpFullscreenOverlayTooltip(
                m_rdpFullscreenOverlayHover,
                m_rdpFullscreenOverlayPinned),
            -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

LRESULT CALLBACK WebViewWindow::rdpFullscreenOverlayProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *self = reinterpret_cast<WebViewWindow *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = static_cast<WebViewWindow *>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(window, message, wParam, lParam);
    const bool popup = self->m_rdpFullscreenOverlayPopup == window;
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        const int hover = popup
            ? self->rdpFullscreenOverlayPopupButtonAt(point)
            : self->rdpFullscreenOverlayButtonAt(point);
        if (hover != self->m_rdpFullscreenOverlayHover) {
            self->m_rdpFullscreenOverlayHover = hover;
            self->m_rdpFullscreenOverlayHoverAt =
                !popup && hover >= 0 && hover < 6
                ? GetTickCount64() : 0;
            self->updateRdpFullscreenOverlay();
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->m_rdpFullscreenOverlayHover != -1) {
            self->m_rdpFullscreenOverlayHover = -1;
            self->m_rdpFullscreenOverlayHoverAt = 0;
            self->updateRdpFullscreenOverlay();
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam))};
        self->handleRdpFullscreenOverlayAction(
            popup ? self->rdpFullscreenOverlayPopupButtonAt(point)
                  : self->rdpFullscreenOverlayButtonAt(point));
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (popup)
            self->paintRdpFullscreenOverlayPopup(dc);
        else
            self->paintRdpFullscreenOverlay(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST:
        return popup && self->m_rdpFullscreenOverlayMenu == 0
            ? HTTRANSPARENT : HTCLIENT;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void WebViewWindow::showServerContextMenu(
    int profileIndex, int x, int y, bool canMoveUp, bool canMoveDown)
{
    if (!m_window || !m_host || profileIndex < 0)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);
    const UINT command = showNativePopupMenu(
        point.x, point.y,
        std::vector<NativePopupMenuItem>{
            {ServerContextConnect, L"连接"},
            {0, L"", true},
            {ServerContextEdit, L"编辑"},
            {ServerContextCopy, L"复制"},
            {0, L"", true},
            {ServerContextMoveUp, L"上移", false, !canMoveUp},
            {ServerContextMoveDown, L"下移", false, !canMoveDown},
            {0, L"", true},
            {ServerContextBatchWorkspace, L"批量设置工作区"},
            {ServerContextBatchColor, L"批量设置颜色"},
            {ServerContextBatchIcon, L"批量设置图标"},
            {ServerContextBatchDelete, L"批量删除"},
            {ServerContextDelete, L"删除"}},
        170);
    if (!command)
        return;

    const wchar_t *action = nullptr;
    switch (command) {
    case ServerContextConnect:
        action = L"connect";
        break;
    case ServerContextEdit:
        action = L"edit";
        break;
    case ServerContextCopy:
        action = L"copy";
        break;
    case ServerContextMoveUp:
        action = L"move-up";
        break;
    case ServerContextMoveDown:
        action = L"move-down";
        break;
    case ServerContextBatchWorkspace:
        action = L"batch-workspace";
        break;
    case ServerContextBatchColor:
        action = L"batch-color";
        break;
    case ServerContextBatchIcon:
        action = L"batch-icon";
        break;
    case ServerContextBatchDelete:
        action = L"batch-delete";
        break;
    case ServerContextDelete:
        action = L"delete";
        break;
    default:
        break;
    }
    if (!action)
        return;
    m_host->sendJsonToWebView(
        L"{\"event\":\"app.nativeServerContextAction\",\"payload\":{"
        L"\"index\":" + std::to_wstring(profileIndex)
        + L",\"action\":\"" + action + L"\"}}");
}

void WebViewWindow::showExistingConnectionsMenu(
    int x, int y, const std::vector<std::pair<int, std::string>> &items)
{
    if (!m_window || !m_host || items.empty())
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);
    std::vector<NativePopupMenuItem> menuItems;
    menuItems.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index) {
        if (index >= static_cast<size_t>(UINT_MAX - ExistingConnectionMenuCommand))
            break;
        menuItems.push_back({
            ExistingConnectionMenuCommand + static_cast<UINT>(index),
            utf8ToWide(items[index].second)});
    }
    if (menuItems.empty())
        return;
    const UINT command = showNativePopupMenu(
        point.x, point.y, std::move(menuItems), 230);
    if (command < ExistingConnectionMenuCommand)
        return;
    const size_t itemIndex = static_cast<size_t>(
        command - ExistingConnectionMenuCommand);
    if (itemIndex >= items.size())
        return;
    m_host->sendJsonToWebView(
        L"{\"event\":\"app.nativeOpenConnection\",\"payload\":{"
        L"\"index\":" + std::to_wstring(items[itemIndex].first)
        + L"}}");
}

void WebViewWindow::showRdpContextMenu(
    const std::string &sessionId, int x, int y)
{
    if (!m_window || !m_host || !findRdpSession(sessionId))
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);
    const UINT command = showNativePopupMenu(
        point.x, point.y,
        std::vector<NativePopupMenuItem>{
            {RdpContextReconnect, L"重新连接"},
            {RdpContextClose, L"关闭当前"},
            {RdpContextCloseOthers, L"关闭其它"},
            {RdpContextCloseAll, L"关闭全部"},
            {0, L"", true},
            {RdpContextMoveLeft, L"左移"},
            {RdpContextMoveRight, L"右移"},
            {0, L"", true},
            {RdpContextFullscreen,
             m_rdpFullscreen ? L"退出全屏" : L"全屏显示"}});
    if (!command)
        return;

    const wchar_t *action = nullptr;
    switch (command) {
    case RdpContextReconnect:
        action = L"reconnect";
        break;
    case RdpContextClose:
        action = L"close";
        break;
    case RdpContextCloseOthers:
        action = L"close-others";
        break;
    case RdpContextCloseAll:
        action = L"close-all";
        break;
    case RdpContextMoveLeft:
        action = L"move-left";
        break;
    case RdpContextMoveRight:
        action = L"move-right";
        break;
    case RdpContextFullscreen:
        action = L"fullscreen";
        break;
    default:
        break;
    }
    if (!action)
        return;
    m_host->sendJsonToWebView(
        L"{\"event\":\"rdp.contextAction\",\"payload\":{\"sessionId\":\""
        + utf8ToWide(sessionId)
        + L"\",\"action\":\"" + action + L"\"}}");
}

void WebViewWindow::showRdpTabsContextMenu(int x, int y, bool canCloseSplit)
{
    if (!m_window || !m_host)
        return;
    POINT point{x, y};
    if (!ClientToScreen(m_window, &point))
        GetCursorPos(&point);
    std::vector<NativePopupMenuItem> menuItems{
        {RdpTabsNewLocal, L"打开默认本地终端"},
        {RdpTabsNewLocalPwsh, L"PowerShell 7"},
        {RdpTabsNewLocalPowerShell, L"Windows PowerShell"},
        {RdpTabsNewLocalCmd, L"命令提示符 (CMD)"},
        {RdpTabsNewLocalWsl, L"WSL (Linux)"},
        {RdpTabsNewLocalGitBash, L"Git Bash"},
        {0, L"", true},
        {RdpTabsNewSsh, L"打开 SSH 终端…"},
        {RdpTabsNewRdp, L"打开远程桌面 (RDP)…"},
        {0, L"", true},
        {RdpTabsCloseAll, L"关闭全部"},
        {RdpTabsCloseSplit, L"关闭分屏", false, !canCloseSplit},
        {RdpTabsBatchCommand, L"批量执行命令…"},
        {0, L"", true},
        {RdpTabsSortName, L"标签按名称排序"},
        {RdpTabsSortType, L"标签按连接类型排序"}
    };
    const UINT command = showNativePopupMenu(point.x, point.y, std::move(menuItems), 210);
    if (!command)
        return;

    const wchar_t *action = nullptr;
    switch (command) {
    case RdpTabsNewLocal: action = L"new-local"; break;
    case RdpTabsNewLocalPwsh: action = L"new-local-pwsh"; break;
    case RdpTabsNewLocalPowerShell: action = L"new-local-powershell"; break;
    case RdpTabsNewLocalCmd: action = L"new-local-cmd"; break;
    case RdpTabsNewLocalWsl: action = L"new-local-wsl"; break;
    case RdpTabsNewLocalGitBash: action = L"new-local-gitbash"; break;
    case RdpTabsNewSsh: action = L"new-ssh"; break;
    case RdpTabsNewRdp: action = L"new-rdp"; break;
    case RdpTabsCloseAll: action = L"close-all"; break;
    case RdpTabsCloseSplit: action = L"split-close"; break;
    case RdpTabsBatchCommand: action = L"batch-command"; break;
    case RdpTabsSortName: action = L"sort-name"; break;
    case RdpTabsSortType: action = L"sort-type"; break;
    default: break;
    }
    if (!action)
        return;
    m_host->sendJsonToWebView(
        std::wstring(L"{\"event\":\"app.nativeTabsContextAction\",\"payload\":{\"action\":\"")
        + action + L"\"}}");
}
