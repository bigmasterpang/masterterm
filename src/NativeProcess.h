#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NativeProcess final
{
public:
    NativeProcess() = default;
    ~NativeProcess();

    NativeProcess(const NativeProcess &) = delete;
    NativeProcess &operator=(const NativeProcess &) = delete;

    bool start(const std::wstring &executable,
               const std::vector<std::wstring> &arguments,
               const std::map<std::wstring, std::wstring> &environmentOverrides,
               bool enableStandardInput = false);
    void terminate();

    std::string takeStandardOutput();
    std::string takeStandardError();
    bool isFinished(DWORD &exitCode);
    bool write(const std::string &bytes);
    void closeInput();
    std::uint64_t bufferedInputBytes() const;
    const std::wstring &errorMessage() const { return m_errorMessage; }

private:
    static std::wstring quoteArgument(const std::wstring &argument);
    static std::vector<wchar_t> createEnvironmentBlock(
        const std::map<std::wstring, std::wstring> &overrides);
    static void closeHandle(HANDLE &handle);
    static void readAvailable(HANDLE pipe, std::string &target);
    void writerLoop();
    void closeProcessHandles();

    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    HANDLE m_standardOutput = nullptr;
    HANDLE m_standardError = nullptr;
    HANDLE m_standardInput = nullptr;
    bool m_finished = false;
    DWORD m_exitCode = 0;
    std::wstring m_errorMessage;

    std::thread m_writer;
    mutable std::mutex m_writeMutex;
    std::condition_variable m_writeCondition;
    std::string m_pendingInput;
    std::atomic<std::uint64_t> m_bufferedInput{0};
    bool m_inputEnabled = false;
    bool m_inputClosed = true;
    bool m_closeInputRequested = false;
    bool m_stopWriter = false;
};
