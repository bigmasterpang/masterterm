#include "SshTunnel.h"

#include "SshLibrary.h"

#include <libssh2.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

std::string nextTunnelId()
{
    static std::atomic_uint64_t sequence{0};
    return "tunnel-" + std::to_string(++sequence);
}

std::string tunnelError(_LIBSSH2_SESSION *session)
{
    if (!session)
        return "SSH 会话不可用";
    char *message = nullptr;
    libssh2_session_last_error(session, &message, nullptr, 0);
    return message && *message ? message : "未知 SSH 错误";
}

bool setNonBlocking(std::uintptr_t socketValue)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(socketValue), FIONBIO, &mode) == 0;
#else
    return fcntl(static_cast<int>(socketValue), F_SETFL,
                 O_NONBLOCK) == 0;
#endif
}

std::uintptr_t createTcpSocket(const std::string &host, int port,
                               bool *connectNonBlocking = nullptr)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                    &hints, &addresses) != 0)
        return ~std::uintptr_t(0);
    SOCKET socketValue = INVALID_SOCKET;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        socketValue = ::socket(address->ai_family,
                               address->ai_socktype, address->ai_protocol);
        if (socketValue == INVALID_SOCKET)
            continue;
        if (connectNonBlocking) {
            const int result = connect(socketValue, address->ai_addr,
                                       static_cast<int>(address->ai_addrlen));
            if (result == 0 || WSAGetLastError() == WSAEWOULDBLOCK)
                break;
            closesocket(socketValue);
            socketValue = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    if (socketValue == INVALID_SOCKET)
        return ~std::uintptr_t(0);
    if (!setNonBlocking(static_cast<std::uintptr_t>(socketValue))) {
        closesocket(socketValue);
        return ~std::uintptr_t(0);
    }
    return static_cast<std::uintptr_t>(socketValue);
}

} // namespace

SshTunnel::SshTunnel(Mode mode, std::string listenHost, int listenPort,
                     std::string targetHost, int targetPort)
    : m_id(nextTunnelId()),
      m_mode(mode),
      m_listenHost(std::move(listenHost)),
      m_listenPort(listenPort),
      m_targetHost(std::move(targetHost)),
      m_targetPort(targetPort)
{
}

SshTunnel::~SshTunnel()
{
    cleanup();
}

void SshTunnel::start(_LIBSSH2_SESSION *session, StateCallback callback)
{
    m_session = session;
    m_stateCallback = std::move(callback);
    if (!m_session) {
        setState(State::Failed, "SSH 会话不可用");
        return;
    }
    m_sessionValid = true;
    const bool started = m_mode == Mode::Local
        ? startLocalListener() : startRemoteListener();
    if (!started)
        return; // state already set by the helper
    if (m_mode == Mode::Local)
        setState(State::Listening);
    // Remote listeners complete asynchronously from poll().
}

void SshTunnel::stop()
{
    if (m_state == State::Stopped)
        return;
    cleanup();
    setState(State::Stopped);
}

void SshTunnel::invalidate()
{
    m_sessionValid = false;
    cleanup();
    setState(State::Failed, "SSH 会话已断开");
}

void SshTunnel::poll()
{
    if (m_state == State::Starting) {
        // Remote listeners complete asynchronously on a non-blocking session.
        if (m_mode == Mode::Remote && !m_listener) {
            std::lock_guard<std::recursive_mutex> locker(
                masterSshLibraryMutex());
            int boundPort = 0;
            m_listener = libssh2_channel_forward_listen_ex(
                m_session, m_listenHost.empty()
                    ? nullptr : m_listenHost.c_str(),
                m_listenPort, &boundPort, 16);
            if (!m_listener) {
                const int error = libssh2_session_last_errno(m_session);
                if (error != LIBSSH2_ERROR_EAGAIN) {
                    setState(State::Failed,
                             "无法在远端建立监听：" + tunnelError(m_session));
                }
                return;
            }
            m_boundPort = boundPort > 0 ? boundPort : m_listenPort;
            setState(State::Listening);
            return;
        }
        return;
    }
    if (m_state != State::Listening)
        return;
    if (m_mode == Mode::Local)
        acceptLocalConnections();
    else
        acceptRemoteConnections();
    pollRelays();
}

