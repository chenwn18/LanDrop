#include "sender.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include "common.h"
#include "platform.h"
#include "protocol.h"
#include "scanner.h"
#include "socket.h"
#include "util.h"

namespace landrop {
namespace {

constexpr int kMaxNetworkRetries = 3;
constexpr int kMaxBusyRetries = 8;
constexpr int kMaxIntegrityRetries = 1;
constexpr auto kUiUpdateInterval = std::chrono::milliseconds(120);

std::string truncateUtf8(std::string text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    text.resize(maxBytes);
    while (!text.empty() && !isValidUtf8(text)) {
        text.pop_back();
    }
    return text;
}

ErrorKind kindFromRemote(proto::ErrorCode code) {
    switch (code) {
        case proto::ErrorCode::Busy:
            return ErrorKind::Busy;
        case proto::ErrorCode::Integrity:
            return ErrorKind::Integrity;
        case proto::ErrorCode::Cancelled:
            return ErrorKind::Cancelled;
        case proto::ErrorCode::Protocol:
            return ErrorKind::Protocol;
        case proto::ErrorCode::IncomingDisabled:
        case proto::ErrorCode::PathRejected:
        case proto::ErrorCode::Disk:
        case proto::ErrorCode::NoSpace:
        case proto::ErrorCode::Internal:
        case proto::ErrorCode::NoError:
        default:
            return ErrorKind::Rejected;
    }
}

bool waitWithCancellation(const std::shared_ptr<std::atomic<bool>>& cancel,
                          int milliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel->load()) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        platform::sleepMilliseconds(
            static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(100, remaining))));
    }
    return !cancel->load();
}

class ClientAttempt {
public:
    ClientAttempt(CoreState& state, const SendRequest& request,
                  const ScanResult& scan)
        : state_(state),
          request_(request),
          scan_(scan),
          cancel_(request.cancel),
          channel_(nullptr) {}

    void execute() {
        state_.updateJob(request_.jobId, [](JobRecord& job) {
            job.state = TransferState::Connecting;
            job.status = "正在连接";
        });

        if (!socket_.connectTo(request_.host, request_.port, 6000,
                               cancel_.get(), connectError_)) {
            if (cancelled()) {
                throw Error(ErrorKind::Cancelled, "操作已取消");
            }
            throw Error(ErrorKind::Network, connectError_);
        }
        channel_.reset(new proto::Channel(socket_, cancel_.get()));
        performHandshake();
        sendManifest();
        std::vector<uint64_t> offsets = receiveResumeOffsets();
        sendFiles(offsets);
        finishTransfer();
    }

private:
    bool cancelled() const { return cancel_->load(); }

