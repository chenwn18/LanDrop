#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace landrop {

class CoreState;

class DiscoveryService {
public:
    DiscoveryService(CoreState& state, std::string instanceId,
                     std::string deviceName, uint16_t tcpPort,
                     uint16_t discoveryPort);
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace landrop
