#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class SshProxyRelay;

struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_KNOWNHOSTS;

class SshSession final
{
public:
    SshSession();
    ~SshSession();

    // When `command` is non-empty the session runs it directly on the remote
    // host through an SSH "exec" channel (no PTY, no interactive shell) and
    // forwards stdin/stdout/stderr. Otherwise an interactive PTY shell opens.
    void start(std::string_view connection, std::string_view port,
               std::string_view password, std::string_view privateKeyPath,
               std::string_view proxyJump, std::string_view proxyPassword = {},
               std::string_view proxyKeyPath = {},
               std::string_view command = {},
               std::string_view passphrase = {});
    void reconnect();
    bool canReconnect() const { return m_reconnectAllowed; }
    _LIBSSH2_SESSION *sessionHandle() const { return m_session; }
    void poll();
    void resize(int columns, int rows);
    bool isRunning() const;
    const std::string &errorString() const { return m_errorString; }
    std::size_t write(const char *data, std::size_t size);
    std::size_t write(std::string_view data)
    {
        return write(data.data(), data.size());
    }
    // Signal end-of-input on the channel (stdin EOF). Used by exec mode so a
    // remote command that reads stdin (cat, sort, ...) sees EOF and exits.
    void sendEof();
    void setStartedHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void()> handler);
    void setFinishedHandler(std::function<void(int)> handler);
    void setDataHandler(std::function<void(const std::string &)> handler);
    // Optional: when set, channel stream 1 (stderr) is routed here instead of
    // the shared data handler, so exec mode can keep the two streams separate.
    void setStderrHandler(std::function<void(const std::string &)> handler);
    void setKeepaliveInterval(int seconds);
    // Fired when the remote host key changed since the last connection.
    // The connection pauses until confirmHostKey() or abortHostKey() runs.
    void setHostKeyMismatchHandler(std::function<void()> handler);
    bool hostKeyPending() const { return m_hostKeyPending; }
    const std::string &hostKeyMismatchHost() const { return m_hostKeyHost; }
    const std::string &hostKeyOldFingerprint() const { return m_oldFingerprint; }
    const std::string &hostKeyNewFingerprint() const { return m_newFingerprint; }
    bool confirmHostKey();
    void abortHostKey();

private:
    enum class State { Idle, TcpConnecting, Handshake, Authenticate, OpenChannel, RequestPty, SetLocale, StartShell, Running, Failed, AwaitHostKeyConfirm };

    void openTcpSocket();
    void pollResolver();
    void cleanupResolver();
    void openProxySocket(std::uint16_t localPort, const std::string &error);
    void appendResolvedAddress(
        int family, const void *address, std::size_t addressSize,
        std::uint16_t port);
    void openNextTcpSocket();
    bool pollTcpConnection();
    bool verifyHostKey();
    std::filesystem::path knownHostsPath() const;
    void advanceConnection();
    void flushInput();
    void readChannel(int stream);
    void finishSession(int exitCode);
    void fail(std::string message);
    void cleanup();
    std::string lastSessionError() const;
    std::string_view connectionPhase() const;
    bool retryLater(int result) const;

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_connectionStarted{};
    Clock::time_point m_keepaliveStarted{};
    bool m_connectionTimerActive = false;
    bool m_keepaliveTimerActive = false;
    std::size_t m_localeIndex = 0;
    std::string m_pendingWrite;
    bool m_eofPending = false;
    std::string m_errorString;
    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_passphrase;
    std::string m_privateKeyPath;
    int m_port = 22;
    int m_socketPort = 22;
    int m_columns = 80;
    int m_rows = 24;
    int m_pendingColumns = 0;
    int m_pendingRows = 0;
    State m_state = State::Idle;
    bool m_finishedEmitted = false;
    bool m_reconnectAllowed = false;
    std::string m_connection;
    std::string m_portText;
    std::string m_proxyJump;
    std::string m_proxyPassword;
    std::string m_proxyKeyPath;
    std::string m_command;
    std::unique_ptr<SshProxyRelay> m_proxyRelay;
    struct ResolverState;
    std::unique_ptr<ResolverState> m_resolverState;
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_CHANNEL *m_channel = nullptr;
    _LIBSSH2_KNOWNHOSTS *m_knownHosts = nullptr;
    std::uintptr_t m_socket = ~std::uintptr_t(0);
    struct SocketAddress {
        alignas(std::max_align_t) std::array<unsigned char, 128> bytes{};
        int length = 0;
        int family = 0;
    };
    std::vector<SocketAddress> m_resolvedAddresses;
    int m_nextAddressIndex = 0;
    int m_lastTcpError = 0;
    int m_keepaliveIntervalSeconds = 30;
    struct ProxyResult {
        std::uint16_t localPort = 0;
        std::string error;
        std::uint64_t generation = 0;
        bool ready = false;
    };
    std::mutex m_proxyResultMutex;
    ProxyResult m_proxyResult;
    std::uint64_t m_attemptGeneration = 0;
    bool m_errorNotificationPending = false;
    bool m_hostKeyPending = false;
    std::string m_hostKeyHost;
    std::string m_oldFingerprint;
    std::string m_newFingerprint;
    std::string m_oldKeyBase64;
    std::function<void()> m_startedHandler;
    std::function<void()> m_errorHandler;
    std::function<void(int)> m_finishedHandler;
    std::function<void(const std::string &)> m_dataHandler;
    std::function<void(const std::string &)> m_stderrHandler;
    std::function<void()> m_hostKeyMismatchHandler;
};