    void updateStatus(const std::string& status, bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastUiUpdate_ < kUiUpdateInterval) {
            return;
        }
        lastUiUpdate_ = now;
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.status = status;
        });
    }

    void updateProgress(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgressUpdate_ < kUiUpdateInterval) {
            return;
        }
        lastProgressUpdate_ = now;
        const uint64_t done = std::min<uint64_t>(
            scan_.totalBytes, resumedBytes_ + sentThisAttempt_);
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.bytesDone = done;
            job.totalBytes = scan_.totalBytes;
            job.state = TransferState::Transferring;
        });
    }

    void setCurrentFile(const std::string& path) {
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.currentFile = path;
            job.bytesDone =
                std::min<uint64_t>(scan_.totalBytes,
                                   resumedBytes_ + sentThisAttempt_);
            job.totalBytes = scan_.totalBytes;
        });
    }

    void performHandshake() {
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.status = "正在握手";
        });
        proto::Hello hello;
        hello.deviceName =
            truncateUtf8(request_.deviceName.empty() ? "LanDrop"
                                                     : request_.deviceName,
                         200);
        channel_->send(proto::MessageType::Hello, proto::makeHello(hello),
                       proto::kHandshakeTimeoutMs);
        const proto::Frame frame = readFrame(proto::kHandshakeTimeoutMs);
        if (frame.type == proto::MessageType::Error) {
            throwRemote(proto::parseError(frame.payload));
        }
        if (frame.type != proto::MessageType::HelloAck) {
            throw Error(ErrorKind::Protocol, "接收方未返回握手响应");
        }
        const proto::HelloAck ack = proto::parseHelloAck(frame.payload);
        if (ack.status != proto::HelloStatus::Ok) {
            if (ack.status == proto::HelloStatus::Busy) {
                throw Error(ErrorKind::Busy,
                            ack.message.empty() ? "接收方正忙" : ack.message);
            }
            throw Error(ErrorKind::Rejected,
                        ack.message.empty() ? "接收方拒绝了连接" : ack.message);
        }
        std::string display = request_.host;
        if (!ack.serverName.empty()) {
            display = ack.serverName + " (" + request_.host + ")";
        }
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.peer = display;
            job.status = "正在准备清单";
        });
    }

    void sendManifest() {
        proto::Start start;
        start.transferId = proto::makeTransferId();
        start.fileCount = scan_.fileCount;
        start.directoryCount = scan_.directoryCount;
        start.totalBytes = scan_.totalBytes;
        channel_->send(proto::MessageType::Start, proto::makeStart(start),
                       proto::kHandshakeTimeoutMs);
        for (std::size_t i = 0; i < scan_.entries.size(); ++i) {
            const ScannedEntry& source = scan_.entries[i];
            proto::Entry entry;
            entry.index = static_cast<uint32_t>(i);
            entry.kind = source.kind;
            entry.size = source.size;
            entry.digest = source.digest;
            entry.relativePath = source.relativePath;
            channel_->send(proto::MessageType::Entry, proto::makeEntry(entry),
                           proto::kHandshakeTimeoutMs);
            if (cancelled()) {
                throw Error(ErrorKind::Cancelled, "操作已取消");
            }
        }
        channel_->send(proto::MessageType::ManifestEnd,
                       proto::makeManifestEnd(
                           static_cast<uint32_t>(scan_.entries.size())),
                       proto::kHandshakeTimeoutMs);
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.status = "等待接收方确认";
        });
    }

    std::vector<uint64_t> receiveResumeOffsets() {
        const std::size_t itemCount = scan_.entries.size();
        std::vector<uint64_t> offsets(itemCount, 0);
        std::vector<bool> received(itemCount, false);
        for (;;) {
            const proto::Frame frame = readFrame(proto::kIoTimeoutMs);
            if (frame.type == proto::MessageType::Error) {
                throwRemote(proto::parseError(frame.payload));
            }
            if (frame.type == proto::MessageType::Ping) {
                channel_->send(proto::MessageType::Pong,
                               proto::makePing(proto::parsePingPong(frame.payload)),
                               proto::kIoTimeoutMs);
                continue;
            }
            if (frame.type == proto::MessageType::Progress) {
                const proto::Progress progress =
                    proto::parseProgress(frame.payload);
                if (!progress.message.empty()) {
                    updateStatus(progress.message, true);
                }
                continue;
            }
            if (frame.type == proto::MessageType::Resume) {
                const proto::Resume resume = proto::parseResume(frame.payload);
                if (resume.index >= itemCount || received[resume.index]) {
                    throw Error(ErrorKind::Protocol, "接收方返回了无效的续传位置");
                }
                const ScannedEntry& entry = scan_.entries[resume.index];
                if (entry.kind == proto::EntryKind::Directory) {
                    if (resume.offset != 0) {
                        throw Error(ErrorKind::Protocol,
                                    "目录续传位置非法");
                    }
                } else if (resume.offset > entry.size) {
                    throw Error(ErrorKind::Protocol,
                                "续传位置超过文件大小");
                }
                offsets[resume.index] = resume.offset;
                received[resume.index] = true;
                if (resume.offset != 0) {
                    state_.log(LogLevel::Info,
                               "续传 " + entry.relativePath + "，从 " +
                                   formatBytes(resume.offset) + " 继续");
                }
                continue;
            }
            if (frame.type == proto::MessageType::ResumeEnd) {
                const uint32_t count =
                    proto::parseResumeEnd(frame.payload);
                if (count != itemCount) {
                    throw Error(ErrorKind::Protocol, "续传确认数量不一致");
                }
                for (std::size_t i = 0; i < itemCount; ++i) {
                    if (!received[i]) {
                        throw Error(ErrorKind::Protocol,
                                    "接收方遗漏了续传确认");
                    }
                }
                break;
            }
            throw Error(ErrorKind::Protocol, "等待续传确认时收到意外消息");
        }

        resumedBytes_ = 0;
        for (std::size_t i = 0; i < itemCount; ++i) {
            if (scan_.entries[i].kind == proto::EntryKind::File) {
                resumedBytes_ += offsets[i];
            }
        }
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.bytesDone = resumedBytes_;
            job.totalBytes = scan_.totalBytes;
            job.state = TransferState::Transferring;
            job.status = "正在传输";
        });
        return offsets;
    }

    void sendFiles(const std::vector<uint64_t>& offsets) {
        std::vector<uint8_t> payload(4 + kTransferChunkSize);
        for (std::size_t i = 0; i < scan_.entries.size(); ++i) {
            if (cancelled()) {
                throw Error(ErrorKind::Cancelled, "操作已取消");
            }
            const ScannedEntry& entry = scan_.entries[i];
            if (entry.kind != proto::EntryKind::File) {
                continue;
            }
            const uint64_t offset = offsets[i];
            setCurrentFile(entry.relativePath);
            updateStatus(offset == 0 ? "正在发送" : "正在续传", true);
            channel_->send(proto::MessageType::FileBegin,
                           proto::makeFileBegin(static_cast<uint32_t>(i), offset),
                           proto::kHandshakeTimeoutMs);
            if (offset < entry.size) {
                FileReader reader;
                std::string error;
                if (!reader.open(entry.sourcePath, error)) {
                    throw Error(ErrorKind::LocalIo, error);
                }
                if (reader.size() != entry.size) {
                    throw Error(ErrorKind::LocalIo,
                                "文件在传输前发生变化: " + entry.relativePath);
                }
                if (!reader.seek(offset, error)) {
                    throw Error(ErrorKind::LocalIo, error);
                }
                uint64_t remaining = entry.size - offset;
                while (remaining > 0) {
                    if (cancelled()) {
                        throw Error(ErrorKind::Cancelled, "操作已取消");
                    }
                    const std::size_t want = static_cast<std::size_t>(
                        std::min<uint64_t>(remaining, kTransferChunkSize));
                    payload.resize(4 + want);
                    payload[0] = static_cast<uint8_t>(i >> 24);
                    payload[1] = static_cast<uint8_t>(i >> 16);
                    payload[2] = static_cast<uint8_t>(i >> 8);
                    payload[3] = static_cast<uint8_t>(i);
                    std::size_t obtained = 0;
                    if (!reader.read(payload.data() + 4, want, obtained, error)) {
                        throw Error(ErrorKind::LocalIo, error);
                    }
                    if (obtained != want) {
                        throw Error(ErrorKind::LocalIo,
                                    "读取文件时提前到达末尾: " +
                                        entry.relativePath);
                    }
                    try {
                        channel_->send(proto::MessageType::FileData, payload,
                                       proto::kIoTimeoutMs);
                    } catch (const Error& sendError) {
                        // If the receiver failed locally (disk full, rejected
                        // path), it sends an Error frame and closes while the
                        // sender may still be writing. Prefer that explicit
                        // reason over a generic connection reset.
                        std::string remoteMessage;
                        if (sendError.kind() == ErrorKind::Network &&
                            readPendingRemoteError(remoteMessage)) {
                            throw Error(ErrorKind::Rejected, remoteMessage);
                        }
                        throw;
                    }
                    remaining -= obtained;
                    sentThisAttempt_ += obtained;
                    updateProgress();
                    pollRemoteControl();
                }
            }
            channel_->send(proto::MessageType::FileEnd,
                           proto::makeFileEnd(static_cast<uint32_t>(i)),
                           proto::kHandshakeTimeoutMs);
            waitForFileResult(static_cast<uint32_t>(i));
            updateProgress(true);
        }
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.state = TransferState::Verifying;
            job.status = "等待接收方确认完成";
        });
    }

    void waitForFileResult(uint32_t expectedIndex) {
        for (;;) {
            if (cancelled()) {
                throw Error(ErrorKind::Cancelled, "操作已取消");
            }
            const proto::Frame frame = readFrame(proto::kIoTimeoutMs);
            if (frame.type == proto::MessageType::FileResult) {
                const proto::FileResult result =
                    proto::parseFileResult(frame.payload);
                if (result.index != expectedIndex) {
                    throw Error(ErrorKind::Protocol,
                                "接收方返回了错误的文件确认");
                }
                if (!result.success) {
                    const std::string message =
                        result.message.empty() ? "接收方未能写入文件"
                                               : result.message;
                    throw Error(result.code == proto::ErrorCode::NoError
                                    ? ErrorKind::Rejected
                                    : kindFromRemote(result.code),
                                message);
                }
                return;
            }
            if (frame.type == proto::MessageType::Progress) {
                const proto::Progress progress =
                    proto::parseProgress(frame.payload);
                if (!progress.message.empty()) {
                    state_.updateJob(request_.jobId, [&](JobRecord& job) {
                        job.state = TransferState::Verifying;
                        job.status = progress.message;
                    });
                }
                continue;
            }
            if (frame.type == proto::MessageType::Ping) {
                channel_->send(proto::MessageType::Pong,
                               proto::makePing(proto::parsePingPong(frame.payload)),
                               proto::kIoTimeoutMs);
                continue;
            }
            if (frame.type == proto::MessageType::Error) {
                throwRemote(proto::parseError(frame.payload));
            }
            throw Error(ErrorKind::Protocol, "等待文件确认时收到意外消息");
        }
    }

    // The receiver reports local failures (disk full, rejected path) with an
    // Error frame while the sender is still streaming, but the sender only
    // otherwise reads after FileEnd. Poll while sending so the real reason
    // surfaces instead of a generic disconnect after retries.
    void pollRemoteControl() {
        bool aborted = false;
        std::string waitError;
        if (!socket_.waitReadable(0, nullptr, aborted, waitError)) {
            return;
        }
        const proto::Frame frame = channel_->read(1000);
        if (frame.type == proto::MessageType::Error) {
            throwRemote(proto::parseError(frame.payload));
        }
        if (frame.type == proto::MessageType::Cancel) {
            throw Error(ErrorKind::Cancelled,
                        proto::parseCancel(frame.payload));
        }
        if (frame.type == proto::MessageType::Ping) {
            channel_->send(proto::MessageType::Pong,
                           proto::makePing(proto::parsePingPong(frame.payload)),
                           proto::kIoTimeoutMs);
        }
    }

    bool readPendingRemoteError(std::string& message) {
        bool aborted = false;
        std::string waitError;
        if (!socket_.waitReadable(200, nullptr, aborted, waitError)) {
            return false;
        }
        try {
            const proto::Frame frame = channel_->read(1000);
            if (frame.type == proto::MessageType::Error) {
                message = proto::parseError(frame.payload).message;
                return true;
            }
        } catch (...) {
        }
        return false;
    }

    void finishTransfer() {
        channel_->send(proto::MessageType::Done,
                       proto::makeDone(scan_.totalBytes, scan_.fileCount),
                       proto::kHandshakeTimeoutMs);
        for (;;) {
            const proto::Frame frame = readFrame(proto::kIoTimeoutMs);
            if (frame.type == proto::MessageType::DoneAck) {
                break;
            }
            if (frame.type == proto::MessageType::Progress) {
                const proto::Progress progress =
                    proto::parseProgress(frame.payload);
                if (!progress.message.empty()) {
                    updateStatus(progress.message, true);
                }
                continue;
            }
            if (frame.type == proto::MessageType::Ping) {
                channel_->send(proto::MessageType::Pong,
                               proto::makePing(proto::parsePingPong(frame.payload)),
                               proto::kIoTimeoutMs);
                continue;
            }
            if (frame.type == proto::MessageType::Error) {
                throwRemote(proto::parseError(frame.payload));
            }
            throw Error(ErrorKind::Protocol, "等待完成确认时收到意外消息");
        }
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.bytesDone = scan_.totalBytes;
            job.totalBytes = scan_.totalBytes;
            job.currentFile.clear();
            job.state = TransferState::Verifying;
            job.status = "已完成";
        });
    }

    proto::Frame readFrame(int timeoutMs) {
        if (cancelled()) {
            throw Error(ErrorKind::Cancelled, "操作已取消");
        }
        return channel_->read(timeoutMs);
    }

    [[noreturn]] void throwRemote(const proto::ErrorMessage& error) {
        throw Error(kindFromRemote(error.code), error.message);
    }

    CoreState& state_;
    const SendRequest& request_;
    const ScanResult& scan_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    Socket socket_;
    std::unique_ptr<proto::Channel> channel_;
    std::string connectError_;
    uint64_t resumedBytes_ = 0;
    uint64_t sentThisAttempt_ = 0;
    std::chrono::steady_clock::time_point lastUiUpdate_{};
    std::chrono::steady_clock::time_point lastProgressUpdate_{};
};

}  // namespace

