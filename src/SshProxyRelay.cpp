#include "SshProxyRelay.h"
#include "SshLibrary.h"
#include "NativeDataDir.h"
#include "DiagnosticLog.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <libssh2.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace {

std::string trim(std::string value)
{
    const auto isSpace = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\r'
            || character == '\n';
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](unsigned char character) {
                                                return !isSpace(character);
                                            }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](unsigned char character) {
                                 return !isSpace(character);
                             }).base(), value.end());
    return value;
}

std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

void proxyDiagnosticLog(const std::string &message)
{
    const std::filesystem::path path =
        executableDirectory() / "MasterSSH-sftp-worker.log";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file)
        return;
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localTime = *std::localtime(&time);
#endif
    file << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S")
         << ".000  proxy-relay " << message << '\n';
}

std::string sessionError(LIBSSH2_SESSION *session)
{
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    return message && length > 0 ? std::string(message, length)
                                 : std::string("未知 SSH 错误");
}

bool parseJump(const std::string &spec, std::string *user,
               std::string *host, int *port)
{
    const std::size_t at = spec.find('@');
    if (at == std::string::npos || at == 0)
        return false;
    *user = trim(spec.substr(0, at));
    std::string hostPort = trim(spec.substr(at + 1));
    *port = 22;
    const std::size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && colon > 0) {
        try {
            const int parsed = std::stoi(hostPort.substr(colon + 1));
            if (parsed > 0 && parsed <= 65535) {
                *port = parsed;
                hostPort.resize(colon);
            }
        } catch (...) {
            return false;
        }
    }
    *host = trim(hostPort);
    return !user->empty() && !host->empty();
}

#ifdef _WIN32
bool connectBlocking(const std::string &host, int port, SOCKET *result,
                     std::string *error)
{
    *result = INVALID_SOCKET;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        *error = "无法解析跳板机地址";
        return false;
    }
    for (addrinfo *entry = addresses; entry; entry = entry->ai_next) {
        SOCKET socket = ::socket(entry->ai_family, entry->ai_socktype,
                                 entry->ai_protocol);
        if (socket == INVALID_SOCKET)
            continue;
        DWORD timeout = 10000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        if (::connect(socket, entry->ai_addr,
                      static_cast<int>(entry->ai_addrlen)) == 0) {
            *result = socket;
            freeaddrinfo(addresses);
            return true;
        }
        closesocket(socket);
    }
    freeaddrinfo(addresses);
    *error = "无法连接跳板机";
    return false;
}

std::filesystem::path knownHostsPath()
{
    return NativeDataDir::knownHostsFile();
}

bool verifyHost(LIBSSH2_SESSION *session, const std::string &host, int port,
                std::string *error)
{
    const std::filesystem::path path = knownHostsPath();
    const std::string filename = path.string();
    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        *error = "无法初始化跳板机主机密钥校验";
        return false;
    }
    std::error_code fileError;
    if (std::filesystem::exists(path, fileError))
        libssh2_knownhost_readfile(hosts, filename.c_str(),
                                    LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    size_t length = 0;
    int type = 0;
    const char *key = libssh2_session_hostkey(session, &length, &type);
    int keyType = LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    if (type == LIBSSH2_HOSTKEY_TYPE_RSA)
        keyType = LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    else if (type == LIBSSH2_HOSTKEY_TYPE_ED25519)
        keyType = LIBSSH2_KNOWNHOST_KEY_ED25519;
    else if (type == LIBSSH2_HOSTKEY_TYPE_ECDSA_256)
        keyType = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    const std::string name = port == 22
        ? host : "[" + host + "]:" + std::to_string(port);
    const int flags = LIBSSH2_KNOWNHOST_TYPE_PLAIN
        | LIBSSH2_KNOWNHOST_KEYENC_RAW | keyType;
    libssh2_knownhost *matched = nullptr;
    const int check = key
        ? libssh2_knownhost_check(hosts, name.c_str(), key, length, flags,
                                  &matched)
        : LIBSSH2_KNOWNHOST_CHECK_FAILURE;
    bool valid = check == LIBSSH2_KNOWNHOST_CHECK_MATCH;
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        valid = libssh2_knownhost_addc(hosts, name.c_str(), nullptr, key,
                                       length, nullptr, 0, flags, nullptr) == 0;
        if (valid) {
            std::filesystem::create_directories(path.parent_path(), fileError);
            valid = libssh2_knownhost_writefile(
                hosts, filename.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0;
        }
    }
    if (!valid)
        *error = check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH
            ? "跳板机主机密钥不匹配" : "无法校验跳板机主机密钥";
    libssh2_knownhost_free(hosts);
    return valid;
}
#endif

} // namespace

