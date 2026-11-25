#include "receiver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <locale.h>
#include <wctype.h>
#endif

#include "common.h"
#include "platform.h"
#include "protocol.h"
#include "socket.h"
#include "state.h"
#include "util.h"

namespace landrop {
namespace {

constexpr int kWorkerCount = 3;
constexpr std::size_t kMaxQueuedConnections = 16;
constexpr std::size_t kHashBufferSize = 1024 * 1024;
constexpr char kStateDirectoryName[] = ".landrop-parts";
constexpr char kMetaMagic[4] = {'L', 'D', 'P', '1'};
constexpr std::size_t kMetaSize = 48;
constexpr auto kUiUpdateInterval = std::chrono::milliseconds(120);
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

std::string sanitizePeerName(const std::string& name) {
    if (name.empty() || !isValidUtf8(name)) {
        return "未知设备";
    }
    std::string result = name;
    for (char& c : result) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 0x20 || value == 0x7f) {
            c = ' ';
        }
    }
    return truncateUtf8(trim(result), 120);
}

#ifndef _WIN32
void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        out.push_back(
            static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        out.push_back(
            static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        out.push_back(
            static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

locale_t utf8CaseLocale() {
    static locale_t locale = [] {
        locale_t candidate =
            newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        if (candidate == nullptr) {
            candidate = newlocale(LC_CTYPE_MASK, "C.utf8", nullptr);
        }
        return candidate;
    }();
    return locale;
}

// Lower-cases valid UTF-8 with the C.UTF-8 tables. This is used only to decide
// whether two manifest paths would alias on a case-insensitive destination,
// never to rename anything. Invalid input falls back to ASCII folding.
std::string foldUtf8Case(const std::string& value) {
    const locale_t locale = utf8CaseLocale();
    if (locale == nullptr) {
        return toLowerAscii(value);
    }
    std::string result;
    result.reserve(value.size());
    std::size_t i = 0;
    while (i < value.size()) {
        const auto lead = static_cast<unsigned char>(value[i]);
        uint32_t codePoint = 0;
        std::size_t length = 0;
        if (lead < 0x80) {
            codePoint = lead;
            length = 1;
        } else if ((lead & 0xe0) == 0xc0) {
            codePoint = lead & 0x1f;
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            codePoint = lead & 0x0f;
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            codePoint = lead & 0x07;
            length = 4;
        } else {
            return toLowerAscii(value);
        }
        if (i + length > value.size()) {
            return toLowerAscii(value);
        }
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) {
                return toLowerAscii(value);
            }
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint > 0x10ffff) {
            return toLowerAscii(value);
        }
        const wint_t lowered =
            towlower_l(static_cast<wint_t>(codePoint), locale);
        appendUtf8(result, static_cast<uint32_t>(lowered));
        i += length;
    }
    return result;
}
#endif

std::string foldPathCase(const std::string& value) {
#ifdef _WIN32
    return caseFoldForPathComparison(value);
#else
    return foldUtf8Case(value);
#endif
}

// The receive directory's filesystem decides whether names that differ only
// by case are distinct. A probe (below) tells us which rule applies; on a
// case-sensitive filesystem the paths stay exactly as sent, so a.txt and
// A.txt are two independent files.
std::string normalizedRelativeKey(const std::string& relativePath,
                                  bool caseInsensitive) {
    return caseInsensitive ? foldPathCase(relativePath) : relativePath;
}

// Writes a uniquely named probe file and checks whether its ASCII upper-case
// spelling resolves to the same directory entry. True means the destination
// cannot represent manifest paths that differ only by case; such a manifest
// must be rejected rather than letting one entry replace another.
bool receivePathIsCaseInsensitive(const fs::path& directory) {
#ifdef _WIN32
    (void)directory;
    return true;
#else
    const fs::path probe =
        directory / pathFromUtf8(".landrop-case-probe-" + randomHex(8));
    std::string error;
    FileWriter writer;
    if (!writer.openAt(probe, 0, error)) {
        // The receive directory is not writable; the transfer will fail
        // later anyway. Treat it as case-insensitive so we never silently
        // overwrite one manifest entry with another.
        return true;
    }
    const char marker = 'x';
    const bool written = writer.writeAll(&marker, 1, error);
    writer.close();
    bool insensitive = true;
    if (written) {
        std::string upper = pathToUtf8(probe.filename());
        for (char& c : upper) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        std::error_code ec;
        const bool upperExists =
            fs::exists(directory / pathFromUtf8(upper), ec);
        insensitive = ec ? true : upperExists;
    }
    std::error_code removeError;
    fs::remove(probe, removeError);
    return insensitive;
#endif
}

// Keys used for in-process path locks. Two connections writing case variants
// to a case-insensitive destination must share a lock; on a case-sensitive
// destination they are independent.
std::string collisionKeyForPath(const fs::path& path, bool caseInsensitive) {
    std::string key = pathToUtf8(path);
    return caseInsensitive ? foldPathCase(key) : std::move(key);
}

std::string firstComponent(const std::string& relativePath) {
    const auto slash = relativePath.find('/');
    return slash == std::string::npos ? relativePath
                                      : relativePath.substr(0, slash);
}

bool isMissingPathError(const std::error_code& error) {
    if (!error) {
        return false;
    }
    if (error == std::errc::no_such_file_or_directory) {
        return true;
    }
#ifdef _WIN32
    return error.value() == 2 || error.value() == 3;  // ERROR_FILE/PATH_NOT_FOUND
#else
    return false;
#endif
}

void validateDestinationPath(const std::string& relativePath,
                             bool caseInsensitive, std::string& message) {
    if (!validateRelativePath(relativePath, message)) {
        return;
    }
    // The reserved cache directory can only be collided with when the
    // destination filesystem ignores case. On a case-sensitive filesystem
    // ".Landrop-Parts" is a separate, safely receivable directory; on
    // Windows or a case-insensitive mount it must stay reserved.
    const std::string first = firstComponent(relativePath);
    const bool reserved =
        caseInsensitive
            ? foldPathCase(first) == foldPathCase(kStateDirectoryName)
            : first == kStateDirectoryName;
    if (reserved) {
        message = "该路径使用了 LanDrop 的保留名称";
        return;
    }
#ifdef _WIN32
    std::size_t start = 0;
    while (start <= relativePath.size()) {
        const std::size_t slash = relativePath.find('/', start);
        const std::size_t end =
            slash == std::string::npos ? relativePath.size() : slash;
        const std::string component = relativePath.substr(start, end - start);
        if (!isSafeWindowsComponent(component)) {
            message = "文件名在 Windows 上无效: " + component;
            return;
        }
        if (component.size() > 255) {
            message = "文件名过长: " + component;
            return;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
#endif
    message.clear();
}

bool isValidPartMeta(const std::array<uint8_t, kMetaSize>& data,
                     const Sha256::Digest& expectedDigest, uint64_t expectedSize,
                     uint64_t& storedSize) {
    if (std::memcmp(data.data(), kMetaMagic, 4) != 0 || data[4] != 1) {
        return false;
    }
    uint64_t size = 0;
    for (int i = 0; i < 8; ++i) {
        size = (size << 8) | data[8 + static_cast<std::size_t>(i)];
    }
    if (size != expectedSize) {
        return false;
    }
    if (std::memcmp(data.data() + 16, expectedDigest.data(),
                    expectedDigest.size()) != 0) {
        return false;
    }
    storedSize = size;
    return true;
}

bool readPartMeta(const fs::path& path, const Sha256::Digest& expectedDigest,
                  uint64_t expectedSize) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (ec && !isMissingPathError(ec)) {
            throw Error(ErrorKind::LocalIo,
                        "无法检查临时文件状态: " + pathToUtf8(path) + " — " +
                            platform::errorCodeMessage(ec));
        }
        return false;
    }
    FileReader reader;
    std::string error;
    if (!reader.open(path, error)) {
        throw Error(ErrorKind::LocalIo, error);
    }
    if (reader.size() != kMetaSize) {
        return false;
    }
    std::array<uint8_t, kMetaSize> data{};
    std::size_t obtained = 0;
    if (!reader.read(data.data(), data.size(), obtained, error) ||
        obtained != data.size()) {
        return false;
    }
    uint64_t storedSize = 0;
    return isValidPartMeta(data, expectedDigest, expectedSize, storedSize);
}

void writePartMeta(const fs::path& path, const Sha256::Digest& digest,
                   uint64_t totalSize) {
    std::array<uint8_t, kMetaSize> data{};
    std::memcpy(data.data(), kMetaMagic, 4);
    data[4] = 1;
    for (int i = 0; i < 8; ++i) {
        data[8 + i] = static_cast<uint8_t>(totalSize >>
                                           (56 - static_cast<unsigned>(i) * 8));
    }
    std::memcpy(data.data() + 16, digest.data(), digest.size());
    // This sidecar only accelerates resume. If a crash tears or loses it, the
    // size/content check in readPartMeta fails and the part is discarded and
    // re-sent; the final full-file SHA-256 still gates every rename. Writing
    // it without an fsync or atomic replace therefore cannot weaken data
    // integrity, and it removes a durable barrier that dominated the cost of
    // transfers containing many small files.
    FileWriter writer;
    std::string error;
    if (!writer.openAt(path, 0, error) ||
        !writer.writeAll(data.data(), data.size(), error)) {
        writer.close();
        throw Error(ErrorKind::LocalIo, error);
    }
}

bool queryRegularFileSize(const fs::path& path, uint64_t& size) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec && !isMissingPathError(ec)) {
        throw Error(ErrorKind::LocalIo,
                    "无法检查文件: " + pathToUtf8(path) + " — " + platform::errorCodeMessage(ec));
    }
    if (!exists) {
        return false;
    }
    if (!fs::is_regular_file(path, ec)) {
        if (ec) {
            throw Error(ErrorKind::LocalIo,
                        "无法检查文件: " + pathToUtf8(path) + " — " +
                            platform::errorCodeMessage(ec));
        }
        throw Error(ErrorKind::LocalIo,
                    "临时路径不是普通文件: " + pathToUtf8(path));
    }
    const std::uintmax_t fileSize = fs::file_size(path, ec);
    if (ec) {
        throw Error(ErrorKind::LocalIo,
                    "无法读取文件大小: " + pathToUtf8(path) + " — " +
                        platform::errorCodeMessage(ec));
    }
    size = static_cast<uint64_t>(fileSize);
    return true;
}