Sender::Sender(CoreState& state, SendRequest request)
    : state_(state), request_(std::move(request)) {}

void Sender::run() noexcept {
    if (!request_.cancel) {
        request_.cancel = std::make_shared<std::atomic<bool>>(false);
    }
    auto cancelled = [&]() { return request_.cancel->load(); };
    const std::string target =
        request_.host + ":" + std::to_string(request_.port);

    auto markCanceled = [&]() {
        state_.finishJob(request_.jobId, TransferState::Canceled, "已取消");
        state_.log(LogLevel::Warning, "发送已取消（目标 " + target + "）");
    };
    auto markFailed = [&](const std::string& message) {
        state_.finishJob(request_.jobId, TransferState::Failed, "发送失败",
                         message);
        state_.log(LogLevel::Error,
                   "发送失败（目标 " + target + "）：" + message);
    };

    ScanResult scan;
    try {
        state_.updateJob(request_.jobId, [&](JobRecord& job) {
            job.state = TransferState::Preparing;
            job.status = "正在扫描文件";
            job.peer = request_.host;
        });
        scan = FileScanner::scanAndHash(
            request_.selectedPaths, *request_.cancel,
            [&](const ScanProgress& progress) {
                state_.updateJob(request_.jobId, [&](JobRecord& job) {
                    job.state = TransferState::Preparing;
                    job.currentFile = progress.currentPath;
                    if (progress.stage == ScanStage::Discovering) {
                        job.status = "正在扫描文件";
                    } else {
                        job.status = "正在校验本地文件 " +
                                     std::to_string(progress.filesDone) + "/" +
                                     std::to_string(progress.fileCount);
                        job.bytesDone = progress.bytesDone;
                        job.totalBytes = progress.totalBytes;
                    }
                });
            },
            [&](const std::string& warningText) {
                state_.log(LogLevel::Warning, warningText);
            });
    } catch (const Error& error) {
        if (cancelled() || error.kind() == ErrorKind::Cancelled) {
            markCanceled();
        } else {
            markFailed(error.what());
        }
        return;
    } catch (const std::exception& error) {
        markFailed(std::string("准备发送时发生异常: ") + error.what());
        return;
    }

    state_.updateJob(request_.jobId, [&](JobRecord& job) {
        job.itemCount = static_cast<uint32_t>(scan.entries.size());
        job.fileCount = scan.fileCount;
        job.totalBytes = scan.totalBytes;
        job.bytesDone = 0;
        job.currentFile.clear();
        job.status = "准备发送";
    });
    state_.log(LogLevel::Info,
               "准备发送 " + std::to_string(scan.fileCount) + " 个文件、" +
                   std::to_string(scan.directoryCount) + " 个文件夹（" +
                   formatBytes(scan.totalBytes) + "）到 " + target);

    int integrityRetries = 0;
    int networkRetries = 0;
    int busyRetries = 0;
    for (;;) {
        if (cancelled()) {
            markCanceled();
            return;
        }
        try {
            ClientAttempt client(state_, request_, scan);
            client.execute();
            state_.finishJob(request_.jobId, TransferState::Completed, "已完成");
            state_.log(LogLevel::LogSuccess,
                       "发送完成：" + std::to_string(scan.fileCount) +
                           " 个文件（" + formatBytes(scan.totalBytes) +
                           "）→ " + request_.host);
            return;
        } catch (const Error& error) {
            if (cancelled() || error.kind() == ErrorKind::Cancelled) {
                markCanceled();
                return;
            }

            int delaySeconds = 0;
            const bool busy = error.kind() == ErrorKind::Busy;
            if (busy) {
                ++busyRetries;
                if (busyRetries > kMaxBusyRetries) {
                    markFailed(error.what());
                    return;
                }
                delaySeconds = std::min(15, busyRetries * 3);
            } else if (error.kind() == ErrorKind::Integrity) {
                ++integrityRetries;
                if (integrityRetries > kMaxIntegrityRetries) {
                    markFailed(error.what());
                    return;
                }
                delaySeconds = 1;
            } else if (isRetriable(error.kind())) {
                ++networkRetries;
                if (networkRetries > kMaxNetworkRetries) {
                    markFailed(error.what());
                    return;
                }
                delaySeconds = std::min(5, networkRetries * 2);
            } else {
                markFailed(error.what());
                return;
            }

            state_.updateJob(request_.jobId, [&](JobRecord& job) {
                job.state = TransferState::Connecting;
                job.status =
                    (busy ? "接收方正忙，" : "连接中断，") +
                    std::to_string(delaySeconds) + " 秒后重试";
            });
            state_.log(LogLevel::Warning,
                       std::string(busy ? "接收方正忙：" : "发送中断：") +
                           error.what() + "；" +
                           std::to_string(delaySeconds) +
                           " 秒后尝试续传");
            if (!waitWithCancellation(request_.cancel,
                                      delaySeconds * 1000)) {
                markCanceled();
                return;
            }
        } catch (const std::exception& error) {
            markFailed(std::string("发送时发生异常: ") + error.what());
            return;
        }
    }
}

}  // namespace landrop
