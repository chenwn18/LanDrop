#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/app_controller.h"
#include "core/common.h"
#include "core/platform.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/socket.h"
#include "core/util.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace landrop;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "[FAIL] " << message << "\n";
    }
}

template <typename T>
void checkEqual(const T& actual, const T& expected, const std::string& message) {
    ++g_checks;
    if (!(actual == expected)) {
        ++g_failures;
        std::cerr << "[FAIL] " << message << " (actual != expected)\n";
    }
}

class WorkDir {
public:
    WorkDir() {
        path_ = fs::temp_directory_path() /
                pathFromUtf8("landrop-test-" + randomHex(8));
        fs::create_directories(path_);
    }
    ~WorkDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void writeTextFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readTextFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void writePatternFile(const fs::path& path, uint64_t size, uint32_t seed) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    std::mt19937 generator(seed);
    constexpr std::size_t kBuffer = 1024 * 1024;
    std::vector<char> buffer(kBuffer);
    uint64_t remaining = size;
    while (remaining > 0) {
        for (std::size_t i = 0; i < kBuffer; ++i) {
            buffer[i] = static_cast<char>(generator() & 0xffu);
        }
        const std::size_t toWrite = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, kBuffer));
        output.write(buffer.data(), static_cast<std::streamsize>(toWrite));
        remaining -= toWrite;
    }
}

Sha256::Digest hashFile(const fs::path& path) {
    FileReader reader;
    std::string error;
    if (!reader.open(path, error)) {
        throw std::runtime_error(error);
    }
    Sha256 hasher;
    std::vector<uint8_t> buffer(1024 * 1024);
    for (;;) {
        std::size_t obtained = 0;
        if (!reader.read(buffer.data(), buffer.size(), obtained, error)) {
            throw std::runtime_error(error);
        }
        if (obtained == 0) {
            break;
        }
        hasher.update(buffer.data(), obtained);
    }
    return hasher.finish();
}

struct TransferResult {
    bool completed = false;
    std::string details;
};

TransferResult waitForTransfer(AppController& source, AppController& receiver,
                               int timeoutMs = 60000) {
    TransferResult result;
    bool sawOutgoing = false;
    bool sawIncoming = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const UiSnapshot sender = source.snapshot();
        const UiSnapshot receive = receiver.snapshot();
        bool active = false;
        bool failed = false;
        std::string failure;
        for (const auto& job : sender.transfers) {
            if (!job.outgoing) continue;
            sawOutgoing = true;
            if (isActiveState(job.state)) active = true;
            if (job.state == TransferState::Failed ||
                job.state == TransferState::Canceled) {
                failed = true;
                failure = job.error.empty() ? job.status : job.error;
            }
        }
        for (const auto& job : receive.transfers) {
            if (job.outgoing) continue;
            sawIncoming = true;
            if (isActiveState(job.state)) active = true;
            if (job.state == TransferState::Failed ||
                job.state == TransferState::Canceled) {
                failed = true;
                if (failure.empty()) {
                    failure = job.error.empty() ? job.status : job.error;
                }
            }
        }
        if (failed) {
            result.details = failure;
            return result;
        }
        if (sawOutgoing && sawIncoming && !active) {
            result.completed = true;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    result.details = "timeout waiting for transfer";
    return result;
}

void printTransfers(const char* label, AppController& controller) {
    const UiSnapshot snapshot = controller.snapshot();
    std::cerr << "--- " << label << " ---\n";
    for (const auto& job : snapshot.transfers) {
        std::cerr << (job.outgoing ? "UP " : "DOWN ") << job.peer << " | "
                  << static_cast<int>(job.state) << " | " << job.status
                  << " | " << job.error << " | " << job.bytesDone << "/"
                  << job.totalBytes << "\n";
    }
}

void testSha256() {
    checkEqual(Sha256::toHex(Sha256::hash("", 0)),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495"
                           "991b7852b855"),
               "SHA-256 empty");
    checkEqual(Sha256::toHex(Sha256::hash("abc", 3)),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb4"
                           "10ff61f20015ad"),
               "SHA-256 abc");
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    checkEqual(Sha256::toHex(Sha256::hash(fox.data(), fox.size())),
               std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d"
                           "02d0bf37c9e592"),
               "SHA-256 fox");
    std::string longInput(200000, 'x');
    Sha256 incremental;
    std::size_t offset = 0;
    const std::size_t pieces[] = {1, 31, 64, 65, 1024, 77777};
    for (std::size_t piece : pieces) {
        if (offset >= longInput.size()) break;
        const std::size_t take =
            std::min(piece, longInput.size() - offset);
        incremental.update(longInput.data() + offset, take);
        offset += take;
    }
    if (offset < longInput.size()) {
        incremental.update(longInput.data() + offset,
                           longInput.size() - offset);
    }
    check(incremental.finish() == Sha256::hash(longInput.data(),
                                               longInput.size()),
          "SHA-256 incremental matches one-shot");
}