bool isPathWithin(const fs::path& child, const fs::path& parent) {
    auto childIt = child.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end();
         ++parentIt, ++childIt) {
        if (childIt == child.end()) {
            return false;
        }
        if (pathCollisionKey(*childIt) != pathCollisionKey(*parentIt)) {
            return false;
        }
    }
    return true;
}

bool digestsEqual(const Sha256::Digest& a, const Sha256::Digest& b) {
    unsigned int difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned int>(a[i] ^ b[i]);
    }
    return difference == 0;
}

proto::ErrorCode remoteCodeFor(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::Protocol:
            return proto::ErrorCode::Protocol;
        case ErrorKind::Rejected:
            return proto::ErrorCode::PathRejected;
        case ErrorKind::Busy:
            return proto::ErrorCode::Busy;
        case ErrorKind::LocalIo:
            return proto::ErrorCode::Disk;
        case ErrorKind::Integrity:
            return proto::ErrorCode::Integrity;
        case ErrorKind::Cancelled:
            return proto::ErrorCode::Cancelled;
        case ErrorKind::Timeout:
        case ErrorKind::Network:
            return proto::ErrorCode::Internal;
        case ErrorKind::Internal:
        case ErrorKind::NoError:
        default:
            return proto::ErrorCode::Internal;
    }
}

void removeTemporaryFile(const fs::path& path) {
    std::string error;
    if (!removeFileIfExists(path, error)) {
        throw Error(ErrorKind::LocalIo, error);
    }
}

// Decouples reading from the socket from writing and hashing. A dedicated
// reader keeps draining the TCP receive buffer while the worker thread writes
// to disk and updates SHA-256; without this, every application pause fills the
// kernel receive window and stalls the sender. The bounded queue provides
// backpressure instead of unbounded memory growth.
class FrameQueue {
public:
    FrameQueue(proto::Channel& channel, Socket& socket,
               const std::atomic<bool>& stop)
        : channel_(channel), socket_(socket), stop_(stop) {}

    ~FrameQueue() { stopAndJoin(); }

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reader_.joinable()) {
            return;
        }
        reader_ = std::thread([this]() { readerLoop(); });
    }

    proto::Frame pop(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  std::max(1, timeoutMs));
        for (;;) {
            if (!queue_.empty()) {
                proto::Frame frame = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= frame.payload.size();
                cv_.notify_all();
                return frame;
            }
            if (error_ != nullptr) {
                std::rethrow_exception(error_);
            }
            if (finished_) {
                throw Error(ErrorKind::Network, "连接已被对端关闭");
            }
            if (stopping_ || stop_.load()) {
                throw Error(ErrorKind::Cancelled, "接收已停止");
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout &&
                queue_.empty()) {
                throw Error(ErrorKind::Timeout, "接收数据超时");
            }
        }
    }

    void stopAndJoin() {
        bool needShutdown = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (joined_) {
                return;
            }
            stopping_ = true;
            needShutdown = !finished_;
            cv_.notify_all();
        }
        if (needShutdown && reader_.joinable()) {
            socket_.shutdownBoth();
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        joined_ = true;
    }

private:
    static constexpr std::size_t kMaxQueuedBytes = 8u * 1024u * 1024u;

    void readerLoop() {
        try {
            readerLoopImpl();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
            finished_ = true;
            cv_.notify_all();
        }
    }

    void readerLoopImpl() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_ || stop_.load()) {
                    finished_ = true;
                    cv_.notify_all();
                    return;
                }
            }
            proto::Frame frame;
            try {
                channel_.readInto(frame, proto::kIoTimeoutMs);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = std::current_exception();
                finished_ = true;
                cv_.notify_all();
                return;
            }
            const bool last =
                frame.type == proto::MessageType::Done ||
                frame.type == proto::MessageType::Error ||
                frame.type == proto::MessageType::Cancel;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this, &frame]() {
                    return stopping_ || stop_.load() ||
                           queuedBytes_ + frame.payload.size() <=
                               kMaxQueuedBytes;
                });
                if (stopping_ || stop_.load()) {
                    finished_ = true;
                    cv_.notify_all();
                    return;
                }
                queuedBytes_ += frame.payload.size();
                queue_.push_back(std::move(frame));
                cv_.notify_all();
            }
            if (last) {
                std::lock_guard<std::mutex> lock(mutex_);
                finished_ = true;
                cv_.notify_all();
                return;
            }
        }
    }

    proto::Channel& channel_;
    Socket& socket_;
    const std::atomic<bool>& stop_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<proto::Frame> queue_;
    std::size_t queuedBytes_ = 0;
    bool stopping_ = false;
    bool finished_ = false;
    bool joined_ = false;
    std::exception_ptr error_;
};

}  // namespace

