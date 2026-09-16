#include "SshSession.h"
#include "SshLibrary.h"
#include "SshProxyRelay.h"
#include "NativeDataDir.h"
#include "NativeHash.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <libssh2.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

std::string base64Decode(std::string_view text)
{
    static constexpr int table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
    std::string result;
    result.reserve(text.size() * 3 / 4);
    unsigned int accumulator = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=' || character == '\n' || character == '\r'
            || character == '\t' || character == ' ')
            continue;
        const int value = table[static_cast<unsigned char>(character)];
        if (value < 0)
            continue;
        accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return result;
}

std::wstring utf8ToWide(std::string_view value)
{
#ifdef _WIN32
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::string wideToUtf8(std::wstring_view value)
{
#ifdef _WIN32
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
#else
    return std::string(value.begin(), value.end());
#endif
}

std::string trim(std::string_view value)
{
    const auto whitespace = [](unsigned char character) {
        return character == ' ' || character == '\t'
            || character == '\r' || character == '\n';
    };
    while (!value.empty() && whitespace(
               static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && whitespace(
               static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

std::string socketErrorMessage(int error)
{
#ifdef _WIN32
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                        reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::string message = length
        ? trim(wideToUtf8(std::wstring_view(buffer, length)))
        : "Windows socket error " + std::to_string(error);
    if (buffer) LocalFree(buffer);
    return message;
#else
    return "Socket error " + std::to_string(error);
#endif
}

}

struct SshSession::ResolverState
{
    ~ResolverState()
    {
        if (result)
            FreeAddrInfoExW(result);
        if (event)
            CloseHandle(event);
    }

    OVERLAPPED overlapped{};
    HANDLE event = nullptr;
    HANDLE cancelHandle = nullptr;
    PADDRINFOEXW result = nullptr;
    ADDRINFOEXW hints{};
    std::wstring host;
    bool pending = false;
};

SshSession::SshSession() = default;

SshSession::~SshSession()
{
    cleanup();
}

void SshSession::start(
    std::string_view connection, std::string_view port,
    std::string_view password, std::string_view privateKeyPath,
    std::string_view proxyJump, std::string_view proxyPassword,
    std::string_view proxyKeyPath, std::string_view command,
    std::string_view passphrase)
{
    ++m_attemptGeneration;
    m_connection = connection;
    m_portText = port;
    m_proxyJump = proxyJump;
    m_proxyPassword = proxyPassword;
    m_proxyKeyPath = proxyKeyPath;
    m_command = command;
    m_reconnectAllowed = false;
    cleanup();
    m_pendingWrite.clear();
    m_errorString.clear();
    m_errorNotificationPending = false;
    m_finishedEmitted = false;
    m_state = State::Idle;
    m_resolvedAddresses.clear();
    m_nextAddressIndex = 0;
    m_lastTcpError = 0;
    m_keepaliveTimerActive = false;
    m_password = password;
    m_passphrase = passphrase;
    m_privateKeyPath = privateKeyPath;
    m_port = 0;
    const char *portBegin = m_portText.data();
    const char *portEnd = portBegin + m_portText.size();
    const auto portResult = std::from_chars(portBegin, portEnd, m_port);
    if (portResult.ec != std::errc() || portResult.ptr != portEnd
        || m_port <= 0 || m_port > 65535) {
        m_port = 22;
    }
    m_socketPort = m_port;

    const std::size_t at = m_connection.find('@');
    m_user = at != std::string::npos && at > 0
        ? trim(std::string_view(m_connection).substr(0, at)) : std::string();
    m_host = at != std::string::npos && at > 0
        ? trim(std::string_view(m_connection).substr(at + 1))
        : trim(m_connection);
    if (m_host.empty() || m_user.empty()) {
        fail("服务器地址必须是“用户名@主机”。");
        return;
    }
    if (!masterSshInitializeLibraries()) {
        fail("无法初始化 Windows 网络或内置 SSH 库。");
        return;
    }
    m_state = State::TcpConnecting;
    m_connectionStarted = Clock::now();
    m_connectionTimerActive = true;
    if (trim(m_proxyJump).empty()) {
        openTcpSocket();
    } else {
        m_proxyRelay = std::make_unique<SshProxyRelay>();
        const std::uint64_t generation = m_attemptGeneration;
        if (!m_proxyRelay->start(m_proxyJump, m_host,
                                 m_port, m_proxyPassword,
                                 m_proxyKeyPath,
                                 [this, generation](std::uint16_t localPort,
                                                    const std::string &error) {
                                     std::lock_guard lock(m_proxyResultMutex);
                                     m_proxyResult.localPort = localPort;
                                     m_proxyResult.error = error;
                                     m_proxyResult.generation = generation;
                                     m_proxyResult.ready = true;
                                 })) {
            fail("无法启动内置跳板机转发器。");
            return;
        }
    }
}

void SshSession::openProxySocket(
    std::uint16_t localPort, const std::string &error)
{
    if (m_state != State::TcpConnecting) return;
    if (!error.empty() || localPort == 0) {
        fail(error.empty() ? "跳板机未能建立目标通道。" : error);
        return;
    }
    m_socketPort = localPort;
    m_resolvedAddresses.clear();
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    appendResolvedAddress(
        AF_INET, &loopback, sizeof(loopback), localPort);
    m_nextAddressIndex = 0;
    openNextTcpSocket();
}

void SshSession::reconnect()
{
    if (!m_reconnectAllowed || m_connection.empty()) return;
    start(m_connection, m_portText, m_password, m_privateKeyPath, m_proxyJump,
          m_proxyPassword, m_proxyKeyPath, m_command, m_passphrase);
}

void SshSession::openTcpSocket()
{
#ifdef _WIN32
    const std::wstring nativeHost = utf8ToWide(m_host);
    if (nativeHost.empty()) {
        fail("服务器地址不是有效的 UTF-8 文本。");
        return;
    }
    in_addr ipv4{};
    in6_addr ipv6{};
    int numericFamily = 0;
    const void *numericAddress = nullptr;
    std::size_t numericAddressSize = 0;
    if (InetPtonW(AF_INET, nativeHost.c_str(), &ipv4) == 1) {
        numericFamily = AF_INET;
        numericAddress = &ipv4;
        numericAddressSize = sizeof(ipv4);
    } else if (InetPtonW(AF_INET6, nativeHost.c_str(), &ipv6) == 1) {
        numericFamily = AF_INET6;
        numericAddress = &ipv6;
        numericAddressSize = sizeof(ipv6);
    }
    if (numericAddress) {
        m_resolvedAddresses.clear();
        appendResolvedAddress(
            numericFamily, numericAddress, numericAddressSize,
            static_cast<std::uint16_t>(m_socketPort));
        m_nextAddressIndex = 0;
        m_lastTcpError = 0;
        openNextTcpSocket();
        return;
    }
    m_resolverState = std::make_unique<ResolverState>();
    ResolverState &resolver = *m_resolverState;
    resolver.host = nativeHost;
    resolver.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!resolver.event) {
        m_resolverState.reset();
        fail("无法创建 SSH DNS 完成事件。");
        return;
    }
    resolver.overlapped.hEvent = resolver.event;
    resolver.hints.ai_family = AF_UNSPEC;
    resolver.hints.ai_socktype = SOCK_STREAM;
    resolver.hints.ai_protocol = IPPROTO_TCP;
    const int lookupResult = GetAddrInfoExW(
        resolver.host.c_str(), nullptr, NS_ALL, nullptr, &resolver.hints,
        &resolver.result, nullptr, &resolver.overlapped, nullptr,
        &resolver.cancelHandle);
    if (lookupResult == 0) {
        SetEvent(resolver.event);
    } else if (lookupResult == WSA_IO_PENDING) {
        resolver.pending = true;
    } else {
        m_resolverState.reset();
        fail("无法解析服务器地址“" + m_host + "”：" +
             socketErrorMessage(lookupResult));
    }
#else
    fail("当前内置 SSH 会话仅支持 Windows。");
#endif
}

void SshSession::pollResolver()
{
#ifdef _WIN32
    if (!m_resolverState
        || WaitForSingleObject(m_resolverState->event, 0) != WAIT_OBJECT_0) {
        return;
    }

    ResolverState &resolver = *m_resolverState;
    const int result = resolver.pending
        ? GetAddrInfoExOverlappedResult(&resolver.overlapped) : 0;
    resolver.pending = false;
    if (result != 0) {
        m_resolverState.reset();
        fail("无法解析服务器地址“" + m_host + "”：" +
             socketErrorMessage(result));
        return;
    }

    m_resolvedAddresses.clear();
    for (PADDRINFOEXW entry = resolver.result; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET && entry->ai_addr
            && entry->ai_addrlen >= sizeof(sockaddr_in)) {
            const auto *address =
                reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
            appendResolvedAddress(
                AF_INET, &address->sin_addr, sizeof(address->sin_addr),
                static_cast<std::uint16_t>(m_socketPort));
        }
    }
    for (PADDRINFOEXW entry = resolver.result; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET6 && entry->ai_addr
            && entry->ai_addrlen >= sizeof(sockaddr_in6)) {
            const auto *address =
                reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
            appendResolvedAddress(
                AF_INET6, &address->sin6_addr, sizeof(address->sin6_addr),
                static_cast<std::uint16_t>(m_socketPort));
        }
    }
    m_resolverState.reset();
    if (m_resolvedAddresses.empty()) {
        fail("服务器地址“" + m_host + "”没有可用的 IPv4/IPv6 地址。");
        return;
    }
    m_nextAddressIndex = 0;
    m_lastTcpError = 0;
    openNextTcpSocket();
#endif
}

void SshSession::cleanupResolver()
{
#ifdef _WIN32
    if (!m_resolverState)
        return;
    ResolverState &resolver = *m_resolverState;
    if (resolver.pending
        && WaitForSingleObject(resolver.event, 0) != WAIT_OBJECT_0) {
        if (resolver.cancelHandle)
            GetAddrInfoExCancel(&resolver.cancelHandle);
        WaitForSingleObject(resolver.event, INFINITE);
    }
    resolver.pending = false;
    m_resolverState.reset();
#else
    m_resolverState.reset();
#endif
}

void SshSession::appendResolvedAddress(
    int family, const void *address, std::size_t addressSize,
    std::uint16_t port)
{
#ifdef _WIN32
    SocketAddress result;
    static_assert(sizeof(sockaddr_storage) <= sizeof(result.bytes));
    if (family == AF_INET && address
        && addressSize >= sizeof(in_addr)) {
        sockaddr_in native{};
        native.sin_family = AF_INET;
        native.sin_port = htons(port);
        std::memcpy(&native.sin_addr, address, sizeof(native.sin_addr));
        std::memcpy(result.bytes.data(), &native, sizeof(native));
        result.length = sizeof(native);
        result.family = AF_INET;
    } else if (family == AF_INET6 && address
               && addressSize >= sizeof(in6_addr)) {
        sockaddr_in6 native{};
        native.sin6_family = AF_INET6;
        native.sin6_port = htons(port);
        std::memcpy(&native.sin6_addr, address, sizeof(native.sin6_addr));
        std::memcpy(result.bytes.data(), &native, sizeof(native));
        result.length = sizeof(native);
        result.family = AF_INET6;
    } else {
        return;
    }
    m_resolvedAddresses.push_back(result);
#else
    (void)family;
    (void)address;
    (void)addressSize;
    (void)port;
#endif
}

void SshSession::openNextTcpSocket()
{
#ifdef _WIN32
    while (m_nextAddressIndex < static_cast<int>(m_resolvedAddresses.size())) {
        const SocketAddress &address = m_resolvedAddresses.at(
            static_cast<size_t>(m_nextAddressIndex++));
        SOCKET socket = ::socket(address.family, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            m_lastTcpError = WSAGetLastError();
            continue;
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonBlocking) != 0) {
            m_lastTcpError = WSAGetLastError();
            closesocket(socket);
            continue;
        }
        const int result = ::connect(
            socket,
            reinterpret_cast<const sockaddr *>(address.bytes.data()),
            address.length);
        if (result == 0 || WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS) {
            m_socket = static_cast<std::uintptr_t>(socket);
            // Keep the underlying TCP connection observable while the SSH
            // session is idle. SO_KEEPALIVE is deliberately only a fallback;
            // libssh2 keepalive below carries the SSH-level heartbeat.
            const BOOL keepAlive = TRUE;
            setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE,
                       reinterpret_cast<const char *>(&keepAlive), sizeof(keepAlive));
                return;
        }
            m_lastTcpError = WSAGetLastError();
        closesocket(socket);
    }
    fail("无法建立 TCP 连接：" + socketErrorMessage(m_lastTcpError));