void testPaths() {
#ifdef _WIN32
    const std::string osError = platform::win32ErrorMessage(10061);
    check(!osError.empty() && isValidUtf8(osError),
          "localized Windows network error is valid UTF-8");
#endif
    std::string error;
    check(validateRelativePath("folder/file.txt", error),
          "valid relative path accepted");
    check(!validateRelativePath("", error), "empty path rejected");
    check(!validateRelativePath("../escape", error), "parent traversal rejected");
    check(!validateRelativePath("a/../../b", error),
          "nested parent traversal rejected");
    check(!validateRelativePath("/absolute", error), "absolute path rejected");
    check(!validateRelativePath("a\\b", error), "backslash rejected");
    check(!validateRelativePath("a//b", error), "empty component rejected");
    check(!validateRelativePath(".", error), "dot rejected");
    check(isValidUtf8("中文文件名"), "UTF-8 validation accepts Chinese");
    check(!isValidUtf8(std::string("\xff\xfe", 2)),
          "UTF-8 validation rejects invalid bytes");
}

void testProtocol() {
    proto::Start start;
    start.transferId.fill(7);
    start.fileCount = 3;
    start.directoryCount = 2;
    start.totalBytes = 9ULL << 32;
    const proto::Start decodedStart = proto::parseStart(proto::makeStart(start));
    check(decodedStart.transferId == start.transferId, "protocol transfer id");
    checkEqual(decodedStart.fileCount, start.fileCount,
               "protocol file count");
    checkEqual(decodedStart.totalBytes, start.totalBytes,
               "protocol large total");

    proto::Entry entry;
    entry.index = 4;
    entry.kind = proto::EntryKind::File;
    entry.size = (1ULL << 40) + 7;
    entry.relativePath = "目录/大文件.bin";
    entry.digest = Sha256::hash("hello", 5);
    const proto::Entry decodedEntry =
        proto::parseEntry(proto::makeEntry(entry));
    checkEqual(decodedEntry.index, entry.index, "entry index");
    checkEqual(decodedEntry.size, entry.size, "entry 64-bit size");
    check(decodedEntry.digest == entry.digest, "entry digest");
    checkEqual(decodedEntry.relativePath, entry.relativePath,
               "entry unicode path");

    const std::vector<uint8_t> payload =
        proto::makeFileBegin(42, (1ULL << 33) + 5);
    uint32_t index = 0;
    uint64_t offset = 0;
    proto::parseFileBegin(payload, index, offset);
    checkEqual(index, 42u, "file begin index");
    checkEqual(offset, static_cast<uint64_t>((1ULL << 33) + 5),
               "file begin 64-bit offset");

    const proto::FileResult resultIn =
        proto::parseFileResult(proto::makeFileResult(
            proto::FileResult{9, false, proto::ErrorCode::Integrity,
                              "checksum"}));
    checkEqual(resultIn.index, 9u, "file result index");
    check(!resultIn.success, "file result failure");
    check(resultIn.code == proto::ErrorCode::Integrity,
          "file result code");
}


class CutProxy {
public:
    CutProxy(uint16_t targetPort, uint64_t cutAfter)
        : targetPort_(targetPort), cutAfter_(cutAfter) {}
    ~CutProxy() { stop(); }

    bool start(std::string& error) {
        uint16_t bound = 0;
        listener_ = Socket::createListener(0, bound, error);
        if (!listener_) return false;
        port_ = bound;
        stop_.store(false);
        try {
            thread_ = std::thread([this]() { run(); });
        } catch (const std::exception& exception) {
            error = exception.what();
            listener_.reset();
            return false;
        }
        return true;
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        listener_.reset();
    }

