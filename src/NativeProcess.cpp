#include "NativeProcess.h"

#include "DiagnosticLog.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace {

struct CaseInsensitiveLess
{
    bool operator()(const std::wstring &left, const std::wstring &right) const
    {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](wchar_t a, wchar_t b) {
                return std::towlower(a) < std::towlower(b);
            });
    }
};

std::wstring windowsErrorMessage(DWORD error)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring message =
        length > 0 && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer)
        LocalFree(buffer);
    while (!message.empty()
           && (message.back() == L'\r' || message.back() == L'\n'
               || message.back() == L' '))
        message.pop_back();
    if (message.empty())
        message = L"Windows error " + std::to_wstring(error);
    return message;
}

} // namespace

NativeProcess::~NativeProcess()
{
    terminate();
    closeProcessHandles();
}

bool NativeProcess::start(
    const std::wstring &executable,
    const std::vector<std::wstring> &arguments,
    const std::map<std::wstring, std::wstring> &environmentOverrides,
    bool enableStandardInput)
{
    if (m_process) {
        m_errorMessage = L"Process already started";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE childOutput = nullptr;
    HANDLE childError = nullptr;
    HANDLE childInput = nullptr;
    if (!CreatePipe(&m_standardOutput, &childOutput, &security, 0)
        || !SetHandleInformation(m_standardOutput, HANDLE_FLAG_INHERIT, 0)
        || !CreatePipe(&m_standardError, &childError, &security, 0)
        || !SetHandleInformation(m_standardError, HANDLE_FLAG_INHERIT, 0)
        || !CreatePipe(&childInput, &m_standardInput, &security, 0)
        || !SetHandleInformation(
            m_standardInput, HANDLE_FLAG_INHERIT, 0)) {
        m_errorMessage = windowsErrorMessage(GetLastError());
        closeHandle(childOutput);
        closeHandle(childError);
        closeHandle(childInput);
        closeProcessHandles();
        return false;
    }

    std::wstring commandLine = quoteArgument(executable);
    for (const std::wstring &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    std::vector<wchar_t> environment =
        createEnvironmentBlock(environmentOverrides);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = childOutput;
    startup.hStdError = childError;
    startup.hStdInput = childInput;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
        nullptr, &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    closeHandle(childOutput);
    closeHandle(childError);
    closeHandle(childInput);
    if (!created) {
        m_errorMessage = windowsErrorMessage(createError);
        closeProcessHandles();
        return false;
    }

    m_process = process.hProcess;
    m_thread = process.hThread;
    {
        const std::filesystem::path path(executable);
        std::string name = path.filename().string();
        if (name.empty())
            name = "worker";
        DiagnosticLog::write("worker-start", name);
    }
    if (enableStandardInput) {
        {
            std::lock_guard lock(m_writeMutex);
            m_inputEnabled = true;
            m_inputClosed = false;
        }
        m_writer = std::thread(&NativeProcess::writerLoop, this);
    } else {
        closeHandle(m_standardInput);
    }
    return true;
}

void NativeProcess::terminate()
{
    if (m_process && !m_finished) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(m_process, &exitCode)
            && exitCode == STILL_ACTIVE) {
            DiagnosticLog::write("worker-terminate");
            TerminateProcess(m_process, ERROR_CANCELLED);
            WaitForSingleObject(m_process, 1000);
        }
    }
    {
        std::lock_guard lock(m_writeMutex);
        m_stopWriter = true;
        m_pendingInput.clear();
    }
    m_writeCondition.notify_all();
    if (m_writer.joinable())
        m_writer.join();
    m_bufferedInput = 0;
    closeHandle(m_standardInput);
}

std::string NativeProcess::takeStandardOutput()
{
    std::string result;
    readAvailable(m_standardOutput, result);
    return result;
}

std::string NativeProcess::takeStandardError()
{
    std::string result;
    readAvailable(m_standardError, result);
    return result;
}

bool NativeProcess::isFinished(DWORD &exitCode)
{
    if (!m_process)
        return false;
    if (!m_finished) {
        DWORD current = STILL_ACTIVE;
        if (!GetExitCodeProcess(m_process, &current) || current == STILL_ACTIVE)
            return false;
        m_finished = true;
        m_exitCode = current;
    }
    exitCode = m_exitCode;
    return true;
}

bool NativeProcess::write(const std::string &bytes)
{
    if (bytes.empty())
        return bytes.empty();
    {
        std::lock_guard lock(m_writeMutex);
        if (!m_inputEnabled || m_inputClosed
            || m_closeInputRequested || m_stopWriter)
            return false;
        m_pendingInput.append(bytes);
        m_bufferedInput += bytes.size();
    }
    m_writeCondition.notify_one();
    return true;
}

void NativeProcess::closeInput()
{
    {
        std::lock_guard lock(m_writeMutex);
        m_closeInputRequested = true;
    }
    m_writeCondition.notify_one();
}

std::uint64_t NativeProcess::bufferedInputBytes() const
{
    return m_bufferedInput.load();
}

std::wstring NativeProcess::quoteArgument(const std::wstring &argument)
{
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::vector<wchar_t> NativeProcess::createEnvironmentBlock(
    const std::map<std::wstring, std::wstring> &overrides)
{
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t *entry = block; *entry;) {
            const std::wstring value(entry);
            const size_t separator = value.find(
                L'=', value.empty() || value.front() != L'=' ? 0 : 1);
            if (separator != std::wstring::npos)
                variables[value.substr(0, separator)] =
                    value.substr(separator + 1);
            entry += value.size() + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto &[key, value] : overrides)
        variables[key] = value;

    std::vector<wchar_t> result;
    for (const auto &[key, value] : variables) {
        const std::wstring entry = key + L"=" + value;
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

void NativeProcess::closeHandle(HANDLE &handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    handle = nullptr;
}

void NativeProcess::readAvailable(HANDLE pipe, std::string &target)
{
    if (!pipe)
        return;
    char buffer[64 * 1024];
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)
            || available == 0)
            return;
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(
            available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, requested, &read, nullptr) || read == 0)
            return;
        target.append(buffer, read);
    }
}

void NativeProcess::writerLoop()
{
    DiagnosticLog::registerThread("worker-stdin-writer");
    while (true) {
        std::string chunk;
        {
            std::unique_lock lock(m_writeMutex);
            m_writeCondition.wait(lock, [this] {
                return m_stopWriter || !m_pendingInput.empty()
                    || m_closeInputRequested;
            });
            if (m_stopWriter)
                break;
            if (m_pendingInput.empty()) {
                if (m_closeInputRequested)
                    break;
                continue;
            }
            chunk.swap(m_pendingInput);
        }

        size_t offset = 0;
        while (offset < chunk.size()) {
            DWORD written = 0;
            const DWORD requested = static_cast<DWORD>(
                std::min<size_t>(chunk.size() - offset, 64 * 1024));
            if (!WriteFile(m_standardInput, chunk.data() + offset,
                           requested, &written, nullptr)
                || written == 0) {
                m_bufferedInput -= chunk.size() - offset;
                offset = chunk.size();
                std::lock_guard lock(m_writeMutex);
                m_stopWriter = true;
                m_inputClosed = true;
                break;
            }
            offset += written;
            m_bufferedInput -= written;
        }
    }
    closeHandle(m_standardInput);
    {
        std::lock_guard lock(m_writeMutex);
        m_inputClosed = true;
    }
}

void NativeProcess::closeProcessHandles()
{
    closeHandle(m_standardOutput);
    closeHandle(m_standardError);
    closeHandle(m_standardInput);
    closeHandle(m_thread);
    closeHandle(m_process);
}