class ReceiverServer::Impl {
public:
    Impl(CoreState& state, uint16_t requestedPort)
        : state_(state), requestedPort_(requestedPort) {}

    ~Impl() { stop(); }

    bool start(std::string& error) {
        if (started_) {
            return true;
        }
        std::string listenError;
        listener_ = Socket::createListener(requestedPort_, port_, listenError);
        if (!listener_) {
            error = listenError;
            return false;
        }
        stop_.store(false);
        try {
            workers_.reserve(kWorkerCount);
            for (int i = 0; i < kWorkerCount; ++i) {
                workers_.emplace_back([this]() { workerLoop(); });
            }
            acceptThread_ = std::thread([this]() { acceptLoop(); });
        } catch (const std::exception& exception) {
            stop_.store(true);
            queueCv_.notify_all();
            if (acceptThread_.joinable()) {
                acceptThread_.join();
            }
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                for (auto& connection : queue_) {
                    connection->close();
                }
                queue_.clear();
            }
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            workers_.clear();
            listener_.reset();
            error = std::string("无法启动接收线程: ") + exception.what();
            return false;
        }
        started_ = true;
        return true;
    }

    void stop() {
        if (!started_) {
            return;
        }
        stop_.store(true);
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        listener_.reset();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            for (auto& connection : queue_) {
                connection->close();
            }
            queue_.clear();
        }
        queueCv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        started_ = false;
    }

    bool running() const { return started_; }
    uint16_t port() const { return port_; }

    void releasePathLocks(const std::vector<std::string>& keys) {
        std::lock_guard<std::mutex> lock(pathLockMutex_);
        for (const auto& key : keys) {
            activePaths_.erase(key);
        }
    }