    uint16_t port() const { return port_; }

private:
    static bool wouldBlock() {
#ifdef _WIN32
        const int code = WSAGetLastError();
        return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    void run() {
        while (!stop_.load()) {
            bool timedOut = false;
            bool aborted = false;
            std::string error;
            auto client = listener_->acceptConnection(250, &stop_, timedOut,
                                                      aborted, error);
            if (!client) {
                if (aborted || stop_.load()) break;
                continue;
            }
            Socket upstream;
            if (!upstream.connectTo("127.0.0.1", targetPort_, 4000, nullptr,
                                    error)) {
                continue;
            }
            pump(*client, upstream);
        }
    }

    void pump(Socket& client, Socket& upstream) {
        std::array<char, 64 * 1024> buffer{};
        uint64_t clientBytes = 0;
        bool canCutFirst = !cutDone_;
        for (;;) {
            if (stop_.load()) break;
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(client.native(), &readSet);
            FD_SET(upstream.native(), &readSet);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
#ifdef _WIN32
            const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int selected = select(
                std::max(client.native(), upstream.native()) + 1, &readSet,
                nullptr, nullptr, &timeout);
#endif
            if (selected <= 0) continue;

            if (FD_ISSET(client.native(), &readSet)) {
                const int got = static_cast<int>(
                    recv(client.native(), buffer.data(),
                         static_cast<int>(buffer.size()), 0));
                if (got <= 0) {
                    if (wouldBlock()) continue;
                    break;
                }
                if (canCutFirst &&
                    clientBytes + static_cast<uint64_t>(got) >= cutAfter_) {
                    const std::size_t allowed = static_cast<std::size_t>(
                        cutAfter_ > clientBytes ? cutAfter_ - clientBytes : 0);
                    if (allowed != 0) {
                        std::string error;
                        (void)upstream.sendAll(buffer.data(), allowed, 3000,
                                               &stop_, error);
                    }
                    cutDone_ = true;
                    canCutFirst = false;
                    break;
                }
                std::string error;
                if (!upstream.sendAll(buffer.data(),
                                      static_cast<std::size_t>(got), 5000,
                                      &stop_, error)) {
                    break;
                }
                clientBytes += static_cast<uint64_t>(got);
            }
            if (FD_ISSET(upstream.native(), &readSet)) {
                const int got = static_cast<int>(
                    recv(upstream.native(), buffer.data(),
                         static_cast<int>(buffer.size()), 0));
                if (got <= 0) {
                    if (wouldBlock()) continue;
                    break;
                }
                std::string error;
                if (!client.sendAll(buffer.data(),
                                    static_cast<std::size_t>(got), 5000,
                                    &stop_, error)) {
                    break;
                }
            }
        }
    }

    uint16_t targetPort_ = 0;
    uint64_t cutAfter_ = 0;
    uint16_t port_ = 0;
    std::unique_ptr<Socket> listener_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool cutDone_ = false;
};

// Minimal fake receiver that reports a local disk error while file data is
// still streaming. Verifies that the sender surfaces the remote reason
// instead of treating it as a generic connection loss.
class RemoteErrorServer {
public:
    ~RemoteErrorServer() { stop(); }

    bool start(std::string& error) {
        uint16_t bound = 0;
        listener_ = Socket::createListener(0, bound, error);
        if (!listener_) return false;
        port_ = bound;
        stop_.store(false);
        try {
            thread_ = std::thread([this]() { run(); });
        } catch (const std::exception& exception) {
            error = exception.what();
            listener_.reset();
            return false;
        }
        return true;
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        listener_.reset();
    }

    uint16_t port() const { return port_; }
    bool errorSent() const { return errorSent_.load(); }
    bool sawFileEndAfterError() const {
        return fileEndAfterError_.load();
    }

private:
    void run() {
        while (!stop_.load()) {
            bool timedOut = false;
            bool aborted = false;
            std::string error;
            auto connection = listener_->acceptConnection(250, &stop_,
                                                          timedOut, aborted,
                                                          error);
            if (!connection) {
                if (aborted || stop_.load()) return;
                continue;
            }
            // Keep the accepted receive window small so the sender cannot
            // finish a whole file before the Error below reaches it.
            int receiveBuffer = 8192;
            (void)setsockopt(connection->native(), SOL_SOCKET, SO_RCVBUF,
                             reinterpret_cast<const char*>(&receiveBuffer),
                             sizeof(receiveBuffer));
            proto::Channel channel(*connection, &stop_);
            try {
                proto::Frame frame = channel.read(5000);
                if (frame.type != proto::MessageType::Hello) continue;
                channel.send(
                    proto::MessageType::HelloAck,
                    proto::makeHelloAck(proto::HelloAck{
                        proto::HelloStatus::Ok, "FakeReceiver", ""}),
                    proto::kHandshakeTimeoutMs);
                frame = channel.read(5000);
                if (frame.type != proto::MessageType::Start) continue;
                uint32_t itemCount = 0;
                for (;;) {
                    frame = channel.read(5000);
                    if (frame.type == proto::MessageType::Entry) {
                        ++itemCount;
                        continue;
                    }
                    if (frame.type == proto::MessageType::ManifestEnd) {
                        break;
                    }
                    throw std::runtime_error("unexpected manifest frame");
                }
                for (uint32_t i = 0; i < itemCount; ++i) {
                    channel.send(proto::MessageType::Resume,
                                 proto::makeResume(proto::Resume{i, 0}),
                                 proto::kHandshakeTimeoutMs);
                }
                channel.send(proto::MessageType::ResumeEnd,
                             proto::makeResumeEnd(itemCount),
                             proto::kHandshakeTimeoutMs);
                frame = channel.read(5000);
                if (frame.type != proto::MessageType::FileBegin) continue;
                // Report the error as soon as data starts. Sending it before
                // the first chunk makes the assertion deterministic: a sender
                // that polls while writing must never reach FileEnd.
                channel.send(
                    proto::MessageType::Error,
                    proto::makeError(proto::ErrorCode::Disk,
                                     "模拟磁盘已满"),
                    proto::kHandshakeTimeoutMs);
                errorSent_.store(true);
                // Keep draining so the sender's poll can observe the Error
                // while it is still writing.
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
                while (!stop_.load() &&
                       std::chrono::steady_clock::now() < deadline) {
                    try {
                        const proto::Frame drained = channel.read(250);
                        if (drained.type == proto::MessageType::FileEnd) {
                            fileEndAfterError_.store(true);
                        }
                    } catch (...) {
                        break;
                    }
                }
            } catch (...) {
            }
        }
    }

