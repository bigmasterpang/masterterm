#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_LISTENER;

// One SSH port forwarding rule bound to an established SSH session.
// Local mode listens on the local machine and forwards to a remote target
// through the server; remote mode asks the server to listen and forwards
// accepted connections to a local target.  All sockets and libssh2 channel
// I/O are non-blocking and pumped from the backend poll loop.
class SshTunnel final
{
public:
    enum class Mode { Local, Remote };
    enum class State { Starting, Listening, Failed, Stopped };

    using StateCallback = std::function<void()>;

    SshTunnel(Mode mode, std::string listenHost, int listenPort,
              std::string targetHost, int targetPort);
    ~SshTunnel();

    void start(_LIBSSH2_SESSION *session, StateCallback callback);
    void stop();
    // The owning SSH session is gone; drop libssh2 state without touching the
    // already-freed session object.
    void invalidate();
    void poll();

    const std::string &id() const { return m_id; }
    Mode mode() const { return m_mode; }
    State state() const { return m_state; }
    const std::string &errorString() const { return m_errorString; }
    int listenPort() const { return m_listenPort; }
    int boundPort() const { return m_boundPort; }
    const std::string &targetHost() const { return m_targetHost; }
    int targetPort() const { return m_targetPort; }
    bool active() const
    {
        return m_state == State::Listening;
    }

private:
    struct Relay
    {
        std::uintptr_t clientSocket = ~std::uintptr_t(0);
        _LIBSSH2_CHANNEL *channel = nullptr;
        bool channelPending = false;
        bool connectPending = false;
        bool remoteEof = false;
        bool clientEof = false;
        std::string pendingToClient;
        std::string pendingToRemote;
    };

    bool startLocalListener();
    bool startRemoteListener();
    void acceptLocalConnections();
    void acceptRemoteConnections();
    void pollRelays();
    void pumpRelay(Relay &relay);
    void pumpChannelToClient(Relay &relay);
    void pumpClientToChannel(Relay &relay);
    void closeRelay(Relay &relay);
    void setState(State state, const std::string &error = {});
    void cleanup();

    std::string m_id;
    Mode m_mode;
    std::string m_listenHost;
    int m_listenPort = 0;
    std::string m_targetHost;
    int m_targetPort = 0;
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_LISTENER *m_listener = nullptr;
    std::uintptr_t m_listenSocket = ~std::uintptr_t(0);
    int m_boundPort = 0;
    State m_state = State::Starting;
    std::string m_errorString;
    StateCallback m_stateCallback;
    bool m_sessionValid = false;
    std::vector<std::unique_ptr<Relay>> m_relays;
};