#endif
}

bool SshSession::pollTcpConnection()
{
#ifdef _WIN32
    if (m_socket == ~std::uintptr_t(0)) return false;
    const SOCKET socket = static_cast<SOCKET>(m_socket);
    fd_set writable;
    fd_set errors;
    FD_ZERO(&writable);
    FD_ZERO(&errors);
    FD_SET(socket, &writable);
    FD_SET(socket, &errors);
    timeval timeout{};
    const int selected = select(0, nullptr, &writable, &errors, &timeout);
    if (selected == 0) return false;
    if (selected == SOCKET_ERROR) {
        fail("TCP 连接检查失败：" + socketErrorMessage(WSAGetLastError()));
        return false;
    }
    int socketError = 0;
    int errorSize = sizeof(socketError);
    getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socketError), &errorSize);
    if (socketError != 0 || FD_ISSET(socket, &errors)) {
        m_lastTcpError = socketError != 0 ? socketError : WSAGetLastError();
        closesocket(socket);
        m_socket = ~std::uintptr_t(0);
        openNextTcpSocket();
        return false;
    }
    m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    if (!m_session) {
        fail("无法创建内置 SSH 会话。");
        return false;
    }
    libssh2_session_set_blocking(m_session, 0);
    m_state = State::Handshake;
    return true;
