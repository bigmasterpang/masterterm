#include "SerialSession.h"

#include "DiagnosticLog.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <utility>

namespace {

std::string utf8FromWide(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string formatError(
    std::wstring_view prefix, std::wstring_view portName, DWORD error = 0)
{
    std::wstring message(prefix);
    message.append(portName);
    if (error != 0) {
        message.append(L"（Windows 错误 ");
        message.append(std::to_wstring(error));
        message.append(L"）");
    }
    message.push_back(L'。');
    return utf8FromWide(message);
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return value;
}

std::wstring normalizedPortName(std::wstring value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), std::iswspace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), std::iswspace).base();
    value = first < last ? std::wstring(first, last) : std::wstring();
    std::transform(value.begin(), value.end(), value.begin(), std::towupper);
    return value;
}

} // namespace

SerialSession::~SerialSession()
{
    stop();
}

bool SerialSession::start(
    std::wstring portName, int baudRate, int dataBits,
    std::string parity, std::string stopBits, std::string flowControl)
{
    if (m_opening.load(std::memory_order_acquire))
        return false;

    stop();
    {
        std::scoped_lock lock(m_eventMutex);
        m_events.clear();
    }
    {
        std::scoped_lock lock(m_stateMutex);
        m_portName = normalizedPortName(std::move(portName));
        m_baudRate = std::max(1, baudRate);
        m_dataBits = std::clamp(dataBits, 5, 8);
        m_parity = lowerAscii(std::move(parity));
        m_stopBits = std::move(stopBits);
        m_flowControl = lowerAscii(std::move(flowControl));
        m_errorString.clear();
        if (m_portName.empty()) {
            m_errorString = utf8FromWide(L"请选择串口。");
            queueEvent(EventType::Error, m_errorString);
            return false;
        }
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_opening.store(true, std::memory_order_release);
    m_worker = std::thread(&SerialSession::workerMain, this);
    return true;
}

void SerialSession::reconnect()
{
    std::wstring port;
    std::string parity;
    std::string stopBits;
    std::string flowControl;
    int baudRate = 0;
    int dataBits = 0;
    {
        std::scoped_lock lock(m_stateMutex);
        port = m_portName;
        baudRate = m_baudRate;
        dataBits = m_dataBits;
        parity = m_parity;
        stopBits = m_stopBits;
        flowControl = m_flowControl;
    }
    start(std::move(port), baudRate, dataBits, std::move(parity),
          std::move(stopBits), std::move(flowControl));
}

void SerialSession::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    if (m_worker.joinable())
        m_worker.join();
    closeHandle();
    m_opening.store(false, std::memory_order_release);
    m_connected.store(false, std::memory_order_release);
}

void SerialSession::poll()
{
    std::deque<Event> events;
    {
        std::scoped_lock lock(m_eventMutex);
        events.swap(m_events);
    }
    for (Event &event : events) {
        switch (event.type) {
        case EventType::Started:
            if (m_startedHandler)
                m_startedHandler();
            break;
        case EventType::Error:
            if (m_errorHandler)
                m_errorHandler(event.data);
            break;
        case EventType::Data:
            if (m_dataHandler)
                m_dataHandler(event.data);
            break;
        }
    }
}

bool SerialSession::isConnected() const noexcept
{
    return m_connected.load(std::memory_order_acquire);
}

bool SerialSession::isConnecting() const noexcept
{
    return m_opening.load(std::memory_order_acquire);
}

std::string SerialSession::errorString() const
{
    std::scoped_lock lock(m_stateMutex);
    return m_errorString;
}

std::wstring SerialSession::portName() const
{
    std::scoped_lock lock(m_stateMutex);
    return m_portName;
}

std::size_t SerialSession::write(const char *data, std::size_t size)
{
    if (!data || size == 0 || !isConnected())
        return 0;
    DWORD error = 0;
    DWORD written = 0;
    std::wstring portName;
    {
        std::scoped_lock lock(m_stateMutex);
        if (!m_handle)
            return 0;
        portName = m_portName;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(size, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(
                static_cast<HANDLE>(m_handle), data, requested,
                &written, nullptr)) {
            error = GetLastError();
        }
    }
    if (error != 0) {
        m_stopRequested.store(true, std::memory_order_release);
        setFailure(formatError(L"写入串口 ", portName, error));
        return 0;
    }
    return static_cast<std::size_t>(written);
}

void SerialSession::setStartedHandler(std::function<void()> handler)
{
    m_startedHandler = std::move(handler);
}

void SerialSession::setErrorHandler(
    std::function<void(const std::string &)> handler)
{
    m_errorHandler = std::move(handler);
}

void SerialSession::setDataHandler(
    std::function<void(const std::string &)> handler)
{
    m_dataHandler = std::move(handler);
}

