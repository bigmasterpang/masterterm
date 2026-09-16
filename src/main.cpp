#include "WebViewWindow.h"
#include "WebViewHost.h"
#include "NativeDataDir.h"
#include "DiagnosticLog.h"

#include <objbase.h>
#include <windows.h>

#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path crashLogPath()
{
    const std::filesystem::path directory =
        NativeDataDir::sessionLogDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error ? std::filesystem::path() : directory / L"crash.log";
}

std::string osVersionText()
{
    // RtlGetVersion is the only reliable way to get the real OS version on
    // modern Windows (GetVersionEx is manifest-dependent).
    using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!rtlGetVersion || rtlGetVersion(&info) != 0)
        return "unknown";
    char text[96]{};
    std::snprintf(
        text, sizeof(text), "%lu.%lu.%lu (build %lu)",
        static_cast<unsigned long>(info.dwMajorVersion),
        static_cast<unsigned long>(info.dwMinorVersion),
        static_cast<unsigned long>(info.dwBuildNumber),
        static_cast<unsigned long>(info.dwBuildNumber));
    return text;
}

std::string readRegistryString(
    HKEY root, const wchar_t *path, const wchar_t *value)
{
    wchar_t buffer[128]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    const LSTATUS status = RegGetValueW(
        root, path, value, RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS)
        return {};
    const std::wstring wide(buffer, (size / sizeof(wchar_t)) - 1);
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::string webView2RuntimeVersion()
{
    constexpr wchar_t WebView2ClientKey[] =
        L"Software\\Microsoft\\EdgeUpdate\\Clients\\"
        L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    std::string version = readRegistryString(
        HKEY_CURRENT_USER, WebView2ClientKey, L"pv");
    if (version.empty())
        version = readRegistryString(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\WOW6432Node\\" L"Microsoft\\EdgeUpdate\\Clients\\"
            L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"pv");
    if (version.empty())
        version = readRegistryString(
            HKEY_LOCAL_MACHINE, WebView2ClientKey, L"pv");
    return version.empty() ? std::string("unknown") : version;
}

std::filesystem::path legacyCrashLogPath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer)
        / L"MasterTerm" / L"logs" / L"crash.log";
}

std::filesystem::path defaultCrashLogDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer) / L"MasterSSH" / L"logs";
}