#else
    return false;
#endif
}

void SshSession::resize(int columns, int rows)
{
    if (columns < 1 || rows < 1) return;
    m_columns = columns;
    m_rows = rows;
    m_pendingColumns = columns;
    m_pendingRows = rows;
}

bool SshSession::isRunning() const
{
    return m_state == State::Running;
}

void SshSession::setStartedHandler(std::function<void()> handler)
{
    m_startedHandler = std::move(handler);
}

void SshSession::setErrorHandler(std::function<void()> handler)
{
    m_errorHandler = std::move(handler);
}

void SshSession::setFinishedHandler(std::function<void(int)> handler)
{
    m_finishedHandler = std::move(handler);
}

void SshSession::setDataHandler(
    std::function<void(const std::string &)> handler)
{
    m_dataHandler = std::move(handler);
}

void SshSession::setStderrHandler(
    std::function<void(const std::string &)> handler)
{
    m_stderrHandler = std::move(handler);
}

void SshSession::setKeepaliveInterval(int seconds)
{
    m_keepaliveIntervalSeconds = seconds > 0 ? seconds : 0;
}

std::size_t SshSession::write(const char *data, std::size_t size)
{
    if (m_state != State::Running || !data || size == 0)
        return 0;
    m_pendingWrite.append(data, size);
    return size;
}

