#include "state.h"

#include <algorithm>
#include <ctime>
#include <utility>

namespace landrop {
namespace {

constexpr std::size_t kMaxLogEntries = 1500;
constexpr std::size_t kMaxJobRecords = 300;
constexpr std::size_t kMaxPeerRecords = 256;
constexpr auto kPeerExpiry = std::chrono::seconds(8);

const char* stateName(TransferState state) {
    switch (state) {
        case TransferState::Preparing:
            return "preparing";
        case TransferState::Connecting:
            return "connecting";
        case TransferState::Transferring:
            return "transferring";
        case TransferState::Verifying:
            return "verifying";
        case TransferState::Completed:
            return "completed";
        case TransferState::Failed:
            return "failed";
        case TransferState::Canceled:
            return "canceled";
    }
    return "unknown";
}

void speedSample(JobRecord& job, uint64_t bytesDone,
                 std::chrono::steady_clock::time_point now) {
    if (bytesDone < job.lastSampleBytes) {
        // A reconnect/resume offset can move progress backwards. Do not emit
        // a bogus negative/huge rate.
        job.bytesPerSecond = 0.0;
        job.lastSampleTime = now;
        job.lastSampleBytes = bytesDone;
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(now - job.lastSampleTime).count();
    if (elapsed < 0.2) {
        return;
    }
    const double instantaneous =
        static_cast<double>(bytesDone - job.lastSampleBytes) / elapsed;
    if (job.bytesPerSecond <= 0.0) {
        job.bytesPerSecond = instantaneous;
    } else {
        job.bytesPerSecond =
            job.bytesPerSecond * 0.65 + instantaneous * 0.35;
    }
    job.lastSampleTime = now;
    job.lastSampleBytes = bytesDone;
}

}  // namespace

CoreState::CoreState() = default;

void CoreState::setDeviceInfo(std::string deviceName,
                              std::vector<std::string> localIps) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceName_ = std::move(deviceName);
    localIps_ = std::move(localIps);
}

void CoreState::setServerState(bool listening, bool discoveryActive,
                               const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    serverListening_ = listening;
    discoveryActive_ = discoveryActive;
    serviceMessage_ = message;
}

void CoreState::setReceiveDirectory(const fs::path& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    receiveDirectory_ = directory;
}

fs::path CoreState::receiveDirectory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receiveDirectory_;
}

void CoreState::setAllowIncoming(bool allow) {
    std::lock_guard<std::mutex> lock(mutex_);
    allowIncoming_ = allow;
}

bool CoreState::allowIncoming() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allowIncoming_;
}

std::string CoreState::deviceName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deviceName_;
}

uint16_t CoreState::listenPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listenPort_;
}

void CoreState::setListenPort(uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    listenPort_ = port;
}

uint64_t CoreState::addJob(bool outgoing, const std::string& peer,
                           uint32_t itemCount, uint32_t fileCount,
                           uint64_t totalBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.size() >= kMaxJobRecords) {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                   [](const JobRecord& job) {
                                       return !isActiveState(job.state);
                                   }),
                    jobs_.end());
        if (jobs_.size() >= kMaxJobRecords) {
            jobs_.erase(jobs_.begin());
        }
    }
    JobRecord job;
    job.id = nextJobId_++;
    job.outgoing = outgoing;
    job.peer = peer;
    job.itemCount = itemCount;
    job.fileCount = fileCount;
    job.totalBytes = totalBytes;
    job.state = TransferState::Preparing;
    job.status = stateName(job.state);
    job.lastSampleTime = std::chrono::steady_clock::now();
    job.startedAt = job.lastSampleTime;
    jobs_.push_back(job);
    return job.id;
}

void CoreState::updateJob(
    uint64_t id, const std::function<void(JobRecord&)>& update) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& job : jobs_) {
        if (job.id == id) {
            const auto now = std::chrono::steady_clock::now();
            update(job);
            if (isActiveState(job.state)) {
                speedSample(job, job.bytesDone, now);
            } else {
                job.bytesPerSecond = 0.0;
            }
            return;
        }
    }
}