std::vector<std::wstring> SerialSession::availablePorts()
{
    std::vector<std::wstring> ports;
    std::array<wchar_t, 1024> target{};
    for (int index = 1; index <= 256; ++index) {
        const std::wstring name = L"COM" + std::to_wstring(index);
        if (QueryDosDeviceW(
                name.c_str(), target.data(),
                static_cast<DWORD>(target.size())) != 0) {
            ports.push_back(name);
        }
    }
    return ports;
}

void SerialSession::workerMain()
{
    DiagnosticLog::registerThread("serial");
    std::wstring portName;
    int baudRate = 0;
    int dataBits = 0;
    std::string parity;
    std::string stopBits;
    std::string flowControl;
    {
        std::scoped_lock lock(m_stateMutex);
        portName = m_portName;
        baudRate = m_baudRate;
        dataBits = m_dataBits;
        parity = m_parity;
        stopBits = m_stopBits;
        flowControl = m_flowControl;
    }

    const std::wstring devicePath = portName.rfind(L"\\\\.\\", 0) == 0
        ? portName : L"\\\\.\\" + portName;
    HANDLE handle = CreateFileW(
        devicePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        setFailure(formatError(L"无法打开串口 ", portName, GetLastError()));
        return;
    }
    const auto failOpen = [&](std::wstring_view message, DWORD error = 0) {
        CloseHandle(handle);
        setFailure(formatError(message, portName, error));
    };

    SetupComm(handle, 64 * 1024, 64 * 1024);
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(handle, &dcb)) {
        failOpen(L"无法读取串口 ");
        return;
    }
    dcb.BaudRate = static_cast<DWORD>(baudRate);
    dcb.ByteSize = static_cast<BYTE>(dataBits);
    dcb.fBinary = TRUE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (parity == "even") {
        dcb.Parity = EVENPARITY;
        dcb.fParity = TRUE;
    } else if (parity == "odd") {
        dcb.Parity = ODDPARITY;
        dcb.fParity = TRUE;
    } else if (parity == "mark") {
        dcb.Parity = MARKPARITY;
        dcb.fParity = TRUE;
    } else if (parity == "space") {
        dcb.Parity = SPACEPARITY;
        dcb.fParity = TRUE;
    } else {
        dcb.Parity = NOPARITY;
        dcb.fParity = FALSE;
    }
    dcb.StopBits = stopBits == "2" ? TWOSTOPBITS
        : stopBits == "1.5" ? ONE5STOPBITS : ONESTOPBIT;
    if (flowControl == "hardware") {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    } else if (flowControl == "software") {
        dcb.fOutX = TRUE;
        dcb.fInX = TRUE;
    }
    if (!SetCommState(handle, &dcb)) {
        failOpen(L"无法应用串口 ", GetLastError());
        return;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = 250;
    if (!SetCommTimeouts(handle, &timeouts)) {
        failOpen(L"无法设置串口 ", GetLastError());
        return;
    }
    PurgeComm(handle, PURGE_TXABORT | PURGE_TXCLEAR);

    if (m_stopRequested.load(std::memory_order_acquire)) {
        CloseHandle(handle);
        m_opening.store(false, std::memory_order_release);
        return;
    }
    {
        std::scoped_lock lock(m_stateMutex);
        m_handle = handle;
    }
    m_opening.store(false, std::memory_order_release);
    m_connected.store(true, std::memory_order_release);
    queueEvent(EventType::Started);

    std::array<char, 16 * 1024> buffer{};
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        DWORD errors = 0;
        COMSTAT status{};
        if (!ClearCommError(handle, &errors, &status)) {
            setFailure(formatError(
                L"读取串口状态失败：", portName, GetLastError()));
            break;
        }
        if (status.cbInQue != 0) {
            const DWORD requested = std::min<DWORD>(
                status.cbInQue, static_cast<DWORD>(buffer.size()));
            DWORD count = 0;
            if (!ReadFile(handle, buffer.data(), requested, &count, nullptr)) {
                setFailure(formatError(
                    L"读取串口失败：", portName, GetLastError()));
                break;
            }
            if (count != 0)
                queueEvent(EventType::Data, std::string(buffer.data(), count));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    {
        std::scoped_lock lock(m_stateMutex);
        if (m_handle == handle)
            m_handle = nullptr;
    }
    CloseHandle(handle);
    m_connected.store(false, std::memory_order_release);
    m_opening.store(false, std::memory_order_release);
}

void SerialSession::queueEvent(EventType type, std::string data)
{
    std::scoped_lock lock(m_eventMutex);
    m_events.push_back({type, std::move(data)});
}

void SerialSession::setFailure(std::string message)
{
    {
        std::scoped_lock lock(m_stateMutex);
        m_errorString = message;
    }
    m_connected.store(false, std::memory_order_release);
    m_opening.store(false, std::memory_order_release);
    queueEvent(EventType::Error, std::move(message));
}

void SerialSession::closeHandle()
{
    std::scoped_lock lock(m_stateMutex);
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}