void SshSession::sendEof()
{
    m_eofPending = true;
}

void SshSession::advanceConnection()
{
    if (!m_session || m_socket == ~std::uintptr_t(0)) return;
    const std::string &user = m_user;
    int result = 0;
    switch (m_state) {
    case State::Handshake:
        result = libssh2_session_handshake(m_session, static_cast<libssh2_socket_t>(m_socket));
        if (retryLater(result)) return;
        if (result != 0) { fail(lastSessionError()); return; }
        if (!verifyHostKey()) return;
        m_state = State::Authenticate;
        [[fallthrough]];
    case State::Authenticate:
        if (m_privateKeyPath.empty()) {
            const std::string &password = m_password;
            result = libssh2_userauth_password_ex(
                m_session, user.data(), static_cast<unsigned int>(user.size()),
                password.data(), static_cast<unsigned int>(password.size()), nullptr);
        } else {
            const std::string &key = m_privateKeyPath;
            const std::string &passphrase = m_passphrase.empty()
                ? m_password : m_passphrase;
            result = libssh2_userauth_publickey_fromfile_ex(
                m_session, user.data(), static_cast<unsigned int>(user.size()),
                nullptr, key.c_str(), passphrase.empty() ? nullptr : passphrase.c_str());
        }
        if (retryLater(result)) return;
        if (result != 0) {
            fail("SSH 身份验证失败：" + lastSessionError());
            return;
        }
        m_state = State::OpenChannel;
        [[fallthrough]];
    case State::OpenChannel:
        m_channel = libssh2_channel_open_session(m_session);
        if (!m_channel) {
            if (libssh2_session_last_errno(m_session) == LIBSSH2_ERROR_EAGAIN) return;
            fail(lastSessionError());
            return;
        }
        m_state = State::RequestPty;
        [[fallthrough]];
    case State::RequestPty:
        // `top` depends on the terminfo `smcup`/`rmcup` capabilities to enter
        // the alternate screen. Some minimal WSL/server terminfo installs have
        // an incomplete xterm-256color entry, so top falls back to repainting
        // the primary screen and overwrites the previous `ll` output. `xterm`
        // is universally available and retains the capabilities needed by
        // vim/nano/top, including the alternate-screen pair.
        if (!m_command.empty()) {
            m_state = State::StartShell;
            break;
        }
        result = libssh2_channel_request_pty_ex(m_channel, "xterm", 5, nullptr, 0,
                                                 m_columns, m_rows, 0, 0);
        if (retryLater(result)) return;
        if (result != 0) {
            fail("无法请求远端终端：" + lastSessionError());
            return;
        }
        m_pendingColumns = 0;
        m_pendingRows = 0;
        m_localeIndex = 0;
        m_state = State::SetLocale;
        [[fallthrough]];
    case State::SetLocale:
        {
            // Best-effort UTF-8 locale for the remote shell. Without a UTF-8
            // locale bash prints non-ASCII input as "$'\\350...'" escapes and
            // readline's column math breaks CJK completion. Some servers
            // refuse setenv requests; skip the rest in that case.
            static constexpr std::pair<const char *, const char *> localeVars[] = {
                {"LANG", "C.UTF-8"},
                // LC_CTYPE is what readline uses for multibyte character
                // widths.  Keep it separate from LC_ALL so a server which
                // only accepts part of the LC_* family can still provide a
                // UTF-8 terminal.
                {"LC_CTYPE", "C.UTF-8"},
                {"LC_ALL", "C.UTF-8"},
            };
            if (m_localeIndex < std::size(localeVars)) {
                const auto &[name, value] = localeVars[m_localeIndex];
                result = libssh2_channel_setenv_ex(
                    m_channel, name, static_cast<unsigned int>(std::strlen(name)),
                    value, static_cast<unsigned int>(std::strlen(value)));
                if (retryLater(result)) return;
                // Environment forwarding is governed by sshd's AcceptEnv.
                // A rejected LANG request must not prevent later LC_* names
                // from being tried: servers commonly configure these rules
                // independently.  Every request remains best-effort.
                ++m_localeIndex;
                return;
            }
        }
        m_state = State::StartShell;
        [[fallthrough]];
    case State::StartShell:
        if (m_command.empty()) {
            result = libssh2_channel_shell(m_channel);
            if (retryLater(result)) return;
            if (result != 0) {
                fail("无法启动远端 Shell：" + lastSessionError());
                return;
            }
        } else {
            // Run a single command through an SSH "exec" channel. No PTY is
            // requested, so the remote side does not allocate a terminal and
            // stdout/stderr carry the raw command output (no control codes).
            result = libssh2_channel_process_startup(
                m_channel, "exec", 4, m_command.data(), m_command.size());
            if (retryLater(result)) return;
            if (result != 0) {
                fail("无法启动远端命令：" + lastSessionError());
                return;
            }
        }
        m_state = State::Running;
        // SSH-level keepalive prevents silent half-open sessions when a NAT,
        // Wi-Fi adapter, or firewall drops an idle connection. The call is
        // non-blocking because the session itself is non-blocking.
        libssh2_keepalive_config(m_session, 1, m_keepaliveIntervalSeconds);
        m_keepaliveStarted = Clock::now();
        m_keepaliveTimerActive = true;
        if (m_startedHandler)
            m_startedHandler();
        return;
    default:
        return;
    }
}