struct SshProxyRelay::Impl
{
    std::thread thread;
    std::atomic_bool stopping{false};
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    // The jump socket is a member (not a local) so stop() can close it and
    // interrupt a blocking connect/handshake/auth in the relay thread.  The
    // app must never join a relay thread that is stuck in a 10 s socket
    // timeout during shutdown.
    SOCKET jumpSocket = INVALID_SOCKET;
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_CHANNEL *channel = nullptr;
    ReadyCallback callback;
    std::string jumpSpec, targetHost, password, keyPath;
    int targetPort = 22;
    std::uint16_t localPort = 0;

    void closeSockets()
    {
        if (client != INVALID_SOCKET) {
            closesocket(client);
            client = INVALID_SOCKET;
        }
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
        if (jumpSocket != INVALID_SOCKET) {
            closesocket(jumpSocket);
            jumpSocket = INVALID_SOCKET;
        }
    }

    void run()
    {
#ifdef _WIN32
        DiagnosticLog::registerThread("proxy-relay");
        std::string user, jumpHost, error;
        int jumpPort = 22;
        proxyDiagnosticLog("start jump=" + jumpSpec + " target=" + targetHost
                           + ":" + std::to_string(targetPort));
        if (!parseJump(jumpSpec, &user, &jumpHost, &jumpPort)) {
            callback(0, "跳板机格式应为 user@host[:port]");
            return;
        }
        proxyDiagnosticLog("parsed jump=" + user + "@" + jumpHost + ":"
                           + std::to_string(jumpPort));
        if (!masterSshInitializeLibraries()) {
            callback(0, "无法初始化内置 SSH 库");
            return;
        }
        if (!connectBlocking(jumpHost, jumpPort, &jumpSocket, &error)) {
            callback(0, error);
            return;
        }
        proxyDiagnosticLog("tcp connected");
        session = libssh2_session_init();
        if (!session) {
            closesocket(jumpSocket);
            callback(0, "无法创建跳板机 SSH 会话");
            return;
        }
        libssh2_session_set_blocking(session, 1);
        const int handshakeResult = libssh2_session_handshake(session, jumpSocket);
        proxyDiagnosticLog("handshake result=" + std::to_string(handshakeResult));
        if (handshakeResult != 0)
            error = sessionError(session);
        if (error.empty() && !verifyHost(session, jumpHost, jumpPort, &error)) {}
        proxyDiagnosticLog("host key error=" + error);
        if (error.empty()) {
            const int result = keyPath.empty()
                ? libssh2_userauth_password_ex(
                    session, user.c_str(), user.size(), password.c_str(),
                    password.size(), nullptr)
                : libssh2_userauth_publickey_fromfile_ex(
                    session, user.c_str(), user.size(), nullptr, keyPath.c_str(),
                    password.empty() ? nullptr : password.c_str());
            if (result != 0)
                error = "跳板机身份验证失败：" + sessionError(session);
            proxyDiagnosticLog("auth result=" + std::to_string(result)
                               + " error=" + error);
        }
        if (error.empty()) {
            channel = libssh2_channel_direct_tcpip_ex(
                session, targetHost.c_str(), targetPort, "127.0.0.1", 0);
            if (!channel)
                error = "无法通过跳板机建立目标通道：" + sessionError(session);
            proxyDiagnosticLog(std::string("direct channel=")
                               + (channel ? "ok" : "failed")
                               + " error=" + error);
        }
        if (!error.empty()) {
            libssh2_session_free(session);
            session = nullptr;
            if (jumpSocket != INVALID_SOCKET) {
                closesocket(jumpSocket);
                jumpSocket = INVALID_SOCKET;
            }
            callback(0, error);
            return;
        }
        libssh2_session_set_blocking(session, 0);
        callback(localPort, {});
        sockaddr_storage ignored{};
        int ignoredLength = sizeof(ignored);
        while (!stopping.load() && client == INVALID_SOCKET) {
            client = accept(listener, reinterpret_cast<sockaddr *>(&ignored),
                            &ignoredLength);
            if (client == INVALID_SOCKET)
                Sleep(5);
        }
        if (client == INVALID_SOCKET) {
            libssh2_channel_free(channel);
            channel = nullptr;
            libssh2_session_free(session);
            session = nullptr;
            if (jumpSocket != INVALID_SOCKET) {
                closesocket(jumpSocket);
                jumpSocket = INVALID_SOCKET;
            }
            return;
        }
        u_long clientNonBlocking = 1;
        ioctlsocket(client, FIONBIO, &clientNonBlocking);
        std::string toRemote, toLocal;
        while (!stopping.load()) {
            char buffer[16384];
            const int readLocal = recv(client, buffer, sizeof(buffer), 0);
            if (readLocal > 0)
                toRemote.append(buffer, readLocal);
            else if (readLocal == 0
                     || (readLocal < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
                break;
            while (!toRemote.empty()) {
                const ssize_t written = libssh2_channel_write(
                    channel, toRemote.data(), toRemote.size());
                if (written == LIBSSH2_ERROR_EAGAIN)
                    break;
                if (written < 0) {
                    stopping = true;
                    break;
                }
                toRemote.erase(0, static_cast<std::size_t>(written));
            }
            const ssize_t readRemote = libssh2_channel_read(
                channel, buffer, sizeof(buffer));
            if (readRemote > 0)
                toLocal.append(buffer, static_cast<std::size_t>(readRemote));
            else if (readRemote < 0 && readRemote != LIBSSH2_ERROR_EAGAIN)
                break;
            if (!toLocal.empty()) {
                const int written = send(client, toLocal.data(),
                                         static_cast<int>(toLocal.size()), 0);
                if (written > 0)
                    toLocal.erase(0, static_cast<std::size_t>(written));
                else if (written == 0
                         || (written < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
                    break;
            }
            Sleep(5);
        }
        libssh2_channel_free(channel);
        channel = nullptr;
        libssh2_session_free(session);
        session = nullptr;
        if (jumpSocket != INVALID_SOCKET) {
            closesocket(jumpSocket);
            jumpSocket = INVALID_SOCKET;
        }
        closeSockets();
#else
        callback(0, "内置 ProxyJump 当前仅支持 Windows");
#endif
    }
};

SshProxyRelay::SshProxyRelay() : m_impl(std::make_unique<Impl>()) {}
SshProxyRelay::~SshProxyRelay() { stop(); }

bool SshProxyRelay::start(const std::string &jumpSpec,
                          const std::string &targetHost, int targetPort,
                          const std::string &password,
                          const std::string &privateKeyPath,
                          ReadyCallback callback)
{
#ifdef _WIN32
    if (m_impl->thread.joinable())
        return false;
    m_impl->jumpSpec = jumpSpec;
    m_impl->targetHost = targetHost;
    m_impl->targetPort = targetPort;
    m_impl->password = password;
    m_impl->keyPath = privateKeyPath;
    m_impl->callback = std::move(callback);
    m_impl->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->listener == INVALID_SOCKET)
        return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(m_impl->listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0
        || listen(m_impl->listener, 1) != 0) {
        m_impl->closeSockets();
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(m_impl->listener, FIONBIO, &nonBlocking);
    sockaddr_in bound{};
    int length = sizeof(bound);
    getsockname(m_impl->listener, reinterpret_cast<sockaddr *>(&bound), &length);
    m_impl->localPort = ntohs(bound.sin_port);
    m_impl->thread = std::thread([impl = m_impl.get()] { impl->run(); });
    return true;
#else
    (void)jumpSpec;
    (void)targetHost;
    (void)targetPort;
    (void)password;
    (void)privateKeyPath;
    (void)callback;
    return false;
#endif
}

void SshProxyRelay::stop()
{
    if (!m_impl)
        return;
    m_impl->stopping = true;
    m_impl->closeSockets();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
    m_impl->stopping = false;
}
