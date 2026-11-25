#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "state.h"

namespace landrop {

class ReceiverServer;
class DiscoveryService;

struct ControllerOptions {
    std::filesystem::path receiveDirectory;
    std::string deviceName;
    uint16_t listenPort = kDefaultTcpPort;
    uint16_t discoveryPort = kDefaultDiscoveryPort;
    bool allowIncoming = true;
    bool enableDiscovery = true;
};

class AppController {
public:
    AppController();
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // Returns false only for fatal initialization (e.g. Winsock).  Non-fatal
    // problems are reported through `warning` and the UI remains usable for
    // sending.
    bool start(const ControllerOptions& options, std::string& warning);
    void stop();
    bool running() const;
    uint16_t listenPort() const;

    UiSnapshot snapshot() const;
    std::vector<LogEntry> logsAfter(uint64_t sequence) const;

    uint64_t send(const std::string& target,
                  const std::vector<std::filesystem::path>& paths,
                  std::string& error);
    void cancel(uint64_t jobId);
    void clearFinished();

    bool setReceiveDirectory(const std::filesystem::path& directory,
                             std::string& error);
    void setAllowIncoming(bool allow);
    std::filesystem::path receiveDirectory() const;

private:
    void outgoingWorkerFinished(uint64_t jobId);

    CoreState state_;
    std::unique_ptr<ReceiverServer> receiver_;
    std::unique_ptr<DiscoveryService> discovery_;

    mutable std::mutex lifecycleMutex_;
    std::condition_variable idleCondition_;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancels_;
    int activeOutgoing_ = 0;
    bool started_ = false;
    bool shuttingDown_ = false;
};

}  // namespace landrop