    uint16_t port_ = 0;
    std::unique_ptr<Socket> listener_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> errorSent_{false};
    std::atomic<bool> fileEndAfterError_{false};
    std::thread thread_;
};

void testRemoteErrorDuringData(WorkDir& work) {
    const fs::path sourceFile =
        work.path() / "remote-error-source" / "payload.bin";
    writePatternFile(sourceFile, 8 * 1024 * 1024, 123);

    RemoteErrorServer server;
    std::string error;
    check(server.start(error), "remote error server starts: " + error);

    AppController sender;
    ControllerOptions options;
    options.receiveDirectory = work.path() / "remote-error-inbox";
    options.deviceName = "ErrorSender";
    options.listenPort = 0;
    options.enableDiscovery = false;
    std::string warning;
    check(sender.start(options, warning), "remote error sender starts");
    check(sender.send("127.0.0.1:" + std::to_string(server.port()),
                      {sourceFile}, warning) != 0,
          "remote error send accepted");

    bool failed = false;
    std::string detail;
    uint64_t doneBytes = 0;
    uint64_t totalBytes = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& job : sender.snapshot().transfers) {
            if (job.outgoing && job.state == TransferState::Failed) {
                failed = true;
                detail = job.error.empty() ? job.status : job.error;
                doneBytes = job.bytesDone;
                totalBytes = job.totalBytes;
            }
        }
        if (failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    check(server.errorSent(), "remote error frame was sent");
    check(failed, "sender fails on remote mid-stream error");
    check(detail.find("模拟磁盘已满") != std::string::npos,
          "sender surfaces remote mid-stream error: " + detail);
    check(totalBytes == 8 * 1024 * 1024 && doneBytes < totalBytes / 2,
          "sender stops mid-stream instead of sending the whole file");
    check(!server.sawFileEndAfterError(),
          "sender aborts before sending FileEnd after remote error");
    sender.stop();
    server.stop();
}

void testSocketPoll() {
    uint16_t port = 0;
    std::string error;
    check(platform::initializeNetwork(error),
          "network initializes for poll test: " + error);
    auto listener = Socket::createListener(0, port, error);
    check(listener != nullptr, "poll listener creates: " + error);
    if (!listener) return;

    Socket client;
    std::thread connector([&]() {
        std::string connectError;
        check(client.connectTo("127.0.0.1", port, 3000, nullptr,
                               connectError),
              "poll client connects: " + connectError);
    });
    bool timedOut = false;
    bool aborted = false;
    auto server = listener->acceptConnection(3000, nullptr, timedOut,
                                             aborted, error);
    connector.join();
    check(server != nullptr, "poll server accepts: " + error);
    if (!server) return;

    bool abortedFlag = false;
    check(!server->waitReadable(0, nullptr, abortedFlag, error),
          "poll reports no data before send");
    const char byte = 'x';
    check(client.sendAll(&byte, 1, 2000, nullptr, error),
          "poll client sends byte: " + error);
    bool sawReadable = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server->waitReadable(10, nullptr, abortedFlag, error)) {
            sawReadable = true;
            break;
        }
    }
    check(sawReadable, "poll observes sent data");
}