void CoreState::finishJob(uint64_t id, TransferState state,
                          const std::string& status,
                          const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& job : jobs_) {
        if (job.id == id) {
            job.state = state;
            job.status = status;
            job.error = error;
            job.bytesPerSecond = 0.0;
            return;
        }
    }
}

void CoreState::clearFinishedJobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const JobRecord& job) {
                                   return !isActiveState(job.state);
                               }),
                jobs_.end());
}

std::vector<TransferView> CoreState::transferViews() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TransferView> result;
    result.reserve(jobs_.size());
    for (const auto& job : jobs_) {
        TransferView view;
        view.id = job.id;
        view.outgoing = job.outgoing;
        view.peer = job.peer;
        view.currentFile = job.currentFile;
        view.status = job.status;
        view.error = job.error;
        view.bytesDone = job.bytesDone;
        view.totalBytes = job.totalBytes;
        view.bytesPerSecond = job.bytesPerSecond;
        view.fileCount = job.fileCount;
        view.itemCount = job.itemCount;
        view.state = job.state;
        result.push_back(std::move(view));
    }
    std::sort(result.begin(), result.end(),
              [](const TransferView& a, const TransferView& b) {
                  return a.id > b.id;
              });
    return result;
}

bool CoreState::hasActiveJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](const JobRecord& job) {
        return isActiveState(job.state);
    });
}

bool CoreState::isJobActive(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(),
                       [id](const JobRecord& job) {
                           return job.id == id && isActiveState(job.state);
                       });
}

void CoreState::log(LogLevel level, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    LogEntry entry;
    entry.sequence = nextLogSequence_++;
    entry.level = level;
    entry.unixTime = static_cast<int64_t>(std::time(nullptr));
    entry.text = text;
    logs_.push_back(std::move(entry));
    while (logs_.size() > kMaxLogEntries) {
        logs_.pop_front();
    }
}

std::vector<LogEntry> CoreState::logsAfter(uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogEntry> result;
    for (const auto& entry : logs_) {
        if (entry.sequence > sequence) {
            result.push_back(entry);
        }
    }
    return result;
}

uint64_t CoreState::latestLogSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextLogSequence_ > 0 ? nextLogSequence_ - 1 : 0;
}

void CoreState::peerSeen(const std::string& id, const std::string& name,
                         const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (now - it->second.lastSeen > kPeerExpiry) {
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    if (peers_.find(id) == peers_.end() &&
        peers_.size() >= kMaxPeerRecords) {
        return;
    }
    PeerRecord& peer = peers_[id];
    peer.id = id;
    peer.name = name;
    peer.ip = ip;
    peer.port = port;
    peer.lastSeen = now;
}

std::vector<PeerView> CoreState::peerViews() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::vector<PeerView> result;
    for (const auto& pair : peers_) {
        if (now - pair.second.lastSeen > kPeerExpiry) {
            continue;
        }
        PeerView peer;
        peer.id = pair.second.id;
        peer.name = pair.second.name;
        peer.ip = pair.second.ip;
        peer.port = pair.second.port;
        result.push_back(std::move(peer));
    }
    std::sort(result.begin(), result.end(),
              [](const PeerView& a, const PeerView& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.ip < b.ip;
              });
    return result;
}

UiSnapshot CoreState::snapshot() const {
    UiSnapshot result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.deviceName = deviceName_;
        result.localIps = localIps_;
        result.listenPort = listenPort_;
        result.serverListening = serverListening_;
        result.discoveryActive = discoveryActive_;
        result.serviceMessage = serviceMessage_;
        result.allowIncoming = allowIncoming_;
        result.receiveDirectory = receiveDirectory_;
    }
    result.transfers = transferViews();
    result.peers = peerViews();
    return result;
}

}  // namespace landrop