private:
    class PathLockGuard {
    public:
        PathLockGuard(Impl& owner, std::vector<std::string> keys)
            : owner_(owner), keys_(std::move(keys)) {}
        ~PathLockGuard() {
            if (held_) {
                owner_.releasePathLocks(keys_);
            }
        }
        PathLockGuard(const PathLockGuard&) = delete;
        PathLockGuard& operator=(const PathLockGuard&) = delete;

        bool acquire() {
            if (keys_.empty()) {
                return true;
            }
            if (!owner_.tryAcquirePathLocks(keys_)) {
                return false;
            }
            held_ = true;
            return true;
        }

    private:
        Impl& owner_;
        std::vector<std::string> keys_;
        bool held_ = false;
    };

    bool tryAcquirePathLocks(const std::vector<std::string>& keys) {
        std::lock_guard<std::mutex> lock(pathLockMutex_);
        for (const auto& key : keys) {
            if (activePaths_.count(key) != 0) {
                return false;
            }
        }
        try {
            for (const auto& key : keys) {
                activePaths_.insert(key);
            }
        } catch (...) {
            // No other thread can insert while pathLockMutex_ is held, and
            // every key was absent above, so erasing all of them here can
            // only remove locks partially acquired by this call.
            for (const auto& key : keys) {
                activePaths_.erase(key);
            }
            throw;
        }
        return true;
    }

    void acceptLoop() {
        while (!stop_.load()) {
            bool timedOut = false;
            bool aborted = false;
            std::string error;
            auto connection = listener_->acceptConnection(
                300, &stop_, timedOut, aborted, error);
            if (connection) {
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    if (queue_.size() >= kMaxQueuedConnections) {
                        connection.reset();
                    } else {
                        queue_.push_back(std::move(connection));
                        queueCv_.notify_one();
                    }
                }
                continue;
            }
            if (aborted || stop_.load()) {
                break;
            }
            if (!timedOut && !error.empty() && !stop_.load()) {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(logMutex_);
                if (now - lastAcceptErrorLog_ > std::chrono::seconds(5)) {
                    lastAcceptErrorLog_ = now;
                    state_.log(LogLevel::Warning,
                               "接收监听发生错误: " + error);
                }
                platform::sleepMilliseconds(50);
            }
        }
    }

    void workerLoop() {
        for (;;) {
            std::unique_ptr<Socket> connection;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait(lock, [this]() {
                    return stop_.load() || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stop_.load()) {
                        break;
                    }
                    continue;
                }
                connection = std::move(queue_.front());
                queue_.pop_front();
            }
            if (stop_.load()) {
                break;
            }
            try {
                handleConnection(std::move(connection));
            } catch (const std::exception& error) {
                if (!stop_.load()) {
                    state_.log(LogLevel::Error,
                               std::string("接收线程异常: ") + error.what());
                }
            }
        }
    }

    void handleConnection(std::unique_ptr<Socket> socket) {
        if (!socket) {
            return;
        }
        proto::Channel channel(*socket, &stop_);
        uint64_t jobId = 0;
        const std::string peerIp = socket->remoteAddress();
        std::string peerName = "未知设备";
        std::string peerDisplay =
            peerIp.empty() ? peerName : peerName + " (" + peerIp + ")";
        std::unique_ptr<FrameQueue> frames;

        try {
            const proto::Frame helloFrame =
                channel.read(proto::kHandshakeTimeoutMs);
            if (helloFrame.type != proto::MessageType::Hello) {
                throw Error(ErrorKind::Protocol, "缺少 HELLO 握手");
            }
            const proto::Hello hello = proto::parseHello(helloFrame.payload);
            peerName = sanitizePeerName(hello.deviceName);
            peerDisplay =
                peerName + (peerIp.empty() ? "" : " (" + peerIp + ")");
            proto::HelloAck ack;
            ack.serverName = state_.deviceName();
            if (hello.protocolVersion != kProtocolVersion) {
                ack.status = proto::HelloStatus::Version;
                ack.message = "协议版本不兼容，请使用相同版本的 LanDrop";
                channel.send(proto::MessageType::HelloAck,
                             proto::makeHelloAck(ack),
                             proto::kHandshakeTimeoutMs);
                return;
            }
            if (!state_.allowIncoming()) {
                ack.status = proto::HelloStatus::Rejected;
                ack.message = "接收方已关闭文件接收";
                channel.send(proto::MessageType::HelloAck,
                             proto::makeHelloAck(ack),
                             proto::kHandshakeTimeoutMs);
                state_.log(LogLevel::Warning,
                           "已拒绝 " + peerDisplay + "：本机接收功能已关闭");
                return;
            }
            ack.status = proto::HelloStatus::Ok;
            channel.send(proto::MessageType::HelloAck,
                         proto::makeHelloAck(ack),
                         proto::kHandshakeTimeoutMs);

            const proto::Frame startFrame =
                channel.read(proto::kHandshakeTimeoutMs);
            if (startFrame.type == proto::MessageType::Error) {
                const auto remote = proto::parseError(startFrame.payload);
                throw Error(ErrorKind::Rejected, remote.message);
            }
            if (startFrame.type != proto::MessageType::Start) {
                throw Error(ErrorKind::Protocol, "缺少传输清单起始消息");
            }
            const proto::Start start = proto::parseStart(startFrame.payload);
            const uint64_t itemCount64 =
                static_cast<uint64_t>(start.fileCount) + start.directoryCount;
            if (itemCount64 == 0 ||
                itemCount64 > static_cast<uint64_t>(kMaxManifestEntries)) {
                throw Error(ErrorKind::Protocol, "传输清单数量无效");
            }
            if (!state_.allowIncoming()) {
                sendError(channel, proto::ErrorCode::IncomingDisabled,
                          "接收方已关闭文件接收");
                return;
            }
            const uint32_t itemCount = static_cast<uint32_t>(itemCount64);
            jobId = state_.addJob(false, peerDisplay, itemCount,
                                  start.fileCount, start.totalBytes);
            state_.updateJob(jobId, [](JobRecord& job) {
                job.state = TransferState::Preparing;
                job.status = "正在接收清单";
            });
            state_.log(LogLevel::Info,
                       "收到来自 " + peerDisplay + " 的传输请求（" +
                           std::to_string(start.fileCount) + " 个文件，" +
                           formatBytes(start.totalBytes) + "）");

            fs::path root = state_.receiveDirectory();
            if (root.empty()) {
                throw Error(ErrorKind::LocalIo, "未设置接收目录");
            }
            std::string ioError;
            if (!ensureDirectories(root, ioError)) {
                throw Error(ErrorKind::LocalIo, ioError);
            }
            std::error_code ec;
            const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
            if (ec) {
                throw Error(ErrorKind::LocalIo,
                            "无法解析接收目录: " + pathToUtf8(root) + " — " +
                                platform::errorCodeMessage(ec));
            }
            // The destination's own filesystem decides whether case variants
            // are distinct. Probe once per transfer; the probe file is
            // removed immediately.
            const bool caseInsensitive = receivePathIsCaseInsensitive(root);

            std::vector<proto::Entry> entries;
            entries.reserve(itemCount);
            std::vector<fs::path> finalPaths;
            finalPaths.reserve(itemCount);
            uint64_t sumBytes = 0;
            std::size_t totalPathBytes = 0;
            std::size_t totalPathComponents = 0;
            const auto manifestDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (uint32_t index = 0; index < itemCount; ++index) {
                if (stop_.load()) {
                    throw Error(ErrorKind::Cancelled, "接收已停止");
                }
                const int remainingMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        manifestDeadline - std::chrono::steady_clock::now())
                        .count());
                if (remainingMs <= 0) {
                    throw Error(ErrorKind::Timeout,
                                "接收传输清单超时");
                }
                const proto::Frame frame = channel.read(std::min(
                    proto::kIoTimeoutMs, std::max(1, remainingMs)));
                if (frame.type == proto::MessageType::Error) {
                    const auto remote = proto::parseError(frame.payload);
                    throw Error(ErrorKind::Rejected, remote.message);
                }
                if (frame.type != proto::MessageType::Entry) {
                    throw Error(ErrorKind::Protocol, "传输清单不完整");
                }
                proto::Entry entry = proto::parseEntry(frame.payload);
                if (entry.index != index) {
                    throw Error(ErrorKind::Protocol, "传输清单索引乱序");
                }
                if (entry.kind != proto::EntryKind::File &&
                    entry.kind != proto::EntryKind::Directory) {
                    throw Error(ErrorKind::Protocol, "未知的清单项目类型");
                }
                std::string pathError;
                validateDestinationPath(entry.relativePath, caseInsensitive,
                                        pathError);
                if (!pathError.empty()) {
                    throw Error(ErrorKind::Rejected,
                                "接收方拒绝路径 " + entry.relativePath + "：" +
                                    pathError);
                }
                const std::size_t pathComponents =
                    1 + static_cast<std::size_t>(
                            std::count(entry.relativePath.begin(),
                                       entry.relativePath.end(), '/'));
                if (pathComponents > kMaxManifestPathDepth ||
                    entry.relativePath.size() >
                        kMaxManifestPathBytes - totalPathBytes ||
                    pathComponents >
                        kMaxManifestPathComponents - totalPathComponents) {
                    throw Error(ErrorKind::Protocol,
                                "传输清单的路径总量超过安全上限");
                }
                totalPathBytes += entry.relativePath.size();
                totalPathComponents += pathComponents;
                if (entry.kind == proto::EntryKind::File) {
                    if (entry.size > UINT64_MAX - sumBytes) {
                        throw Error(ErrorKind::Protocol, "传输总大小溢出");
                    }
                    sumBytes += entry.size;
                }
                const fs::path finalPath =
                    appendRelativePath(root, entry.relativePath);
#ifdef _WIN32
                if (finalPath.native().size() > 248) {
                    throw Error(ErrorKind::Rejected,
                                "目标路径过长（Windows 限制）：" +
                                    entry.relativePath);
                }
#endif
                entries.push_back(std::move(entry));
                finalPaths.push_back(finalPath);
            }
            const proto::Frame manifestEnd =
                channel.read(proto::kHandshakeTimeoutMs);
            if (manifestEnd.type != proto::MessageType::ManifestEnd) {
                throw Error(ErrorKind::Protocol, "缺少清单结束消息");
            }
            if (proto::parseManifestEnd(manifestEnd.payload) != itemCount) {
                throw Error(ErrorKind::Protocol, "清单项目数量不一致");
            }
            if (sumBytes != start.totalBytes) {
                throw Error(ErrorKind::Protocol, "清单总大小不一致");
            }

            struct KeyedEntry {
                std::string key;
                bool file = false;
                std::string originalPath;
            };
            std::vector<KeyedEntry> pathKeys;
            pathKeys.reserve(entries.size());
            for (const auto& entry : entries) {
                pathKeys.push_back(
                    {normalizedRelativeKey(entry.relativePath,
                                           caseInsensitive),
                     entry.kind == proto::EntryKind::File,
                     entry.relativePath});
            }
            std::sort(pathKeys.begin(), pathKeys.end(),
                      [](const KeyedEntry& a, const KeyedEntry& b) {
                          return a.key < b.key;
                      });
            for (std::size_t i = 1; i < pathKeys.size(); ++i) {
                if (pathKeys[i].key == pathKeys[i - 1].key) {
                    if (caseInsensitive &&
                        pathKeys[i].originalPath !=
                            pathKeys[i - 1].originalPath) {
                        throw Error(
                            ErrorKind::Rejected,
                            "接收目录所在文件系统不区分大小写，以下路径无法同时保存: " +
                                pathKeys[i - 1].originalPath + " 与 " +
                                pathKeys[i].originalPath +
                                "；请重命名其中一个后重试");
                    }
                    throw Error(ErrorKind::Rejected,
                                "传输清单包含重复路径: " +
                                    pathKeys[i].originalPath);
                }
            }
            // Reject a file that is also an ancestor directory of another
            // item. A sorted lower_bound check avoids materializing every
            // path prefix, which a hostile manifest could otherwise amplify
            // into gigabytes of allocations.
            for (const auto& item : pathKeys) {
                if (!item.file) {
                    continue;
                }
                const std::string descendantPrefix = item.key + "/";
                const auto next = std::lower_bound(
                    pathKeys.begin(), pathKeys.end(), descendantPrefix,
                    [](const KeyedEntry& entry, const std::string& prefix) {
                        return entry.key < prefix;
                    });
                if (next != pathKeys.end() &&
                    next->key.compare(0, descendantPrefix.size(),
                                      descendantPrefix) == 0) {
                    throw Error(ErrorKind::Rejected,
                                "清单中存在文件/目录路径冲突: " +
                                    item.originalPath);
                }
            }
            uint32_t fileCount = 0;
            uint32_t directoryCount = 0;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& entry = entries[i];
                if (entry.kind == proto::EntryKind::File) {
                    ++fileCount;
                } else {
                    ++directoryCount;
                }
                fs::file_status status = fs::symlink_status(finalPaths[i], ec);
                if (ec && isMissingPathError(ec)) {
                    ec.clear();
                    status = fs::file_status(fs::file_type::not_found);
                }
                if (ec) {
                    throw Error(ErrorKind::LocalIo,
                                "无法检查目标路径: " +
                                    pathToUtf8(finalPaths[i]) + " — " +
                                    platform::errorCodeMessage(ec));
                }
                const bool reparse =
                    platform::isReparsePoint(finalPaths[i]);
                if (fs::exists(status) || reparse) {
                    if (fs::is_symlink(status) || reparse) {
                        throw Error(
                            ErrorKind::Rejected,
                            "目标位置存在符号链接/重解析点，已拒绝写入: " +
                                entry.relativePath);
                    }
                    if (entry.kind == proto::EntryKind::File &&
                        fs::is_directory(status)) {
                        throw Error(ErrorKind::Rejected,
                                    "目标位置是文件夹，无法写入同名文件: " +
                                        entry.relativePath);
                    }
                    if (entry.kind == proto::EntryKind::Directory &&
                        !fs::is_directory(status)) {
                        throw Error(ErrorKind::Rejected,
                                    "目标位置是文件，无法创建同名文件夹: " +
                                        entry.relativePath);
                    }
                }
            }
            if (fileCount != start.fileCount ||
                directoryCount != start.directoryCount) {
                throw Error(ErrorKind::Protocol, "清单统计信息不一致");
            }

            std::vector<std::string> lockKeys;
            lockKeys.reserve(fileCount);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].kind == proto::EntryKind::File) {
                    lockKeys.push_back(
                        collisionKeyForPath(finalPaths[i], caseInsensitive));
                }
            }
            PathLockGuard pathGuard(*this, lockKeys);
            if (!pathGuard.acquire()) {
                throw Error(ErrorKind::Busy,
                            "另一个接收任务正在写入相同文件，请稍后重试");
            }

            state_.updateJob(jobId, [](JobRecord& job) {
                job.status = "正在准备接收目录";
            });

            std::set<std::string> directoryPaths;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].kind == proto::EntryKind::Directory) {
                    directoryPaths.insert(pathToUtf8(finalPaths[i]));
                }
            }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].kind == proto::EntryKind::File &&
                    !finalPaths[i].parent_path().empty()) {
                    directoryPaths.insert(
                        pathToUtf8(finalPaths[i].parent_path()));
                }
            }
            auto lastHeartbeat = std::chrono::steady_clock::now();
            auto maybeHeartbeat = [&](const std::string& message) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastHeartbeat < kHeartbeatInterval) {
                    return;
                }
                lastHeartbeat = now;
                channel.send(proto::MessageType::Progress,
                             proto::makeProgress(proto::kProgressGeneral, 0,
                                                 message),
                             proto::kHandshakeTimeoutMs);
            };
            // Windows junctions are reparse points, but symlink_status and
            // weakly_canonical may still report them as ordinary directories.
            // Walk every existing destination ancestor explicitly so a
            // planted junction cannot redirect writes outside the inbox.
            {
                std::set<std::string> checkedReparseDirs;
                const std::string rootKey = pathCollisionKey(root);
                for (const auto& directory : directoryPaths) {
                    std::vector<fs::path> pending;
                    fs::path current = pathFromUtf8(directory);
                    while (!current.empty()) {
                        const std::string key = pathCollisionKey(current);
                        if (key == rootKey ||
                            checkedReparseDirs.count(key) != 0) {
                            break;
                        }
                        pending.push_back(current);
                        const fs::path parent = current.parent_path();
                        if (parent == current) {
                            break;
                        }
                        current = parent;
                    }
                    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                        if (platform::isReparsePoint(*it)) {
                            throw Error(
                                ErrorKind::Rejected,
                                "目标目录包含符号链接/重解析点，已拒绝写入: " +
                                    pathToUtf8(*it));
                        }
                        checkedReparseDirs.insert(pathCollisionKey(*it));
                    }
                    maybeHeartbeat("正在检查目标目录");
                }
            }
            // Resolve every destination directory before creating anything.
            // This catches an existing symlinked subdirectory that would
            // otherwise redirect writes outside the selected receive folder.
            for (const auto& directory : directoryPaths) {
                if (stop_.load()) {
                    throw Error(ErrorKind::Cancelled, "接收已停止");
                }
                const fs::path directoryPath = pathFromUtf8(directory);
                const fs::path canonicalDirectory =
                    fs::weakly_canonical(directoryPath, ec);
                if (ec || !isPathWithin(canonicalDirectory, canonicalRoot)) {
                    throw Error(ErrorKind::Rejected,
                                "目标路径超出了接收目录（符号链接或路径异常）: " +
                                    directory);
                }
                maybeHeartbeat("正在检查目标目录");
            }
            for (const auto& directory : directoryPaths) {
                if (stop_.load()) {
                    throw Error(ErrorKind::Cancelled, "接收已停止");
                }
                if (!ensureDirectories(pathFromUtf8(directory), ioError)) {
                    throw Error(ErrorKind::LocalIo, ioError);
                }
                maybeHeartbeat("正在创建接收目录");
            }

            const fs::path stateDirectory =
                root / pathFromUtf8(kStateDirectoryName);
            fs::path partDirectory;
            if (fileCount != 0) {
                const fs::path canonicalStateDirectory =
                    fs::weakly_canonical(stateDirectory, ec);
                if (ec || !isPathWithin(canonicalStateDirectory,
                                        canonicalRoot) ||
                    platform::isReparsePoint(stateDirectory)) {
                    throw Error(ErrorKind::Rejected,
                                "接收缓存目录不安全: " +
                                    pathToUtf8(stateDirectory));
                }
#ifdef _WIN32
                // Part names are 64 hex characters plus ".part.meta". Keep
                // them under MAX_PATH as well; a short final name does not
                // imply a short cache path.
                if (stateDirectory.native().size() + 1 + 64 + 10 > 248) {
                    throw Error(
                        ErrorKind::LocalIo,
                        "接收目录路径过长，无法创建续传缓存，"
                        "请选择更短的接收目录");
                }
#endif
                if (!ensureDirectories(stateDirectory, ioError)) {
                    throw Error(ErrorKind::LocalIo, ioError);
                }
                partDirectory = stateDirectory;
            }

            std::vector<std::string> partPaths(itemCount);
            std::vector<std::string> metaPaths(itemCount);
            std::vector<uint64_t> resumeOffsets(itemCount, 0);
            std::vector<bool> fileComplete(itemCount, false);
            uint64_t baseDone = 0;
            state_.updateJob(jobId, [](JobRecord& job) {
                job.status = "正在检查续传数据";
            });

            auto verifyExistingFinal =
                [&](const fs::path& path, const proto::Entry& entry) -> bool {
                if (platform::isReparsePoint(path)) {
                    return false;
                }
                uint64_t existingSize = 0;
                if (!queryRegularFileSize(path, existingSize) ||
                    existingSize != entry.size) {
                    return false;
                }
                FileReader reader;
                std::string readError;
                if (!reader.open(path, readError) ||
                    reader.size() != entry.size) {
                    return false;
                }
                state_.updateJob(jobId, [&](JobRecord& job) {
                    job.status = "正在校验已有文件";
                    job.currentFile = entry.relativePath;
                });
                Sha256 hasher;
                std::vector<uint8_t> buffer(kHashBufferSize);
                uint64_t checked = 0;
                auto lastUpdate = std::chrono::steady_clock::now();
                for (;;) {
                    if (stop_.load()) {
                        throw Error(ErrorKind::Cancelled, "接收已停止");
                    }
                    std::size_t obtained = 0;
                    if (!reader.read(buffer.data(), buffer.size(), obtained,
                                     readError)) {
                        return false;
                    }
                    if (obtained == 0) {
                        break;
                    }
                    hasher.update(buffer.data(), obtained);
                    checked += obtained;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastUpdate >= kHeartbeatInterval) {
                        lastUpdate = now;
                        maybeHeartbeat("正在校验接收目录中已存在的同名文件（" +
                                       formatBytes(checked) + " / " +
                                       formatBytes(entry.size) + "）");
                    }
                }
                const Sha256::Digest digest = hasher.finish();
                return digestsEqual(digest, entry.digest);
            };

            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (stop_.load()) {
                    throw Error(ErrorKind::Cancelled, "接收已停止");
                }
                const auto& entry = entries[i];
                if (entry.kind != proto::EntryKind::File) {
                    continue;
                }
                const std::string key =
                    collisionKeyForPath(finalPaths[i], caseInsensitive);
                const Sha256::Digest pathDigest =
                    Sha256::hash(key.data(), key.size());
                const std::string partName =
                    Sha256::toHex(pathDigest) + ".part";
                const fs::path partPath = partDirectory / pathFromUtf8(partName);
                const fs::path metaPath =
                    partDirectory / pathFromUtf8(partName + ".meta");
                partPaths[i] = pathToUtf8(partPath);
                metaPaths[i] = pathToUtf8(metaPath);

                std::error_code linkError;
                const bool partLink =
                    fs::is_symlink(partPath, linkError) ||
                    platform::isReparsePoint(partPath);
                linkError.clear();
                const bool metaLink =
                    fs::is_symlink(metaPath, linkError) ||
                    platform::isReparsePoint(metaPath);
                if (partLink || metaLink) {
                    throw Error(
                        ErrorKind::Rejected,
                        "接收缓存目录中存在符号链接/重解析点，已拒绝写入");
                }

                // A completed destination can be reused after a reconnect or
                // when the same selection is sent again. Verify it before
                // skipping network data; the full digest makes this safe.
                if (verifyExistingFinal(finalPaths[i], entry)) {
                    fileComplete[i] = true;
                    resumeOffsets[i] = entry.size;
                    baseDone += entry.size;
                    std::string cleanupError;
                    if (!removeFileIfExists(partPath, cleanupError) ||
                        !removeFileIfExists(metaPath, cleanupError)) {
                        state_.log(LogLevel::Warning, cleanupError);
                    }
                    state_.log(LogLevel::Info,
                               "目标文件已存在且 SHA-256 一致，跳过重传: " +
                                   entry.relativePath);
                    maybeHeartbeat("正在检查续传数据");
                    continue;
                }

                uint64_t partSize = 0;
                const bool partExists = queryRegularFileSize(partPath, partSize);
                const bool metaOk = readPartMeta(metaPath, entry.digest,
                                                 entry.size);
                uint64_t offset = 0;
                bool reset = false;
                if (metaOk && partExists && partSize <= entry.size) {
                    offset = partSize;
                } else {
                    if (partExists || metaOk) {
                        reset = true;
                    }
                }
                if (reset) {
                    removeTemporaryFile(partPath);
                    removeTemporaryFile(metaPath);
                    offset = 0;
                    state_.log(LogLevel::Warning,
                               "已丢弃不匹配的未完成数据: " +
                                   entry.relativePath);
                }
                resumeOffsets[i] = offset;
                baseDone += offset;
                if (offset > 0) {
                    state_.log(LogLevel::Info,
                               "从 " + formatBytes(offset) + " 继续接收 " +
                                   entry.relativePath);
                }
                maybeHeartbeat("正在检查续传数据");
            }

            for (std::size_t i = 0; i < entries.size(); ++i) {
                proto::Resume resume;
                resume.index = static_cast<uint32_t>(i);
                resume.offset = resumeOffsets[i];
                channel.send(proto::MessageType::Resume,
                             proto::makeResume(resume),
                             proto::kHandshakeTimeoutMs);
            }
            channel.send(proto::MessageType::ResumeEnd,
                         proto::makeResumeEnd(itemCount),
                         proto::kHandshakeTimeoutMs);
            state_.updateJob(jobId, [&](JobRecord& job) {
                job.state = TransferState::Transferring;
                job.status = "正在接收";
                job.bytesDone = baseDone;
                job.totalBytes = start.totalBytes;
            });
            state_.log(LogLevel::Info,
                       "开始接收来自 " + peerDisplay + " 的 " +
                           std::to_string(fileCount) + " 个文件");

            frames = std::make_unique<FrameQueue>(channel, *socket, stop_);
            frames->start();

            uint64_t receivedThisSession = 0;
            auto lastUiUpdate = std::chrono::steady_clock::time_point{};
            auto updateProgress = [&](const fs::path& current, bool force) {
                const auto now = std::chrono::steady_clock::now();
                if (!force && now - lastUiUpdate < kUiUpdateInterval) {
                    return;
                }
                lastUiUpdate = now;
                const uint64_t done = std::min<uint64_t>(
                    start.totalBytes, baseDone + receivedThisSession);
                state_.updateJob(jobId, [&](JobRecord& job) {
                    job.currentFile = pathToUtf8(current);
                    job.bytesDone = done;
                    job.totalBytes = start.totalBytes;
                    job.state = TransferState::Transferring;
                    job.status = "正在接收";
                });
            };

            // Frames arrive from the reader thread; each payload stays alive
            // until its write and hash for the frame are complete.
            proto::Frame frame;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].kind != proto::EntryKind::File) {
                    continue;
                }
                const auto fileIndex = static_cast<uint32_t>(i);
                const auto& entry = entries[i];
                const uint64_t offset = resumeOffsets[i];
                const bool alreadyComplete = fileComplete[i];
                const fs::path partPath = pathFromUtf8(partPaths[i]);
                const fs::path metaPath = pathFromUtf8(metaPaths[i]);

                frame = frames->pop(proto::kIoTimeoutMs);
                while (frame.type == proto::MessageType::Ping) {
                    channel.send(proto::MessageType::Pong,
                                 proto::makePing(proto::parsePingPong(
                                     frame.payload)),
                                 proto::kIoTimeoutMs);
                    frame = frames->pop(proto::kIoTimeoutMs);
                }
                if (frame.type == proto::MessageType::Error) {
                    const auto remote = proto::parseError(frame.payload);
                    throw Error(ErrorKind::Rejected, remote.message);
                }
                if (frame.type != proto::MessageType::FileBegin) {
                    throw Error(ErrorKind::Protocol,
                                "开始接收文件时收到意外消息");
                }
                uint32_t beginIndex = 0;
                uint64_t beginOffset = 0;
                proto::parseFileBegin(frame.payload, beginIndex,
                                      beginOffset);
                if (beginIndex != fileIndex || beginOffset != offset) {
                    throw Error(ErrorKind::Protocol, "文件续传位置不一致");
                }

                updateProgress(finalPaths[i], true);
                state_.updateJob(jobId, [&](JobRecord& job) {
                    job.currentFile = pathToUtf8(finalPaths[i]);
                    job.status = alreadyComplete
                                     ? "已校验，等待确认"
                                     : (offset > 0 ? "正在续传" : "正在接收");
                    job.state = alreadyComplete ? TransferState::Verifying
                                                : TransferState::Transferring;
                });

                FileWriter writer;
                Sha256 hasher;
                bool writerOpen = false;
                if (!alreadyComplete) {
                    // A zero-byte file has offset == size == 0, but it still
                    // needs a fresh (empty) part file and metadata entry
                    // before FileEnd.
                    if (offset < entry.size || entry.size == 0) {
                        std::string writeError;
                        if (!writer.openAt(partPath, offset, writeError)) {
                            throw Error(ErrorKind::LocalIo, writeError);
                        }
                        writerOpen = true;
                        writePartMeta(metaPath, entry.digest, entry.size);
                    } else {
                        uint64_t existing = 0;
                        if (!queryRegularFileSize(partPath, existing) ||
                            existing != entry.size) {
                            throw Error(ErrorKind::LocalIo,
                                        "续传临时文件已被删除或损坏");
                        }
                    }
                }

                uint64_t remaining = entry.size - offset;
                for (;;) {
                    if (stop_.load()) {
                        throw Error(ErrorKind::Cancelled, "接收已停止");
                    }
                    frame = frames->pop(proto::kIoTimeoutMs);
                    if (frame.type == proto::MessageType::FileData) {
                        uint32_t dataIndex = 0;
                        const uint8_t* data = nullptr;
                        std::size_t dataSize = 0;
                        proto::parseFileData(frame.payload, dataIndex, data,
                                             dataSize);
                        if (dataIndex != fileIndex) {
                            throw Error(ErrorKind::Protocol,
                                        "数据块文件索引不一致");
                        }
                        if (dataSize == 0) {
                            throw Error(ErrorKind::Protocol,
                                        "收到零长度文件数据帧");
                        }
                        if (dataSize > remaining) {
                            throw Error(ErrorKind::Protocol,
                                        "数据块超过声明的文件大小");
                        }
                        if (!writerOpen) {
                            throw Error(ErrorKind::Protocol,
                                        "文件数据长度与续传位置不一致");
                        }
                        std::string writeError;
                        if (!writer.writeAll(data, dataSize, writeError)) {
                            throw Error(ErrorKind::LocalIo, writeError);
                        }
                        if (offset == 0) {
                            hasher.update(data, dataSize);
                        }
                        remaining -= dataSize;
                        receivedThisSession += dataSize;
                        updateProgress(finalPaths[i], false);
                        continue;
                    }
                    if (frame.type == proto::MessageType::FileEnd) {
                        if (proto::parseFileEnd(frame.payload) != fileIndex) {
                            throw Error(ErrorKind::Protocol,
                                        "文件结束索引不一致");
                        }
                        if (remaining != 0) {
                            throw Error(ErrorKind::Protocol,
                                        "文件数据不完整");
                        }
                        break;
                    }
                    if (frame.type == proto::MessageType::Ping) {
                        channel.send(proto::MessageType::Pong,
                                     proto::makePing(proto::parsePingPong(
                                         frame.payload)),
                                     proto::kIoTimeoutMs);
                        continue;
                    }
                    if (frame.type == proto::MessageType::Error) {
                        const auto remote =
                            proto::parseError(frame.payload);
                        throw Error(ErrorKind::Rejected, remote.message);
                    }
                    if (frame.type == proto::MessageType::Cancel) {
                        throw Error(ErrorKind::Cancelled,
                                    proto::parseCancel(frame.payload));
                    }
                    throw Error(ErrorKind::Protocol,
                                "接收文件时收到意外消息");
                }

                if (alreadyComplete) {
                    proto::FileResult result;
                    result.index = fileIndex;
                    result.success = true;
                    result.code = proto::ErrorCode::NoError;
                    channel.send(proto::MessageType::FileResult,
                                 proto::makeFileResult(result),
                                 proto::kHandshakeTimeoutMs);
                    updateProgress(finalPaths[i], true);
                    continue;
                }

                if (writerOpen) {
                    std::string syncError;
                    if (!writer.sync(syncError)) {
                        writer.close();
                        throw Error(ErrorKind::LocalIo, syncError);
                    }
                    writer.close();
                }

                Sha256::Digest actualDigest{};
                if (offset == 0) {
                    actualDigest = hasher.finish();
                } else {
                    state_.updateJob(jobId, [](JobRecord& job) {
                        job.state = TransferState::Verifying;
                        job.status = "正在校验已续传数据";
                    });
                    FileReader reader;
                    std::string readError;
                    if (!reader.open(partPath, readError)) {
                        throw Error(ErrorKind::LocalIo, readError);
                    }
                    if (reader.size() != entry.size) {
                        throw Error(ErrorKind::LocalIo,
                                    "续传临时文件大小异常");
                    }
                    Sha256 verifyHasher;
                    std::vector<uint8_t> buffer(kHashBufferSize);
                    uint64_t verified = 0;
                    auto lastVerifyHeartbeat =
                        std::chrono::steady_clock::now();
                    for (;;) {
                        if (stop_.load()) {
                            throw Error(ErrorKind::Cancelled, "接收已停止");
                        }
                        std::size_t obtained = 0;
                        if (!reader.read(buffer.data(), buffer.size(),
                                         obtained, readError)) {
                            throw Error(ErrorKind::LocalIo, readError);
                        }
                        if (obtained == 0) {
                            break;
                        }
                        verifyHasher.update(buffer.data(), obtained);
                        verified += obtained;
                        const auto now = std::chrono::steady_clock::now();
                        if (now - lastVerifyHeartbeat >= kHeartbeatInterval) {
                            lastVerifyHeartbeat = now;
                            channel.send(
                                proto::MessageType::Progress,
                                proto::makeProgress(
                                    fileIndex, verified,
                                    "正在校验已续传数据 " +
                                        std::to_string(
                                            entry.size == 0
                                                ? 0
                                                : static_cast<uint64_t>(
                                                      (static_cast<long double>(
                                                           verified) /
                                                       entry.size) *
                                                      100)) +
                                        "%"),
                                proto::kHandshakeTimeoutMs);
                        }
                    }
                    actualDigest = verifyHasher.finish();
                }

                if (!digestsEqual(actualDigest, entry.digest)) {
                    std::string cleanupError;
                    if (!removeFileIfExists(partPath, cleanupError)) {
                        state_.log(LogLevel::Warning, cleanupError);
                    }
                    if (!removeFileIfExists(metaPath, cleanupError)) {
                        state_.log(LogLevel::Warning, cleanupError);
                    }
                    const std::string message =
                        "SHA-256 校验失败，已丢弃损坏的临时文件: " +
                        entry.relativePath;
                    proto::FileResult result;
                    result.index = fileIndex;
                    result.success = false;
                    result.code = proto::ErrorCode::Integrity;
                    result.message = truncateUtf8(message, 1800);
                    channel.send(proto::MessageType::FileResult,
                                 proto::makeFileResult(result),
                                 proto::kHandshakeTimeoutMs);
                    state_.finishJob(jobId, TransferState::Failed,
                                     "校验失败", message);
                    state_.log(LogLevel::Error, message);
                    return;
                }

                std::string renameError;
                if (!atomicReplaceFile(partPath, finalPaths[i], renameError)) {
                    const std::string message =
                        "无法保存文件 " + entry.relativePath + "：" +
                        renameError;
                    proto::FileResult result;
                    result.index = fileIndex;
                    result.success = false;
                    result.code = proto::ErrorCode::Disk;
                    result.message = truncateUtf8(message, 1800);
                    channel.send(proto::MessageType::FileResult,
                                 proto::makeFileResult(result),
                                 proto::kHandshakeTimeoutMs);
                    state_.finishJob(jobId, TransferState::Failed,
                                     "保存失败", message);
                    state_.log(LogLevel::Error, message);
                    return;
                }
                std::string removeMetaError;
                if (!removeFileIfExists(metaPath, removeMetaError)) {
                    state_.log(LogLevel::Warning, removeMetaError);
                }

                proto::FileResult result;
                result.index = fileIndex;
                result.success = true;
                result.code = proto::ErrorCode::NoError;
                channel.send(proto::MessageType::FileResult,
                             proto::makeFileResult(result),
                             proto::kHandshakeTimeoutMs);
                updateProgress(finalPaths[i], true);
            }

            frame = frames->pop(proto::kIoTimeoutMs);
            while (frame.type == proto::MessageType::Ping) {
                channel.send(proto::MessageType::Pong,
                             proto::makePing(proto::parsePingPong(
                                 frame.payload)),
                             proto::kIoTimeoutMs);
                frame = frames->pop(proto::kIoTimeoutMs);
            }
            if (frame.type != proto::MessageType::Done) {
                throw Error(ErrorKind::Protocol, "缺少传输完成消息");
            }
            uint64_t doneTotal = 0;
            uint32_t doneFiles = 0;
            proto::parseDone(frame.payload, doneTotal, doneFiles);
            if (doneTotal != start.totalBytes || doneFiles != start.fileCount) {
                throw Error(ErrorKind::Protocol, "传输完成统计不一致");
            }
            channel.send(proto::MessageType::DoneAck,
                         proto::makeDoneAck("接收完成"),
                         proto::kHandshakeTimeoutMs);
            frames->stopAndJoin();
            state_.updateJob(jobId, [&](JobRecord& job) {
                job.bytesDone = start.totalBytes;
                job.currentFile.clear();
            });
            state_.finishJob(jobId, TransferState::Completed, "已完成");
            state_.log(LogLevel::LogSuccess,
                       "接收完成：" + std::to_string(fileCount) + " 个文件（" +
                           formatBytes(start.totalBytes) + "）来自 " +
                           peerDisplay);
        } catch (const Error& error) {
            if (jobId != 0) {
                if (stop_.load() || error.kind() == ErrorKind::Cancelled) {
                    state_.finishJob(jobId, TransferState::Canceled,
                                     "接收已中断", error.what());
                } else if (error.kind() == ErrorKind::Network ||
                           error.kind() == ErrorKind::Timeout) {
                    state_.finishJob(jobId, TransferState::Failed,
                                     "连接中断", error.what());
                } else {
                    state_.finishJob(jobId, TransferState::Failed,
                                     "接收失败", error.what());
                }
            }
            if (!stop_.load()) {
                if (error.kind() == ErrorKind::Network ||
                    error.kind() == ErrorKind::Timeout) {
                    if (jobId != 0) {
                        state_.log(LogLevel::Warning,
                                   "接收中断（" + peerDisplay +
                                       "）：连接已断开，未完成数据已保留，可断点续传");
                    }
                } else {
                    state_.log(LogLevel::Error,
                               "接收失败（" + peerDisplay + "）：" +
                                   error.what());
                    const proto::ErrorCode code = remoteCodeFor(error.kind());
                    try {
                        sendError(channel, code, truncateUtf8(error.what(), 1800));
                    } catch (...) {
                    }
                }
            }
        } catch (const std::exception& error) {
            if (jobId != 0) {
                state_.finishJob(jobId, TransferState::Failed, "接收失败",
                                 error.what());
            }
            if (!stop_.load()) {
                state_.log(LogLevel::Error,
                           "接收失败（" + peerDisplay + "）：" +
                               std::string(error.what()));
                try {
                    sendError(channel, proto::ErrorCode::Internal,
                              truncateUtf8(error.what(), 1800));
                } catch (...) {
                }
            }
        }
    }

    void sendError(proto::Channel& channel, proto::ErrorCode code,
                   const std::string& message) {
        channel.send(proto::MessageType::Error,
                     proto::makeError(code, truncateUtf8(message, 1800)),
                     proto::kHandshakeTimeoutMs);
    }

    CoreState& state_;
    uint16_t requestedPort_;
    uint16_t port_ = 0;
    std::unique_ptr<Socket> listener_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::thread acceptThread_;
    std::vector<std::thread> workers_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::unique_ptr<Socket>> queue_;
    std::mutex pathLockMutex_;
    std::unordered_set<std::string> activePaths_;
    std::mutex logMutex_;
    std::chrono::steady_clock::time_point lastAcceptErrorLog_{};
};

ReceiverServer::ReceiverServer(CoreState& state, uint16_t requestedPort)
    : impl_(std::make_unique<Impl>(state, requestedPort)) {}

ReceiverServer::~ReceiverServer() = default;

bool ReceiverServer::start(std::string& error) {
    return impl_->start(error);
}

void ReceiverServer::stop() { impl_->stop(); }

bool ReceiverServer::running() const { return impl_->running(); }

uint16_t ReceiverServer::port() const { return impl_->port(); }

}  // namespace landrop
