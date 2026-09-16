#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SshProxyRelay final
{
public:
    using ReadyCallback = std::function<void(std::uint16_t localPort,
                                             const std::string &error)>;

    SshProxyRelay();
    ~SshProxyRelay();

    bool start(const std::string &jumpSpec, const std::string &targetHost, int targetPort,
               const std::string &password, const std::string &privateKeyPath,
               ReadyCallback callback);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