void testTransfer(WorkDir& work) {
    const fs::path sourceRoot = work.path() / "source";
    const fs::path receiveRoot = work.path() / "received";
    const fs::path folder = sourceRoot / "alpha";
    fs::create_directories(folder / "empty" / "nested-empty");
    writeTextFile(folder / "hello.txt", "hello from LanDrop\n");
    writeTextFile(folder / "中文名称.txt", "Unicode filename content\n");
    writeTextFile(folder / "zero-byte.txt", "");
    writeTextFile(folder / "name with spaces.txt", "spaces\n");
    writePatternFile(folder / "nested" / "payload.bin", 12 * 1024 * 1024, 11);
    fs::create_directories(receiveRoot);

    AppController receiver;
    ControllerOptions receiverOptions;
    receiverOptions.receiveDirectory = receiveRoot;
    receiverOptions.deviceName = "TestReceiver";
    receiverOptions.listenPort = 0;
    receiverOptions.enableDiscovery = false;
    receiverOptions.allowIncoming = true;
    std::string warning;
    check(receiver.start(receiverOptions, warning),
          "receiver controller starts");
    const uint16_t port = receiver.listenPort();
    check(port != 0, "receiver got ephemeral port");

    AppController sender;
    ControllerOptions senderOptions;
    senderOptions.receiveDirectory = work.path() / "sender-inbox";
    senderOptions.deviceName = "TestSender";
    senderOptions.listenPort = 0;
    senderOptions.enableDiscovery = false;
    sender.start(senderOptions, warning);
    // A trailing separator must not turn the folder name into "root".
    const fs::path folderWithSlash(folder.string() + "/");
    check(sender.send("127.0.0.1:" + std::to_string(port),
                      {folderWithSlash}, warning) != 0,
          "send job accepted: " + warning);

    const TransferResult result = waitForTransfer(sender, receiver);
    if (!result.completed) {
        std::cerr << "transfer failed: " << result.details << "\n";
        printTransfers("sender", sender);
        printTransfers("receiver", receiver);
    }
    check(result.completed, "end-to-end folder transfer completes");

    const fs::path received = receiveRoot / "alpha";
    check(fs::is_directory(received / "empty" / "nested-empty"),
          "empty nested directory preserved");
    check(readTextFile(received / "hello.txt") == "hello from LanDrop\n",
          "small file content");
    check(readTextFile(received / "中文名称.txt") ==
              "Unicode filename content\n",
          "unicode filename and content");
    check(fs::exists(received / "zero-byte.txt") &&
              fs::file_size(received / "zero-byte.txt") == 0,
          "empty file transferred");
    check(readTextFile(received / "name with spaces.txt") == "spaces\n",
          "filename with spaces transferred");
    check(hashFile(received / "nested" / "payload.bin") ==
              hashFile(folder / "nested" / "payload.bin"),
          "large file checksum matches");
    const fs::path stateDirectory = receiveRoot / ".landrop-parts";
    bool leftovers = false;
    if (fs::exists(stateDirectory)) {
        for (const auto& entry : fs::directory_iterator(stateDirectory)) {
            (void)entry;
            leftovers = true;
        }
    }
    check(!leftovers, "no partial files remain after successful transfer");

    // Reverse direction: both instances must be able to send and receive.
    const fs::path reverseSource =
        work.path() / "reverse-source" / "from-receiver.txt";
    writeTextFile(reverseSource, "reverse direction payload\n");
    check(receiver.send(
              "127.0.0.1:" + std::to_string(sender.listenPort()),
              {reverseSource}, warning) != 0,
          "reverse send accepted");
    const fs::path reverseTarget =
        senderOptions.receiveDirectory / "from-receiver.txt";
    const auto reverseDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < reverseDeadline &&
           !fs::exists(reverseTarget)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(fs::exists(reverseTarget), "reverse direction file arrives");
    if (fs::exists(reverseTarget)) {
        check(readTextFile(reverseTarget) == "reverse direction payload\n",
              "reverse direction content matches");
    }

    sender.stop();
    receiver.stop();
}

void writePartMetaForTest(const fs::path& path,
                          const Sha256::Digest& digest, uint64_t size) {
    std::array<uint8_t, 48> data{};
    data[0] = 'L';
    data[1] = 'D';
    data[2] = 'P';
    data[3] = '1';
    data[4] = 1;
    for (int i = 0; i < 8; ++i) {
        data[8 + i] = static_cast<uint8_t>(size >>
                                           (56 - static_cast<unsigned>(i) * 8));
    }
    std::copy(digest.begin(), digest.end(), data.begin() + 16);
    std::string error;
    if (!writeFileAtomic(path, data.data(), data.size(), error)) {
        throw std::runtime_error(error);
    }
}

