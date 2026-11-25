#include "app_controller.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <thread>
#include <utility>

#include "discovery.h"
#include "platform.h"
#include "receiver.h"
#include "sender.h"
#include "settings.h"
#include "util.h"

namespace landrop {
namespace {

struct TargetAddress {
    std::string host;
    uint16_t port = kDefaultTcpPort;
};

bool parseTarget(const std::string& input, TargetAddress& target,
                 std::string& error) {
    std::string value = trim(input);
    if (value.empty()) {
        error = "请输入目标 IP 地址";
        return false;
    }
    if (value.find("://") != std::string::npos) {
        error = "请输入 IP 地址，不要包含 http:// 等前缀";
        return false;
    }
    const std::size_t colonCount =
        static_cast<std::size_t>(std::count(value.begin(), value.end(), ':'));
    if (colonCount > 1) {
        error = "当前版本仅支持 IPv4 地址";
        return false;
    }
    if (colonCount == 1) {
        const std::size_t colon = value.find(':');
        const std::string portText = value.substr(colon + 1);
        value.resize(colon);
        if (portText.empty()) {
            error = "端口号为空";
            return false;
        }
        unsigned int port = 0;
        for (char c : portText) {
            if (c < '0' || c > '9') {
                error = "端口号只能包含数字";
                return false;
            }
            port = port * 10 + static_cast<unsigned int>(c - '0');
            if (port > 65535) {
                error = "端口号超出范围";
                return false;
            }
        }
        if (port == 0) {
            error = "端口号不能为 0";
            return false;
        }
        target.port = static_cast<uint16_t>(port);
    }
    value = trim(value);
    if (value.empty() || value.size() > 253) {
        error = "目标地址无效";
        return false;
    }
    for (unsigned char c : value) {
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '\\' ||
            c == '@' || c == '?' || c == '#') {
            error = "目标地址包含非法字符";
            return false;
        }
    }
    target.host = value;
    return true;
}

std::string joinMessages(const std::vector<std::string>& messages) {
    std::ostringstream stream;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            stream << '\n';
        }
        stream << messages[i];
    }
    return stream.str();
}

}  // namespace

AppController::AppController() = default;

AppController::~AppController() { stop(); }

bool AppController::start(const ControllerOptions& options,
                          std::string& warning) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (started_) {
        warning.clear();
        return true;
    }
    warning.clear();
    std::string networkError;
    if (!platform::initializeNetwork(networkError)) {
        state_.log(LogLevel::Error, networkError);
        warning = networkError;
        return false;
    }
    shuttingDown_ = false;

    std::vector<std::string> warnings;
    auto addWarning = [&](const std::string& message) {
        warnings.push_back(message);
        state_.log(LogLevel::Warning, message);
    };

    std::string deviceName = trim(options.deviceName);
    if (deviceName.empty()) {
        deviceName = platform::hostName();
    }
    if (deviceName.empty() || !isValidUtf8(deviceName)) {
        deviceName = "LanDrop";
    }
    deviceName = truncateUtf8(deviceName, 120);

    fs::path receiveDirectory = options.receiveDirectory;
    if (receiveDirectory.empty()) {
        receiveDirectory = platform::defaultReceiveDirectory();
    }
    auto normalizeDirectory = [](const fs::path& value, fs::path& result,
                                 std::string& error) {
        if (value.empty()) {
            error = "接收目录为空";
            return false;
        }
        std::error_code ec;
        fs::path absolute = fs::absolute(value, ec);
        if (ec) {
            error = "无法解析接收目录: " + pathToUtf8(value) + " — " +
                    platform::errorCodeMessage(ec);
            return false;
        }
        absolute = absolute.lexically_normal();
        if (!ensureDirectories(absolute, error)) {
            return false;
        }
        result = absolute;
        return true;
    };
    std::string directoryError;
    fs::path normalizedDirectory;
    if (!normalizeDirectory(receiveDirectory, normalizedDirectory,
                            directoryError)) {
        addWarning(directoryError + "，已改用默认接收目录");
        receiveDirectory = platform::defaultReceiveDirectory();
        if (!normalizeDirectory(receiveDirectory, normalizedDirectory,
                                directoryError)) {
            addWarning(directoryError + "，已改用配置目录下的 Inbox");
            receiveDirectory = platform::configDirectory() / "Inbox";
            std::string finalError;
            if (!normalizeDirectory(receiveDirectory, normalizedDirectory,
                                    finalError)) {
                addWarning(finalError);
                normalizedDirectory = platform::configDirectory();
                (void)ensureDirectories(normalizedDirectory, finalError);
            }
        }
    }

    state_.setDeviceInfo(deviceName, platform::localIpv4Addresses());
    state_.setReceiveDirectory(normalizedDirectory);
    state_.setAllowIncoming(options.allowIncoming);

    std::string receiveError;
    receiver_ = std::make_unique<ReceiverServer>(state_, options.listenPort);
    if (!receiver_->start(receiveError)) {
        addWarning("接收服务启动失败: " + receiveError);
        receiver_.reset();
    }
    const uint16_t actualPort =
        receiver_ && receiver_->running() ? receiver_->port() : 0;
    state_.setListenPort(actualPort);

    if (receiver_ && receiver_->running() && options.enableDiscovery) {
        discovery_ = std::make_unique<DiscoveryService>(
            state_, randomHex(16), deviceName, actualPort,
            options.discoveryPort);
        std::string discoveryError;
        if (!discovery_->start(discoveryError)) {
            addWarning("设备发现启动失败: " + discoveryError);
            discovery_.reset();
        }
    }

    const bool listening = receiver_ && receiver_->running();
    const bool discovering = discovery_ && discovery_->running();
    const std::string serviceMessage =
        warnings.empty()
            ? (listening ? "接收服务运行中" : "仅发送模式")
            : joinMessages(warnings);
    state_.setServerState(listening, discovering, serviceMessage);
    state_.log(LogLevel::Info,
               std::string(kAppName) + " 已启动，设备名: " + deviceName);
    if (listening) {
        state_.log(LogLevel::Info,
                   "监听 TCP 端口 " + std::to_string(actualPort) +
                       "，接收目录: " + pathToUtf8(normalizedDirectory));
    }
    if (discovering) {
        state_.log(LogLevel::Info,
                   "局域网设备发现已开启（UDP " +
                       std::to_string(options.discoveryPort) + "）");
    }
    warning = serviceMessage == "接收服务运行中" ? "" : serviceMessage;
    started_ = true;
    return true;
}