bool SshSession::verifyHostKey()
{
    m_knownHosts = libssh2_knownhost_init(m_session);
    if (!m_knownHosts) {
        fail("无法初始化 SSH 主机密钥校验。");
        return false;
    }
    const std::filesystem::path filePath = knownHostsPath();
    const std::string fileName = wideToUtf8(filePath.wstring());
    std::error_code existsError;
    if (std::filesystem::exists(filePath, existsError))
        libssh2_knownhost_readfile(m_knownHosts, fileName.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    else if (existsError) {
        fail("无法读取 SSH 主机密钥文件：" + existsError.message());
        return false;
    }

    size_t keyLength = 0;
    int keyType = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    const char *key = libssh2_session_hostkey(m_session, &keyLength, &keyType);
    if (!key || keyLength == 0) {
        fail("服务器未提供可校验的 SSH 主机密钥。");
        return false;
    }
    int knownKeyType = LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    switch (keyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: knownKeyType = LIBSSH2_KNOWNHOST_KEY_SSHRSA; break;
    case LIBSSH2_HOSTKEY_TYPE_DSS: knownKeyType = LIBSSH2_KNOWNHOST_KEY_SSHDSS; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: knownKeyType = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: knownKeyType = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: knownKeyType = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: knownKeyType = LIBSSH2_KNOWNHOST_KEY_ED25519; break;
    default: break;
    }
    const std::string hostName = m_port == 22
        ? m_host : "[" + m_host + "]:" + std::to_string(m_port);
    const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownKeyType;
    libssh2_knownhost *matched = nullptr;
    const int check = libssh2_knownhost_check(
        m_knownHosts, hostName.c_str(), key, keyLength, typeMask, &matched);
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        // Do not block silently: pause the connection and show the old vs new
        // fingerprints side by side so the user can decide whether to trust
        // the changed key (server reinstall) or abort (possible attack).
        const char *hash = libssh2_hostkey_hash(
            m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
        m_newFingerprint = hash
            ? NativeHash::sha256Hex(
                reinterpret_cast<const unsigned char *>(hash), 32)
            : std::string();
        m_oldKeyBase64 = matched && matched->key ? matched->key : "";
        const std::string oldBlob = base64Decode(m_oldKeyBase64);
        m_oldFingerprint = oldBlob.empty()
            ? std::string() : NativeHash::sha256Hex(
                reinterpret_cast<const unsigned char *>(oldBlob.data()),
                oldBlob.size());
        m_hostKeyHost = hostName;
        m_hostKeyPending = true;
        m_connectionTimerActive = false;
        m_state = State::AwaitHostKeyConfirm;
        const auto handler = m_hostKeyMismatchHandler;
        if (handler)
            handler();
        return false;
    }
    if (check == LIBSSH2_KNOWNHOST_CHECK_FAILURE) {
        fail("无法校验 SSH 主机密钥。");
        return false;
    }
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        if (libssh2_knownhost_addc(m_knownHosts, hostName.c_str(), nullptr, key, keyLength,
                                   nullptr, 0, typeMask, nullptr) != 0) {
            fail("无法保存 SSH 主机密钥。");
            return false;
        }
        std::error_code directoryError;
        std::filesystem::create_directories(filePath.parent_path(), directoryError);
        if (directoryError) {
            fail("无法创建 SSH 主机密钥目录。");
            return false;
        }
        if (libssh2_knownhost_writefile(m_knownHosts, fileName.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
            fail("无法写入 SSH 主机密钥文件。");
            return false;
        }
    }
    return true;
}

void SshSession::setHostKeyMismatchHandler(std::function<void()> handler)
{
    m_hostKeyMismatchHandler = std::move(handler);
}

bool SshSession::confirmHostKey()
{
    std::lock_guard<std::recursive_mutex> locker(masterSshLibraryMutex());
    if (!m_hostKeyPending || !m_knownHosts || !m_session)
        return false;
    const int knownKeyType = [this] {
        size_t keyLength = 0;
        int keyType = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
        libssh2_session_hostkey(m_session, &keyLength, &keyType);
        switch (keyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
        }
    }();
    const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN
        | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownKeyType;
    size_t keyLength = 0;
    int keyType = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    const char *key = libssh2_session_hostkey(
        m_session, &keyLength, &keyType);
    if (!key || keyLength == 0)
        return false;
    libssh2_knownhost *matched = nullptr;
    const int check = libssh2_knownhost_check(
        m_knownHosts, m_hostKeyHost.c_str(), key, keyLength,
        typeMask, &matched);
    if (check != LIBSSH2_KNOWNHOST_CHECK_MISMATCH || !matched)
        return false;
    libssh2_knownhost_del(m_knownHosts, matched);
    if (libssh2_knownhost_addc(m_knownHosts, m_hostKeyHost.c_str(), nullptr,
                               key, keyLength, nullptr, 0, typeMask, nullptr) != 0)
        return false;
    const std::filesystem::path filePath = knownHostsPath();
    std::error_code directoryError;
    std::filesystem::create_directories(filePath.parent_path(), directoryError);
    if (directoryError)
        return false;
    const std::string fileName = wideToUtf8(filePath.wstring());
    if (libssh2_knownhost_writefile(
            m_knownHosts, fileName.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0)
        return false;
    m_hostKeyPending = false;
    m_state = State::Authenticate;
    return true;
}

void SshSession::abortHostKey()
{
    if (!m_hostKeyPending)
        return;
    m_hostKeyPending = false;
    fail("主机密钥已更改，连接已取消。");
}

std::filesystem::path SshSession::knownHostsPath() const
{
    return NativeDataDir::knownHostsFile();
}

void SshSession::poll()
{
    if (m_errorNotificationPending) {
        m_errorNotificationPending = false;
        const auto handler = m_errorHandler;
        if (handler)
            handler();
        return;
    }

    ProxyResult proxyResult;
    {
        std::lock_guard lock(m_proxyResultMutex);
        if (m_proxyResult.ready) {
            proxyResult = std::move(m_proxyResult);
            m_proxyResult = {};
        }
    }
    if (proxyResult.ready
        && proxyResult.generation == m_attemptGeneration) {
        openProxySocket(proxyResult.localPort, proxyResult.error);
    }

    std::lock_guard<std::recursive_mutex> locker(masterSshLibraryMutex());
    if (m_state == State::Failed || m_state == State::Idle) return;
    if (m_state == State::AwaitHostKeyConfirm) return;
    if (m_state != State::Running && m_connectionTimerActive
        && Clock::now() - m_connectionStarted > std::chrono::seconds(45)) {
        fail("SSH 连接超时（45 秒，停留阶段：" +
             std::string(connectionPhase()) + "）。");
        return;
    }
    if (m_state == State::TcpConnecting) {
        if (m_resolverState) {
            pollResolver();
            return;
        }
        pollTcpConnection();
        return;
    }
    if (m_state != State::Running) {
        advanceConnection();
        return;
    }
    if (m_session && m_keepaliveTimerActive && m_keepaliveIntervalSeconds > 0
        && Clock::now() - m_keepaliveStarted
            >= std::chrono::seconds(m_keepaliveIntervalSeconds)) {
        int secondsToNext = 0;
        const int keepaliveResult = libssh2_keepalive_send(m_session, &secondsToNext);
        if (keepaliveResult == LIBSSH2_ERROR_EAGAIN) return;
        if (keepaliveResult < 0) {
            fail("SSH 保活失败：" + lastSessionError());
            return;
        }
        m_keepaliveStarted = Clock::now();
    }
    if (m_pendingColumns > 0 && m_channel) {
        const int result = libssh2_channel_request_pty_size_ex(m_channel, m_pendingColumns, m_pendingRows, 0, 0);
        if (!retryLater(result)) {
            if (result != 0) {
                fail("无法更新终端尺寸：" + lastSessionError());
                return;
            }
            m_pendingColumns = 0;
            m_pendingRows = 0;
        }
    }
    flushInput();
    if (m_eofPending && m_pendingWrite.empty() && m_channel) {
        const int eofResult = libssh2_channel_send_eof(m_channel);
        if (eofResult != LIBSSH2_ERROR_EAGAIN)
            m_eofPending = false;
    }
    readChannel(0);
    readChannel(1);
    if (m_channel && libssh2_channel_eof(m_channel))
        finishSession(libssh2_channel_get_exit_status(m_channel));
}

void SshSession::flushInput()
{
    if (!m_channel || m_pendingWrite.empty()) return;
    const ssize_t written = libssh2_channel_write_ex(m_channel, 0, m_pendingWrite.data(),
                                                     static_cast<size_t>(m_pendingWrite.size()));
    if (written == LIBSSH2_ERROR_EAGAIN) return;
    if (written < 0) {
        fail("写入远端终端失败：" + lastSessionError());
        return;
    }
    m_pendingWrite.erase(0, static_cast<size_t>(written));
}

void SshSession::readChannel(int stream)
{
    if (!m_channel) return;
    char buffer[16384];
    // Keep the event loop responsive when a command continuously produces
    // output (apt, journalctl, top, etc.). The next 10ms poll drains more.
    for (int reads = 0; reads < 4; ++reads) {
        const ssize_t count = libssh2_channel_read_ex(m_channel, stream, buffer, sizeof(buffer));
        if (count == LIBSSH2_ERROR_EAGAIN || count == 0) return;
        if (count < 0) {
            fail("读取远端终端失败：" + lastSessionError());
            return;
        }
        const std::string data(buffer, static_cast<std::size_t>(count));
        if (stream == 1 && m_stderrHandler)
            m_stderrHandler(data);
        else if (m_dataHandler)
            m_dataHandler(data);
    }
}

void SshSession::finishSession(int exitCode)
{
    if (m_finishedEmitted) return;
    m_finishedEmitted = true;
    cleanup();
    if (m_finishedHandler) m_finishedHandler(exitCode);
}

void SshSession::fail(std::string message)
{
    if (m_state == State::Failed) return;
    m_errorString = message.empty()
        ? "内置 SSH 会话失败。" : std::move(message);
    m_reconnectAllowed = true;
    m_state = State::Failed;
    cleanup();
    // Keep UI-facing error delivery out of the libssh2/socket stack. The next
    // Win32 backend poll dispatches it without a queued framework callback.
    m_errorNotificationPending = true;
}

void SshSession::cleanup()
{
    std::lock_guard<std::recursive_mutex> locker(masterSshLibraryMutex());
    m_connectionTimerActive = false;
    m_keepaliveTimerActive = false;
    if (m_proxyRelay) {
        auto relay = std::move(m_proxyRelay);
        relay->stop();
    }
    {
        std::lock_guard lock(m_proxyResultMutex);
        m_proxyResult = {};
    }
    cleanupResolver();
    m_resolvedAddresses.clear();
    m_nextAddressIndex = 0;
    // Never turn a session back to blocking mode here. A remote peer that is
    // slow or already gone would otherwise freeze the complete UI while the
    // close handshake waits on the network.
#ifdef _WIN32
    if (m_socket != ~std::uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(m_socket));
        m_socket = ~std::uintptr_t(0);
    }
#endif
    if (m_channel) {
        libssh2_channel_free(m_channel);
        m_channel = nullptr;
    }
    if (m_knownHosts) {
        libssh2_knownhost_free(m_knownHosts);
        m_knownHosts = nullptr;
    }
    if (m_session) {
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_state != State::Failed) m_state = State::Idle;
}

std::string SshSession::lastSessionError() const
{
    if (!m_session) return "SSH 会话不可用。";
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(m_session, &message, &length, 0);
    return message && length > 0
        ? std::string(message, static_cast<std::size_t>(length))
        : "未知 SSH 错误。";
}

std::string_view SshSession::connectionPhase() const
{
    switch (m_state) {
    case State::TcpConnecting: return "TCP 连接";
    case State::Handshake: return "SSH 握手";
    case State::Authenticate: return "身份验证";
    case State::OpenChannel: return "打开会话通道";
    case State::RequestPty: return "请求终端";
    case State::SetLocale: return "设置 UTF-8 终端环境";
    case State::StartShell: return "启动 Shell";
    default: return "未知";
    }
}

bool SshSession::retryLater(int result) const
{
    return result == LIBSSH2_ERROR_EAGAIN;
}