void testAutomaticResume(WorkDir& work) {
    const fs::path sourceRoot = work.path() / "auto-resume-source";
    const fs::path receiveRoot = work.path() / "auto-resume-received";
    const fs::path folder = sourceRoot / "delta";
    const fs::path sourceFile = folder / "payload.bin";
    writePatternFile(sourceFile, 16 * 1024 * 1024, 91);
    fs::create_directories(receiveRoot);

    AppController receiver;
    ControllerOptions receiverOptions;
    receiverOptions.receiveDirectory = receiveRoot;
    receiverOptions.deviceName = "AutoResumeReceiver";
    receiverOptions.listenPort = 0;
    receiverOptions.enableDiscovery = false;
    std::string warning;
    check(receiver.start(receiverOptions, warning),
          "auto-resume receiver starts");

    CutProxy proxy(receiver.listenPort(), 5 * 1024 * 1024);
    std::string proxyError;
    check(proxy.start(proxyError), "cut proxy starts: " + proxyError);

    AppController sender;
    ControllerOptions senderOptions;
    senderOptions.receiveDirectory = work.path() / "auto-resume-sender-inbox";
    senderOptions.deviceName = "AutoResumeSender";
    senderOptions.listenPort = 0;
    senderOptions.enableDiscovery = false;
    sender.start(senderOptions, warning);
    check(sender.send("127.0.0.1:" + std::to_string(proxy.port()), {folder},
                      warning) != 0,
          "auto-resume send accepted");

    const fs::path destination = receiveRoot / "delta" / "payload.bin";
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(90);
    bool failed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(destination)) break;
        const UiSnapshot snapshot = sender.snapshot();
        bool active = false;
        for (const auto& job : snapshot.transfers) {
            if (job.outgoing && isActiveState(job.state)) active = true;
            if (job.outgoing && job.state == TransferState::Failed) {
                failed = true;
            }
        }
        if (failed && !active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!fs::exists(destination)) {
        std::cerr << "auto-resume transfer failed\n";
        printTransfers("sender", sender);
        printTransfers("receiver", receiver);
    }
    check(fs::exists(destination),
          "automatic reconnect produced destination file");
    if (fs::exists(destination)) {
        check(hashFile(destination) == hashFile(sourceFile),
              "automatic reconnect checksum matches");
    }
    bool sawResume = false;
    for (const auto& log : receiver.logsAfter(0)) {
        if (log.text.find("继续接收") != std::string::npos) {
            sawResume = true;
        }
    }
    check(sawResume, "receiver logged automatic resume after connection cut");
    proxy.stop();
    sender.stop();
    receiver.stop();
}

void testUnicodeCaseCollision(WorkDir& work) {
    const fs::path sourceA = work.path() / "unicode-a";
    const fs::path sourceB = work.path() / "unicode-b";
    const fs::path receiveRoot = work.path() / "unicode-received";
    const fs::path fileA = sourceA / "Ä.txt";
    const fs::path fileB = sourceB / "ä.txt";
    writeTextFile(fileA, "uppercase umlaut\n");
    writeTextFile(fileB, "lowercase umlaut\n");
    fs::create_directories(receiveRoot);

    AppController receiver;
    ControllerOptions receiverOptions;
    receiverOptions.receiveDirectory = receiveRoot;
    receiverOptions.deviceName = "UnicodeReceiver";
    receiverOptions.listenPort = 0;
    receiverOptions.enableDiscovery = false;
    std::string warning;
    check(receiver.start(receiverOptions, warning), "unicode receiver starts");

    AppController sender;
    ControllerOptions senderOptions;
    senderOptions.receiveDirectory = work.path() / "unicode-sender-inbox";
    senderOptions.deviceName = "UnicodeSender";
    senderOptions.listenPort = 0;
    senderOptions.enableDiscovery = false;
    sender.start(senderOptions, warning);
    check(sender.send("127.0.0.1:" + std::to_string(receiver.listenPort()),
                      {fileA, fileB}, warning) != 0,
          "unicode-case send accepted");

    const TransferResult result = waitForTransfer(sender, receiver);
    check(result.completed, "unicode-case transfer completes");
    int regularFiles = 0;
    bool sawUppercase = false;
    bool sawLowercase = false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(receiveRoot, ec)) {
        if (!entry.is_regular_file()) continue;
        ++regularFiles;
        const std::string content = readTextFile(entry.path());
        if (content == "uppercase umlaut\n") sawUppercase = true;
        if (content == "lowercase umlaut\n") sawLowercase = true;
    }
    check(regularFiles == 2,
          "unicode case variants both survive without overwrite");
    check(sawUppercase && sawLowercase,
          "both unicode case variant contents are present");

    sender.stop();
    receiver.stop();
}

void testExistingFileSkip(WorkDir& work) {
    const fs::path sourceRoot = work.path() / "skip-source";
    const fs::path receiveRoot = work.path() / "skip-received";
    const fs::path folder = sourceRoot / "existing-data";
    writeTextFile(folder / "already-there.txt", "verified already\n");
    writeTextFile(folder / "needs-send.txt", "send me\n");
    fs::create_directories(receiveRoot / "existing-data");
    writeTextFile(receiveRoot / "existing-data" / "already-there.txt",
                  "verified already\n");

    AppController receiver;
    ControllerOptions receiverOptions;
    receiverOptions.receiveDirectory = receiveRoot;
    receiverOptions.deviceName = "SkipReceiver";
    receiverOptions.listenPort = 0;
    receiverOptions.enableDiscovery = false;
    std::string warning;
    check(receiver.start(receiverOptions, warning), "skip receiver starts");

    AppController sender;
    ControllerOptions senderOptions;
    senderOptions.receiveDirectory = work.path() / "skip-sender-inbox";
    senderOptions.deviceName = "SkipSender";
    senderOptions.listenPort = 0;
    senderOptions.enableDiscovery = false;
    sender.start(senderOptions, warning);
    check(sender.send("127.0.0.1:" + std::to_string(receiver.listenPort()),
                      {folder}, warning) != 0,
          "skip send accepted");

    const TransferResult result = waitForTransfer(sender, receiver);
    check(result.completed, "existing-file skip transfer completes");
    check(readTextFile(receiveRoot / "existing-data" /
                       "needs-send.txt") == "send me\n",
          "new file transferred while existing file skipped");
    check(readTextFile(receiveRoot / "existing-data" /
                       "already-there.txt") == "verified already\n",
          "existing file remains intact");
    bool sawSkip = false;
    for (const auto& log : receiver.logsAfter(0)) {
        if (log.text.find("跳过重传") != std::string::npos) {
            sawSkip = true;
        }
    }
    check(sawSkip, "receiver skipped a verified existing destination");

    sender.stop();
    receiver.stop();
}