void AppController::stop() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!started_ && !receiver_ && !discovery_ && activeOutgoing_ == 0) {
            return;
        }
        shuttingDown_ = true;
        for (const auto& item : cancels_) {
            item.second->store(true);
        }
    }

    if (discovery_) {
        discovery_->stop();
    }
    if (receiver_) {
        receiver_->stop();
    }

    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        idleCondition_.wait(lock, [this]() { return activeOutgoing_ == 0; });
        discovery_.reset();
        receiver_.reset();
        cancels_.clear();
        started_ = false;
        shuttingDown_ = false;
    }
    state_.setServerState(false, false, "服务已停止");
    state_.log(LogLevel::Info, "LanDrop 已停止");
}

bool AppController::running() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return started_;
}

uint16_t AppController::listenPort() const { return state_.listenPort(); }

UiSnapshot AppController::snapshot() const { return state_.snapshot(); }

std::vector<LogEntry> AppController::logsAfter(uint64_t sequence) const {
    return state_.logsAfter(sequence);
}

uint64_t AppController::send(
    const std::string& targetInput,
    const std::vector<std::filesystem::path>& paths, std::string& error) {
    TargetAddress target;
    if (!parseTarget(targetInput, target, error)) {
        return 0;
    }
    if (paths.empty()) {
        error = "请先选择文件或文件夹";
        return 0;
    }
    for (const auto& path : paths) {
        if (path.empty()) {
            error = "选择列表中存在空路径";
            return 0;
        }
    }

    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!started_ || shuttingDown_) {
        error = "LanDrop 尚未启动或正在关闭";
        return 0;
    }
    const uint64_t jobId = state_.addJob(true, target.host, 0, 0, 0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancels_[jobId] = cancel;
    ++activeOutgoing_;
    state_.updateJob(jobId, [](JobRecord& job) {
        job.state = TransferState::Preparing;
        job.status = "排队中";
    });
    state_.log(LogLevel::Info,
               "已加入发送队列，目标 " + target.host + ":" +
                   std::to_string(target.port));

    SendRequest request;
    request.jobId = jobId;
    request.host = target.host;
    request.port = target.port;
    request.deviceName = state_.deviceName();
    request.selectedPaths = paths;
    request.cancel = cancel;
    try {
        std::thread([this, request]() {
            try {
                Sender sender(state_, request);
                sender.run();
            } catch (const std::exception& exception) {
                state_.finishJob(request.jobId, TransferState::Failed,
                                 "发送失败", exception.what());
                state_.log(LogLevel::Error,
                           std::string("发送线程异常: ") + exception.what());
            }
            outgoingWorkerFinished(request.jobId);
        }).detach();
    } catch (const std::exception& exception) {
        cancels_.erase(jobId);
        --activeOutgoing_;
        state_.finishJob(jobId, TransferState::Failed, "发送失败",
                         exception.what());
        error = std::string("无法创建发送线程: ") + exception.what();
        return 0;
    }
    return jobId;
}

void AppController::cancel(uint64_t jobId) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const auto iterator = cancels_.find(jobId);
    if (iterator == cancels_.end() || !state_.isJobActive(jobId)) {
        return;
    }
    iterator->second->store(true);
    state_.updateJob(jobId, [](JobRecord& job) {
        job.status = "正在取消";
    });
}

void AppController::clearFinished() { state_.clearFinishedJobs(); }

bool AppController::setReceiveDirectory(
    const std::filesystem::path& directory, std::string& error) {
    if (directory.empty()) {
        error = "接收目录为空";
        return false;
    }
    std::error_code ec;
    fs::path absolute = fs::absolute(directory, ec);
    if (ec) {
        error = "无法解析接收目录: " + platform::errorCodeMessage(ec);
        return false;
    }
    absolute = absolute.lexically_normal();
    if (!ensureDirectories(absolute, error)) {
        return false;
    }
    state_.setReceiveDirectory(absolute);
    state_.log(LogLevel::Info,
               "接收目录已更改为: " + pathToUtf8(absolute));
    return true;
}

void AppController::setAllowIncoming(bool allow) {
    const bool previous = state_.allowIncoming();
    state_.setAllowIncoming(allow);
    if (previous != allow) {
        state_.log(LogLevel::Info,
                   allow ? "已开启文件接收" : "已关闭文件接收");
    }
}

std::filesystem::path AppController::receiveDirectory() const {
    return state_.receiveDirectory();
}

void AppController::outgoingWorkerFinished(uint64_t jobId) {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        cancels_.erase(jobId);
        if (activeOutgoing_ > 0) {
            --activeOutgoing_;
        }
        if (activeOutgoing_ == 0) {
            idleCondition_.notify_all();
        }
    }
}

}  // namespace landrop