bool SshTunnel::startLocalListener()
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *addresses = nullptr;
    const std::string portText = std::to_string(m_listenPort);
    if (getaddrinfo(m_listenHost.empty() ? nullptr : m_listenHost.c_str(),
                    portText.c_str(), &hints, &addresses) != 0) {
        setState(State::Failed, "无法解析监听地址");
        return false;
    }
    SOCKET listenSocket = INVALID_SOCKET;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        listenSocket = ::socket(address->ai_family,
                                address->ai_socktype, address->ai_protocol);
        if (listenSocket == INVALID_SOCKET)
            continue;
        int reuse = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        if (bind(listenSocket, address->ai_addr,
                 static_cast<int>(address->ai_addrlen)) == 0
            && listen(listenSocket, SOMAXCONN) == 0)
            break;
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (listenSocket == INVALID_SOCKET) {
        setState(State::Failed, "无法在本地监听端口 " + portText);
        return false;
    }
    if (!setNonBlocking(static_cast<std::uintptr_t>(listenSocket))) {
        closesocket(listenSocket);
        setState(State::Failed, "无法设置监听为非阻塞");
        return false;
    }
    m_listenSocket = static_cast<std::uintptr_t>(listenSocket);
    sockaddr_storage bound{};
    socklen_t boundLength = sizeof(bound);
    if (getsockname(listenSocket, reinterpret_cast<sockaddr *>(&bound),
                    &boundLength) == 0) {
        if (bound.ss_family == AF_INET) {
            m_boundPort = ntohs(
                reinterpret_cast<sockaddr_in *>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            m_boundPort = ntohs(
                reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port);
        }
    }
    if (m_boundPort <= 0)
        m_boundPort = m_listenPort;
    return true;
}

bool SshTunnel::startRemoteListener()
{
    // The listener is created asynchronously from poll() because the
    // non-blocking session can return EAGAIN for this call.
    return true;
}

void SshTunnel::acceptLocalConnections()
{
    while (true) {
        SOCKET client = accept(
            static_cast<SOCKET>(m_listenSocket), nullptr, nullptr);
        if (client == INVALID_SOCKET)
            return; // WSAEWOULDBLOCK when no more pending connections
        if (!setNonBlocking(static_cast<std::uintptr_t>(client))) {
            closesocket(client);
            continue;
        }
        auto relay = std::make_unique<Relay>();
        relay->clientSocket = static_cast<std::uintptr_t>(client);
        relay->channelPending = true;
        m_relays.push_back(std::move(relay));
    }
}

void SshTunnel::acceptRemoteConnections()
{
    if (!m_listener)
        return;
    while (true) {
        std::lock_guard<std::recursive_mutex> locker(
            masterSshLibraryMutex());
        _LIBSSH2_CHANNEL *channel =
            libssh2_channel_forward_accept(m_listener);
        if (!channel) {
            const int error = libssh2_session_last_errno(m_session);
            if (error == LIBSSH2_ERROR_EAGAIN)
                return;
            if (error == LIBSSH2_ERROR_CHANNEL_CLOSED)
                return;
            setState(State::Failed,
                     "接受远端转发连接失败：" +
                     tunnelError(m_session));
            return;
        }
        bool connectRequested = false;
        std::uintptr_t client = createTcpSocket(
            m_targetHost, m_targetPort, &connectRequested);
        if (client == ~std::uintptr_t(0)) {
            libssh2_channel_free(channel);
            continue;
        }
        auto relay = std::make_unique<Relay>();
        relay->clientSocket = client;
        relay->channel = channel;
        relay->connectPending = true;
        m_relays.push_back(std::move(relay));
    }
}

void SshTunnel::pollRelays()
{
    for (auto &relay : m_relays)
        pumpRelay(*relay);
    m_relays.erase(
        std::remove_if(m_relays.begin(), m_relays.end(),
                       [](const std::unique_ptr<Relay> &relay) {
                           return relay->clientSocket == ~std::uintptr_t(0);
                       }),
        m_relays.end());
}

void SshTunnel::pumpRelay(Relay &relay)
{
    if (relay.channelPending) {
        std::lock_guard<std::recursive_mutex> locker(
            masterSshLibraryMutex());
        relay.channel = libssh2_channel_direct_tcpip_ex(
            m_session, m_targetHost.c_str(), m_targetPort,
            m_listenHost.empty() ? "127.0.0.1" : m_listenHost.c_str(),
            m_boundPort > 0 ? m_boundPort : m_listenPort);
        if (!relay.channel) {
            const int error = libssh2_session_last_errno(m_session);
            if (error == LIBSSH2_ERROR_EAGAIN)
                return;
            closeRelay(relay);
            return;
        }
        relay.channelPending = false;
    }
    if (relay.connectPending) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(static_cast<SOCKET>(relay.clientSocket), &writeSet);
        timeval timeout{};
        const int ready = select(0, nullptr, &writeSet, nullptr, &timeout);
        if (ready <= 0)
            return;
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(static_cast<SOCKET>(relay.clientSocket),
                       SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char *>(&socketError),
                       &errorLength) != 0
            || socketError != 0) {
            closeRelay(relay);
            return;
        }
        relay.connectPending = false;
    }
    if (!relay.channel || relay.channelPending || relay.connectPending)
        return;
    pumpClientToChannel(relay);
    pumpChannelToClient(relay);

    if (relay.clientEof && relay.remoteEof
        && relay.pendingToRemote.empty()
        && relay.pendingToClient.empty()) {
        closeRelay(relay);
    }
}