bool sendLoopbackUdp(uint16_t port, const std::vector<uint8_t>& payload) {
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int sent = static_cast<int>(
        sendto(sock, reinterpret_cast<const char*>(payload.data()),
               static_cast<int>(payload.size()), 0,
               reinterpret_cast<sockaddr*>(&address), sizeof(address)));
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return sent == static_cast<int>(payload.size());
}

std::vector<uint8_t> makeDiscoveryProbe(const std::string& name,
                                        uint16_t tcpPort) {
    std::vector<uint8_t> packet = {'L', 'D', 'S', 'C', 0, 1,
                                   static_cast<uint8_t>(tcpPort >> 8),
                                   static_cast<uint8_t>(tcpPort)};
    for (int i = 0; i < 16; ++i) {
        packet.push_back(static_cast<uint8_t>(0xa0 + i));
    }
    packet.push_back(static_cast<uint8_t>(name.size() >> 8));
    packet.push_back(static_cast<uint8_t>(name.size()));
    packet.insert(packet.end(), name.begin(), name.end());
    return packet;
}

void testDiscoveryShutdown(WorkDir& work) {
    const uint16_t discoveryPort = 48854;
    AppController controller;
    ControllerOptions options;
    options.receiveDirectory = work.path() / "discovery-inbox";
    options.deviceName = "DiscoveryHost";
    options.listenPort = 0;
    options.discoveryPort = discoveryPort;
    options.enableDiscovery = true;
    std::string warning;
    check(controller.start(options, warning), "discovery controller starts");
    check(controller.snapshot().discoveryActive, "discovery socket is active");

    const std::string peerName = "DiscoveryProbe";
    check(sendLoopbackUdp(discoveryPort,
                          makeDiscoveryProbe(peerName, kDefaultTcpPort)),
          "loopback discovery datagram sent");
    bool sawPeer = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& peer : controller.snapshot().peers) {
            if (peer.name == peerName) {
                sawPeer = true;
            }
        }
        if (sawPeer) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(sawPeer, "discovery received loopback beacon");

    const auto stopStart = std::chrono::steady_clock::now();
    controller.stop();
    const double stopSeconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - stopStart)
                                   .count();
    check(stopSeconds < 5.0, "discovery shutdown is prompt");
}

void testResume(WorkDir& work) {
    const fs::path sourceRoot = work.path() / "resume-source";
    const fs::path receiveRoot = work.path() / "resume-received";
    const fs::path folder = sourceRoot / "beta";
    const fs::path bigSource = folder / "payload.bin";
    writePatternFile(bigSource, 4 * 1024 * 1024, 77);
    writeTextFile(folder / "small.txt", "resume test\n");
    fs::create_directories(receiveRoot);

    const fs::path normalizedRoot =
        fs::absolute(receiveRoot).lexically_normal();
    const fs::path finalFile = normalizedRoot / "beta" / "payload.bin";
    const std::string key = pathCollisionKey(finalFile);
    const std::string partName =
        Sha256::toHex(Sha256::hash(key.data(), key.size())) + ".part";
    const fs::path stateDir = normalizedRoot / ".landrop-parts";
    fs::create_directories(stateDir);
    const fs::path partPath = stateDir / pathFromUtf8(partName);
    const fs::path metaPath = stateDir / pathFromUtf8(partName + ".meta");

    constexpr uint64_t kPrefix = 1536 * 1024;
    {
        FileReader input;
        std::string error;
        check(input.open(bigSource, error), "open resume source");
        FileWriter output;
        check(output.openAt(partPath, 0, error), "create resume part");
        std::vector<uint8_t> buffer(64 * 1024);
        uint64_t written = 0;
        while (written < kPrefix) {
            std::size_t obtained = 0;
            check(input.read(buffer.data(),
                             static_cast<std::size_t>(
                                 std::min<uint64_t>(buffer.size(),
                                                    kPrefix - written)),
                             obtained, error),
                  "read resume prefix");
            if (obtained == 0) break;
            check(output.writeAll(buffer.data(), obtained, error),
                  "write resume prefix");
            written += obtained;
        }
        check(written == kPrefix, "resume prefix length");
    }
    writePartMetaForTest(metaPath, hashFile(bigSource),
                         static_cast<uint64_t>(fs::file_size(bigSource)));

    AppController receiver;
    ControllerOptions receiverOptions;
    receiverOptions.receiveDirectory = receiveRoot;
    receiverOptions.deviceName = "ResumeReceiver";
    receiverOptions.listenPort = 0;
    receiverOptions.enableDiscovery = false;
    std::string warning;
    check(receiver.start(receiverOptions, warning), "resume receiver starts");

    AppController sender;
    ControllerOptions senderOptions;
    senderOptions.receiveDirectory = work.path() / "resume-sender-inbox";
    senderOptions.deviceName = "ResumeSender";
    senderOptions.listenPort = 0;
    senderOptions.enableDiscovery = false;
    sender.start(senderOptions, warning);
    check(sender.send("127.0.0.1:" + std::to_string(receiver.listenPort()),
                      {folder}, warning) != 0,
          "resume send job accepted");

    const TransferResult result = waitForTransfer(sender, receiver);
    if (!result.completed) {
        std::cerr << "resume transfer failed: " << result.details << "\n";
        printTransfers("sender", sender);
        printTransfers("receiver", receiver);
    }
    check(result.completed, "resume transfer completes");
    check(hashFile(finalFile) == hashFile(bigSource),
          "resumed file checksum matches");
    bool sawResume = false;
    for (const auto& log : receiver.logsAfter(0)) {
        if (log.text.find("继续接收") != std::string::npos) {
            sawResume = true;
        }
    }
    check(sawResume, "receiver logged resumed transfer");
    check(!fs::exists(partPath) && !fs::exists(metaPath),
          "partial state removed after resume completes");

    sender.stop();
    receiver.stop();
}

