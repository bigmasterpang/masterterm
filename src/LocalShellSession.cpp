#include "LocalShellSession.h"

#include <cstdlib>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <consoleapi.h>
#include <consoleapi3.h>
#include <io.h>
#endif

namespace {

std::wstring environmentShell()
{
    wchar_t buffer[1024]{};
    const DWORD length = GetEnvironmentVariableW(
        L"COMSPEC", buffer, static_cast<DWORD>(std::size(buffer)));
    return length > 0 && length < std::size(buffer)
        ? std::wstring(buffer, length) : L"C:\\Windows\\System32\\cmd.exe";
}

} // namespace

LocalShellSession::LocalShellSession() = default;

LocalShellSession::~LocalShellSession()
{
    stop();
}

void LocalShellSession::start(
    const std::wstring &shellExe,
    const std::wstring &commandLineArg,
    const std::wstring &workingDir)
{
    if (m_process != INVALID_HANDLE_VALUE)
        return;
    HANDLE ptyRead = INVALID_HANDLE_VALUE;
    HANDLE ptyWrite = INVALID_HANDLE_VALUE;
    HANDLE appRead = INVALID_HANDLE_VALUE;
    HANDLE appWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&ptyRead, &appWrite, nullptr, 0)
        || !CreatePipe(&appRead, &ptyWrite, nullptr, 0)) {
        m_errorString = "无法创建本地终端管道";
        if (m_errorHandler)
            m_errorHandler();
        return;
    }

    COORD size{120, 32};
    if (CreatePseudoConsole(
            size, ptyRead, ptyWrite, 0, &m_pseudoConsole) != S_OK) {
        m_errorString = "无法创建伪控制台（需要 Windows 10 1809+）";
        CloseHandle(ptyRead);
        CloseHandle(ptyWrite);
        CloseHandle(appRead);
        CloseHandle(appWrite);
        if (m_errorHandler)
            m_errorHandler();
        return;
    }
    CloseHandle(ptyRead);
    CloseHandle(ptyWrite);
    m_consoleInput = appWrite;
    m_consoleOutput = appRead;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // With a pseudoconsole attached, the system fills the standard handles
    // from the ConPTY session. Declaring them here (even as NULL) prevents
    // the child from inheriting the host process's real console handles.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(
        nullptr, 1, 0, &attributeSize);
    std::vector<BYTE> attributeBuffer(attributeSize);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(
            startup.lpAttributeList, 1, 0, &attributeSize)
        || !UpdateProcThreadAttribute(
            startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            m_pseudoConsole, sizeof(m_pseudoConsole), nullptr, nullptr)) {
        m_errorString = "无法初始化本地终端进程属性（错误 " +
            std::to_string(GetLastError()) + "）";
        stop();
        if (m_errorHandler)
            m_errorHandler();
        return;
    }

    const std::wstring shell = shellExe.empty() ? environmentShell() : shellExe;
    std::wstring commandLine;
    if (!commandLineArg.empty()) {
        commandLine = commandLineArg;
    } else {
        std::wstring lowerShell = shell;
        for (wchar_t &ch : lowerShell)
            ch = towlower(ch);
        if (lowerShell.find(L"powershell") != std::wstring::npos
            || lowerShell.find(L"pwsh") != std::wstring::npos) {
            commandLine = L"\"" + shell + L"\" -NoLogo";
        } else if (lowerShell.find(L"wsl") != std::wstring::npos) {
            commandLine = L"\"" + shell + L"\"";
        } else if (lowerShell.find(L"bash") != std::wstring::npos) {
            commandLine = L"\"" + shell + L"\" -l -i";
        } else {
            // Default cmd.exe bootstrap with UTF-8 support
            commandLine = L"\"" + shell
                + L"\" /d /q /k chcp 65001 >nul & set LANG=zh_CN.UTF-8"
                  L" & set LC_CTYPE=zh_CN.UTF-8 & set LC_ALL=zh_CN.UTF-8";
        }
    }
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');
    const wchar_t *cwdPtr = workingDir.empty() ? nullptr : workingDir.c_str();
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr, cwdPtr, &startup.StartupInfo, &processInfo);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            m_errorString = "未找到指定的 Shell 可执行文件，无法启动本地终端";
        } else {
            m_errorString = "无法启动本地终端进程（错误代码 " + std::to_string(err) + "）";
        }
        stop();
        if (m_errorHandler)
            m_errorHandler();
        return;
    }
    CloseHandle(processInfo.hThread);
    m_process = processInfo.hProcess;
    m_startedEmitted = false;
    m_finishedEmitted = false;
    m_errorString.clear();
    if (m_startedHandler)
        m_startedHandler();
}

