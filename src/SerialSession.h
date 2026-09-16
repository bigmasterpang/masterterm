#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Win32 serial session with a main-thread polling boundary.
//
// Port opening and reads happen on the worker thread. poll() dispatches all
// callbacks on the caller (WebView backend) thread, so frontend state remains
// single-threaded without relying on a framework event dispatcher.
class SerialSession final
{
public:
    SerialSession() = default;
    ~SerialSession();

    SerialSession(const SerialSession &) = delete;
    SerialSession &operator=(const SerialSession &) = delete;

    bool start(std::wstring portName, int baudRate, int dataBits,
               std::string parity, std::string stopBits,
               std::string flowControl);
    void reconnect();
    void stop();
    void poll();

    bool isConnected() const noexcept;
    bool isConnecting() const noexcept;
    std::string errorString() const;
    std::wstring portName() const;
    std::size_t write(const char *data, std::size_t size);

    void setStartedHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void(const std::string &)> handler);
    void setDataHandler(std::function<void(const std::string &)> handler);

    static std::vector<std::wstring> availablePorts();

private:
    enum class EventType {
        Started,
        Error,
        Data
    };

    struct Event {
        EventType type;
        std::string data;
    };

    void workerMain();
    void queueEvent(EventType type, std::string data = {});
    void setFailure(std::string message);
    void closeHandle();

    mutable std::mutex m_stateMutex;
    std::mutex m_eventMutex;
    std::deque<Event> m_events;
    std::thread m_worker;
    std::wstring m_portName;
    std::string m_errorString;
    int m_baudRate = 115200;
    int m_dataBits = 8;
    std::string m_parity = "none";
    std::string m_stopBits = "1";
    std::string m_flowControl = "none";
    void *m_handle = nullptr;
    std::function<void()> m_startedHandler;
    std::function<void(const std::string &)> m_errorHandler;
    std::function<void(const std::string &)> m_dataHandler;
    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_opening{false};
    std::atomic_bool m_connected{false};
};
