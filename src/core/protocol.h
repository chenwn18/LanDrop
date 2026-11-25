#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"
#include "sha256.h"
#include "socket.h"

namespace landrop {

namespace proto {

inline constexpr int kHandshakeTimeoutMs = 15000;
// Inactivity timeout. File data flows continuously; long local verification
// operations send Progress heartbeats at least once per second.
inline constexpr int kIoTimeoutMs = 30000;
inline constexpr uint32_t kProgressGeneral = 0xffffffffu;

enum class MessageType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Start = 3,
    Entry = 4,
    ManifestEnd = 5,
    Resume = 6,
    ResumeEnd = 7,
    FileBegin = 8,
    FileData = 9,
    FileEnd = 10,
    FileResult = 11,
    Done = 12,
    DoneAck = 13,
    Error = 14,
    Cancel = 15,
    Progress = 16,
    Ping = 17,
    Pong = 18,
};

enum class EntryKind : uint8_t { Directory = 0, File = 1 };

enum class ErrorCode : uint16_t {
    NoError = 0,
    Protocol = 1,
    IncomingDisabled = 2,
    Busy = 3,
    Disk = 4,
    PathRejected = 5,
    Internal = 6,
    Cancelled = 7,
    NoSpace = 8,
    Integrity = 9,
};

enum class HelloStatus : uint8_t { Ok = 0, Busy = 1, Rejected = 2, Version = 3 };

using TransferId = std::array<uint8_t, 16>;

struct Frame {
    MessageType type = MessageType::Hello;
    std::vector<uint8_t> payload;
};

struct Hello {
    uint16_t protocolVersion = kProtocolVersion;
    std::string deviceName;
};

struct HelloAck {
    HelloStatus status = HelloStatus::Ok;
    std::string serverName;
    std::string message;
};

struct Start {
    TransferId transferId{};
    uint32_t fileCount = 0;
    uint32_t directoryCount = 0;
    uint64_t totalBytes = 0;
};

struct Entry {
    uint32_t index = 0;
    EntryKind kind = EntryKind::File;
    uint64_t size = 0;
    Sha256::Digest digest{};
    std::string relativePath;
};

struct Resume {
    uint32_t index = 0;
    uint64_t offset = 0;
};

struct FileResult {
    uint32_t index = 0;
    bool success = false;
    ErrorCode code = ErrorCode::NoError;
    std::string message;
};

struct ErrorMessage {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

struct Progress {
    uint32_t index = 0;
    uint64_t bytesDone = 0;
    std::string message;
};

TransferId makeTransferId();

std::vector<uint8_t> makeHello(const Hello& hello);
Hello parseHello(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeHelloAck(const HelloAck& ack);
HelloAck parseHelloAck(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeStart(const Start& start);
Start parseStart(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeEntry(const Entry& entry);
Entry parseEntry(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeManifestEnd(uint32_t itemCount);
uint32_t parseManifestEnd(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeResume(const Resume& resume);
Resume parseResume(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeResumeEnd(uint32_t itemCount);
uint32_t parseResumeEnd(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeFileBegin(uint32_t index, uint64_t offset);
void parseFileBegin(const std::vector<uint8_t>& payload, uint32_t& index,
                    uint64_t& offset);

std::vector<uint8_t> makeFileData(uint32_t index, const void* data,
                                  std::size_t size);
void parseFileData(const std::vector<uint8_t>& payload, uint32_t& index,
                   const uint8_t*& data, std::size_t& size);

std::vector<uint8_t> makeFileEnd(uint32_t index);
uint32_t parseFileEnd(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeFileResult(const FileResult& result);
FileResult parseFileResult(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeDone(uint64_t totalBytes, uint32_t fileCount);
void parseDone(const std::vector<uint8_t>& payload, uint64_t& totalBytes,
               uint32_t& fileCount);

std::vector<uint8_t> makeDoneAck(const std::string& message);
std::string parseDoneAck(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeError(ErrorCode code, const std::string& message);
ErrorMessage parseError(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeCancel(const std::string& message);
std::string parseCancel(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makeProgress(uint32_t index, uint64_t bytesDone,
                                  const std::string& message);
Progress parseProgress(const std::vector<uint8_t>& payload);

std::vector<uint8_t> makePing(uint64_t token);
uint64_t parsePingPong(const std::vector<uint8_t>& payload);

class Channel {
public:
    Channel(Socket& socket, const std::atomic<bool>* abort);

    void send(MessageType type, const std::vector<uint8_t>& payload,
              int timeoutMs);
    Frame read(int timeoutMs);

private:
    Socket& socket_;
    const std::atomic<bool>* abort_;
};

}  // namespace proto
}  // namespace landrop
