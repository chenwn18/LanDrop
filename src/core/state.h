#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace landrop {

namespace fs = std::filesystem;

enum class TransferState {
    Preparing,
    Connecting,
    Transferring,
    Verifying,
    Completed,
    Failed,
    Canceled,
};

inline bool isActiveState(TransferState state) {
    return state == TransferState::Preparing ||
           state == TransferState::Connecting ||
           state == TransferState::Transferring ||
           state == TransferState::Verifying;
}

enum class LogLevel { Info, LogSuccess, Warning, Error };

struct LogEntry {
    uint64_t sequence = 0;
    LogLevel level = LogLevel::Info;
    int64_t unixTime = 0;
    std::string text;
};

struct TransferView {
    uint64_t id = 0;
    bool outgoing = false;
    std::string peer;
    std::string currentFile;
    std::string status;
    std::string error;
    uint64_t bytesDone = 0;
    uint64_t totalBytes = 0;
    double bytesPerSecond = 0.0;
    uint32_t fileCount = 0;
    uint32_t itemCount = 0;
    TransferState state = TransferState::Preparing;
};

struct PeerView {
    std::string id;
    std::string name;
    std::string ip;
    uint16_t port = 0;
};

struct UiSnapshot {
    std::string deviceName;
    std::vector<std::string> localIps;
    uint16_t listenPort = 0;
    bool serverListening = false;
    bool discoveryActive = false;
    std::string serviceMessage;
    bool allowIncoming = true;
    fs::path receiveDirectory;
    std::vector<TransferView> transfers;
    std::vector<PeerView> peers;
};

// Internal mutable record used by sender/receiver workers.  A snapshot is
// converted to TransferView before it leaves the core.
struct JobRecord {
    uint64_t id = 0;
    bool outgoing = false;
    std::string peer;
    std::string currentFile;
    std::string status;
    std::string error;
    uint64_t bytesDone = 0;
    uint64_t totalBytes = 0;
    double bytesPerSecond = 0.0;
    uint32_t fileCount = 0;
    uint32_t itemCount = 0;
    TransferState state = TransferState::Preparing;
    std::chrono::steady_clock::time_point lastSampleTime;
    uint64_t lastSampleBytes = 0;
    std::chrono::steady_clock::time_point startedAt;
};

class CoreState {
public:
    CoreState();

    void setDeviceInfo(std::string deviceName, std::vector<std::string> localIps);
    void setServerState(bool listening, bool discoveryActive,
                        const std::string& message);
    void setReceiveDirectory(const fs::path& directory);
    fs::path receiveDirectory() const;
    void setAllowIncoming(bool allow);
    bool allowIncoming() const;
    std::string deviceName() const;
    uint16_t listenPort() const;
    void setListenPort(uint16_t port);

    uint64_t addJob(bool outgoing, const std::string& peer,
                    uint32_t itemCount, uint32_t fileCount,
                    uint64_t totalBytes);
    void updateJob(uint64_t id, const std::function<void(JobRecord&)>& update);
    void finishJob(uint64_t id, TransferState state, const std::string& status,
                   const std::string& error = {});
    void clearFinishedJobs();
    std::vector<TransferView> transferViews() const;
    bool hasActiveJobs() const;
    bool isJobActive(uint64_t id) const;

    void log(LogLevel level, const std::string& text);
    std::vector<LogEntry> logsAfter(uint64_t sequence) const;
    uint64_t latestLogSequence() const;

    void peerSeen(const std::string& id, const std::string& name,
                  const std::string& ip, uint16_t port);
    std::vector<PeerView> peerViews() const;

    UiSnapshot snapshot() const;

private:
    static void refreshSpeed(JobRecord& job);

    mutable std::mutex mutex_;
    std::string deviceName_;
    std::vector<std::string> localIps_;
    uint16_t listenPort_ = 0;
    bool serverListening_ = false;
    bool discoveryActive_ = false;
    std::string serviceMessage_;
    fs::path receiveDirectory_;
    bool allowIncoming_ = true;

    uint64_t nextJobId_ = 1;
    std::vector<JobRecord> jobs_;

    uint64_t nextLogSequence_ = 1;
    std::deque<LogEntry> logs_;

    struct PeerRecord {
        std::string id;
        std::string name;
        std::string ip;
        uint16_t port = 0;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::unordered_map<std::string, PeerRecord> peers_;
};

}  // namespace landrop
