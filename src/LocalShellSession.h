#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// Local shell session backed by the Windows pseudo console (ConPTY).  It
// exposes the same data/state callbacks as the SSH and serial sessions so the
// WebView backend can drive it with the existing terminal message flow.
class LocalShellSession final
{
public:
    LocalShellSession();
    ~LocalShellSession();

    LocalShellSession(const LocalShellSession &) = delete;
    LocalShellSession &operator=(const LocalShellSession &) = delete;

    void start(const std::wstring &shellExe = {},
               const std::wstring &commandLine = {},
               const std::wstring &workingDir = {});
    void stop();
    void poll();
    void resize(int columns, int rows);
    bool isRunning() const;
    const std::string &errorString() const { return m_errorString; }
    DWORD processId() const;
    std::size_t write(const char *data, std::size_t size);
    void setStartedHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void()> handler);
    void setFinishedHandler(std::function<void(int)> handler);
    void setDataHandler(std::function<void(const std::string &)> handler);

private:
    HANDLE m_consoleInput = INVALID_HANDLE_VALUE;   // app writes here
    HANDLE m_consoleOutput = INVALID_HANDLE_VALUE;  // app reads here
    HPCON m_pseudoConsole = nullptr;
    HANDLE m_process = INVALID_HANDLE_VALUE;
    std::string m_errorString;
    bool m_startedEmitted = false;
    bool m_finishedEmitted = false;
    std::function<void()> m_startedHandler;
    std::function<void()> m_errorHandler;
    std::function<void(int)> m_finishedHandler;
    std::function<void(const std::string &)> m_dataHandler;
};