void SshTunnel::pumpClientToChannel(Relay &relay)
{
    if (relay.clientEof)
        return;
    char buffer[16384];
    while (relay.pendingToRemote.size() < 1024 * 1024) {
        const int count = recv(
            static_cast<SOCKET>(relay.clientSocket),
            buffer, sizeof(buffer), 0);
        if (count > 0) {
            relay.pendingToRemote.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            relay.clientEof = true;
            break;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            break;
        closeRelay(relay);
        return;
    }
    if (relay.pendingToRemote.empty())
        return;
    std::lock_guard<std::recursive_mutex> locker(masterSshLibraryMutex());
    const ssize_t written = libssh2_channel_write(
        relay.channel, relay.pendingToRemote.data(),
        relay.pendingToRemote.size());
    if (written == LIBSSH2_ERROR_EAGAIN)
        return;
    if (written < 0) {
        closeRelay(relay);
        return;
    }
    relay.pendingToRemote.erase(0, static_cast<std::size_t>(written));
    if (relay.clientEof && relay.pendingToRemote.empty()) {
        libssh2_channel_send_eof(relay.channel);
    }
}

void SshTunnel::pumpChannelToClient(Relay &relay)
{
    char buffer[16384];
    if (!relay.remoteEof) {
        std::lock_guard<std::recursive_mutex> locker(
            masterSshLibraryMutex());
        for (int reads = 0; reads < 4; ++reads) {
            const ssize_t count = libssh2_channel_read(
                relay.channel, buffer, sizeof(buffer));
            if (count == LIBSSH2_ERROR_EAGAIN || count == 0)
                break;
            if (count < 0) {
                closeRelay(relay);
                return;
            }
            relay.pendingToClient.append(
                buffer, static_cast<std::size_t>(count));
        }
        if (relay.channel && libssh2_channel_eof(relay.channel))
            relay.remoteEof = true;
    }
    while (!relay.pendingToClient.empty()) {
        const int count = send(
            static_cast<SOCKET>(relay.clientSocket),
            relay.pendingToClient.data(),
            static_cast<int>(relay.pendingToClient.size()), 0);
        if (count > 0) {
            relay.pendingToClient.erase(0, static_cast<std::size_t>(count));
            continue;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            break;
        closeRelay(relay);
        return;
    }
    if (relay.remoteEof && relay.pendingToClient.empty()) {
        closeRelay(relay);
    }
}

void SshTunnel::closeRelay(Relay &relay)
{
    if (relay.clientSocket != ~std::uintptr_t(0)) {
        shutdown(static_cast<SOCKET>(relay.clientSocket), SD_BOTH);
        closesocket(static_cast<SOCKET>(relay.clientSocket));
        relay.clientSocket = ~std::uintptr_t(0);
    }
    if (relay.channel && m_sessionValid) {
        std::lock_guard<std::recursive_mutex> locker(
            masterSshLibraryMutex());
        libssh2_channel_free(relay.channel);
    }
    relay.channel = nullptr;
    relay.channelPending = false;
    relay.connectPending = false;
    relay.pendingToClient.clear();
    relay.pendingToRemote.clear();
}

void SshTunnel::setState(State state, const std::string &error)
{
    m_state = state;
    if (!error.empty())
        m_errorString = error;
    if (m_stateCallback)
        m_stateCallback();
}

void SshTunnel::cleanup()
{
    for (auto &relay : m_relays)
        closeRelay(*relay);
    m_relays.clear();
    if (m_listenSocket != ~std::uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(m_listenSocket));
        m_listenSocket = ~std::uintptr_t(0);
    }
    if (m_listener) {
        if (m_sessionValid) {
            std::lock_guard<std::recursive_mutex> locker(
                masterSshLibraryMutex());
            libssh2_channel_forward_cancel(m_listener);
        }
        m_listener = nullptr;
    }
    m_session = nullptr;
}