void LocalShellSession::stop()
{
    if (m_process != INVALID_HANDLE_VALUE) {
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(m_process, &exitCode)
            && exitCode == STILL_ACTIVE)
            TerminateProcess(m_process, 1);
        WaitForSingleObject(m_process, 1000);
        CloseHandle(m_process);
        m_process = INVALID_HANDLE_VALUE;
    }
    if (m_pseudoConsole) {
        ClosePseudoConsole(m_pseudoConsole);
        m_pseudoConsole = nullptr;
    }
    if (m_consoleInput != INVALID_HANDLE_VALUE) {
        CloseHandle(m_consoleInput);
        m_consoleInput = INVALID_HANDLE_VALUE;
    }
    if (m_consoleOutput != INVALID_HANDLE_VALUE) {
        CloseHandle(m_consoleOutput);
        m_consoleOutput = INVALID_HANDLE_VALUE;
    }
}

void LocalShellSession::poll()
{
    if (m_consoleOutput == INVALID_HANDLE_VALUE)
        return;
    DWORD available = 0;
    if (PeekNamedPipe(
            m_consoleOutput, nullptr, 0, nullptr, &available, nullptr)
        && available > 0) {
        std::vector<char> buffer(static_cast<std::size_t>(available));
        DWORD readBytes = 0;
        if (ReadFile(
                m_consoleOutput, buffer.data(), available, &readBytes, nullptr)
            && readBytes > 0) {
            if (m_dataHandler) {
                m_dataHandler(std::string(
                    buffer.data(), static_cast<std::size_t>(readBytes)));
            }
        }
    }
    if (m_process != INVALID_HANDLE_VALUE && !isRunning()
        && !m_finishedEmitted) {
        m_finishedEmitted = true;
        DWORD exitCode = 0;
        GetExitCodeProcess(m_process, &exitCode);
        if (m_finishedHandler)
            m_finishedHandler(static_cast<int>(exitCode));
    }
}

void LocalShellSession::resize(int columns, int rows)
{
    if (!m_pseudoConsole)
        return;
    COORD size{
        static_cast<SHORT>(columns > 0 ? columns : 80),
        static_cast<SHORT>(rows > 0 ? rows : 24)};
    ResizePseudoConsole(m_pseudoConsole, size);
}

bool LocalShellSession::isRunning() const
{
    if (m_process == INVALID_HANDLE_VALUE)
        return false;
    DWORD exitCode = STILL_ACTIVE;
    return GetExitCodeProcess(m_process, &exitCode)
        && exitCode == STILL_ACTIVE;
}

DWORD LocalShellSession::processId() const
{
    if (m_process == INVALID_HANDLE_VALUE)
        return 0;
    return GetProcessId(m_process);
}

std::size_t LocalShellSession::write(const char *data, std::size_t size)
{
    if (m_consoleInput == INVALID_HANDLE_VALUE || !data || size == 0)
        return 0;
    DWORD written = 0;
    if (!WriteFile(m_consoleInput, data, static_cast<DWORD>(size),
                   &written, nullptr))
        return 0;
    return static_cast<std::size_t>(written);
}

void LocalShellSession::setStartedHandler(std::function<void()> handler)
{
    m_startedHandler = std::move(handler);
}

void LocalShellSession::setErrorHandler(std::function<void()> handler)
{
    m_errorHandler = std::move(handler);
}

void LocalShellSession::setFinishedHandler(
    std::function<void(int)> handler)
{
    m_finishedHandler = std::move(handler);
}

void LocalShellSession::setDataHandler(
    std::function<void(const std::string &)> handler)
{
    m_dataHandler = std::move(handler);
}