bool equalWindowsPath(
    const std::filesystem::path &left,
    const std::filesystem::path &right)
{
    const std::wstring normalizedLeft = left.lexically_normal().wstring();
    const std::wstring normalizedRight = right.lexically_normal().wstring();
    return CompareStringOrdinal(
        normalizedLeft.c_str(), -1,
        normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void prepareCrashLogging()
{
    // 0.1.18 and older wrote fatal crashes under LOCALAPPDATA while the
    // Settings button opened APPDATA\MasterSSH\logs. Move the existing log
    // into the unified data directory once, before installing the handler,
    // so both historical and future crashes are visible from Settings.
    const std::filesystem::path target = crashLogPath();
    const std::filesystem::path legacy = legacyCrashLogPath();
    const std::filesystem::path defaultDirectory =
        defaultCrashLogDirectory();
    // A portable/custom data root is intentionally isolated. Never copy a
    // machine-wide legacy log into it just because crash.log is absent.
    if (target.empty() || legacy.empty() || defaultDirectory.empty()
        || !equalWindowsPath(target.parent_path(), defaultDirectory)
        || equalWindowsPath(target, legacy))
        return;
    std::error_code error;
    if (!std::filesystem::exists(target, error)
        && std::filesystem::exists(legacy, error)) {
        std::filesystem::copy_file(
            legacy, target, std::filesystem::copy_options::none, error);
    }
}

void writeSymbolicAddress(std::ofstream &out, std::uintptr_t address)
{
    out << "0x" << std::hex << address;
    HMODULE module = nullptr;
    if (address != 0 && GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module)
        && module) {
        char path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        const char *name = path;
        if (length > 0 && length < MAX_PATH) {
            if (const char *separator = std::strrchr(path, '\\'))
                name = separator + 1;
        } else {
            name = "unknown-module";
        }
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
        out << '(' << name << "+0x" << (address - base) << ')';
    }
}

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS *info)
{
    try {
        const std::filesystem::path path = crashLogPath();
        if (path.empty())
            return EXCEPTION_CONTINUE_SEARCH;
        std::ofstream out(path, std::ios::app);
        if (!out)
            return EXCEPTION_CONTINUE_SEARCH;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[64]{};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const std::uintptr_t address = info
            ? reinterpret_cast<std::uintptr_t>(
                info->ExceptionRecord->ExceptionAddress)
            : 0;
        const DWORD code = info ? info->ExceptionRecord->ExceptionCode : 0;
        out << stamp << " MasterTerm " << MASTERTERM_VERSION
            << " crash code=0x" << std::hex << code << " address=";
        writeSymbolicAddress(out, address);
        out << std::dec << '\n';
        // Locate the fault: thread, last phase and active session recorded
        // by the lifecycle instrumentation.
        const DWORD threadId = GetCurrentThreadId();
        const std::string name = DiagnosticLog::threadName(threadId);
        const DiagnosticLog::CrashContext &context =
            DiagnosticLog::crashContext();
        out << "  thread=" << threadId;
        if (!name.empty())
            out << " (" << name << ")";
        out << " phase=" << (context.phase[0] ? context.phase : "-")
            << " session=" << (context.session[0] ? context.session : "-")
            << '\n';
        if (info && info->ContextRecord) {
            CONTEXT context = *info->ContextRecord;
            out << "  rip=0x" << std::hex
                << context.Rip << " rsp=0x" << context.Rsp
                << " frames=";
            // Unwind using the image's native x64 unwind metadata. Reading
            // arbitrary RSP slots here is unsafe when the fault itself is a
            // stack/callback corruption, so use RtlVirtualUnwind instead.
            for (int index = 0; index < 24 && context.Rip != 0; ++index) {
                if (index != 0)
                    out << ',';
                writeSymbolicAddress(
                    out, static_cast<std::uintptr_t>(context.Rip));
                PRUNTIME_FUNCTION function = nullptr;
                DWORD64 imageBase = 0;
                function = RtlLookupFunctionEntry(
                    context.Rip, &imageBase, nullptr);
                if (!function)
                    break;
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                KNONVOLATILE_CONTEXT_POINTERS nonVolatile{};
                RtlVirtualUnwind(
                    UNW_FLAG_NHANDLER, imageBase, context.Rip,
                    function, &context, &handlerData,
                    &establisherFrame, &nonVolatile);
            }
            out << std::dec << '\n';
        }
        out.flush();
        DiagnosticLog::dumpRingTo(out);
        char crashDetail[128]{};
        std::snprintf(
            crashDetail, sizeof(crashDetail),
            "code=0x%08lX address=0x%llX",
            static_cast<unsigned long>(code),
            static_cast<unsigned long long>(address));
        // Keep the compact diagnostic line consistent with crash.log.  The
        // previous implementation appended a decimal code after the 0x
        // prefix (for example 0x3765269347), which made a C++ exception look
        // like an unknown Win32 status and hid the useful exception class.
        DiagnosticLog::writeCrashLine("crash", crashDetail);
    } catch (...) {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    prepareCrashLogging();
    SetUnhandledExceptionFilter(handleUnhandledException);
    DiagnosticLog::registerThread("main");
    DiagnosticLog::setPhase("startup");
    DiagnosticLog::write(
        "app-start",
        "version=" MASTERTERM_VERSION " os=" + osVersionText()
            + " webview2=" + webView2RuntimeVersion());

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return static_cast<int>(comResult);
    const bool comInitialized = SUCCEEDED(comResult);

    int result = 1;
    {
        WebViewWindow window;
        // Hidden diagnostic flag driven by tools/connect-exit-stress.ps1.
        // Runs one SSH connect + SFTP worker + ConPTY cycle and exits with
        // 0 (pass), 10 (no usable SSH profile) or 11 (failed).
        if (std::wstring_view(commandLine).find(L"--exit-self-test")
            != std::wstring_view::npos)
            window.enableExitSelfTest();
        if (window.create(instance, showCommand)) {
            MSG message{};
            while (true) {
                const BOOL status = GetMessageW(&message, nullptr, 0, 0);
                if (status <= 0) {
                    result = status == 0
                        ? static_cast<int>(message.wParam)
                        : static_cast<int>(GetLastError());
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    if (comInitialized)
        CoUninitialize();
    return result;
}
