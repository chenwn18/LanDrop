#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace landrop {

class CoreState;

class ReceiverServer {
public:
    ReceiverServer(CoreState& state, uint16_t requestedPort);
    ~ReceiverServer();

    ReceiverServer(const ReceiverServer&) = delete;
    ReceiverServer& operator=(const ReceiverServer&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const;
    uint16_t port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace landrop