void testTraversalRejection(WorkDir& work) {
    const fs::path receiveRoot = work.path() / "traversal-received";
    fs::create_directories(receiveRoot);
    AppController receiver;
    ControllerOptions options;
    options.receiveDirectory = receiveRoot;
    options.deviceName = "TraversalReceiver";
    options.listenPort = 0;
    options.enableDiscovery = false;
    std::string warning;
    check(receiver.start(options, warning),
          "traversal receiver starts");

    Socket socket;
    std::string error;
    check(socket.connectTo("127.0.0.1", receiver.listenPort(), 5000, nullptr,
                           error),
          "raw client connects: " + error);
    proto::Channel channel(socket, nullptr);
    channel.send(proto::MessageType::Hello,
                 proto::makeHello(proto::Hello{kProtocolVersion, "Attacker"}),
                 proto::kHandshakeTimeoutMs);
    const proto::Frame ackFrame = channel.read(proto::kHandshakeTimeoutMs);
    check(ackFrame.type == proto::MessageType::HelloAck,
          "raw client got hello ack");

    proto::Start start;
    start.transferId = proto::makeTransferId();
    start.fileCount = 1;
    start.directoryCount = 0;
    start.totalBytes = 4;
    channel.send(proto::MessageType::Start, proto::makeStart(start),
                 proto::kHandshakeTimeoutMs);
    proto::Entry entry;
    entry.index = 0;
    entry.kind = proto::EntryKind::File;
    entry.size = 4;
    entry.relativePath = "../escape.txt";
    entry.digest = Sha256::hash("evil", 4);
    channel.send(proto::MessageType::Entry, proto::makeEntry(entry),
                 proto::kHandshakeTimeoutMs);
    channel.send(proto::MessageType::ManifestEnd, proto::makeManifestEnd(1),
                 proto::kHandshakeTimeoutMs);

    bool gotPathError = false;
    try {
        const proto::Frame response = channel.read(5000);
        if (response.type == proto::MessageType::Error) {
            const proto::ErrorMessage remote = proto::parseError(response.payload);
            gotPathError = remote.code == proto::ErrorCode::PathRejected;
        }
    } catch (const std::exception&) {
        // A closed connection is also an acceptable rejection as long as no
        // file escaped.
    }
    check(gotPathError, "receiver rejects parent traversal path");
    check(!fs::exists(receiveRoot.parent_path() / "escape.txt"),
          "path traversal did not create a file outside inbox");
    receiver.stop();
}

}  // namespace

int main() {
    try {
        WorkDir work;
        testSha256();
        testPaths();
        testProtocol();
        testSocketPoll();
        testTransfer(work);
        testRemoteErrorDuringData(work);
        testUnicodeCaseCollision(work);
        testExistingFileSkip(work);
        testResume(work);
        testAutomaticResume(work);
        testDiscoveryShutdown(work);
        testTraversalRejection(work);
    } catch (const std::exception& error) {
        ++g_failures;
        std::cerr << "[FAIL] unexpected exception: " << error.what() << "\n";
    }
    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
