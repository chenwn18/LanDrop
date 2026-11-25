#include "protocol.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "util.h"

namespace landrop {
namespace proto {
namespace {

constexpr uint8_t kMagic[4] = {'L', 'D', 'X', '1'};
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxMessageString = 2048;
constexpr std::size_t kMaxDeviceName = 255;

class Writer {
public:
    void u8(uint8_t value) { data_.push_back(value); }

    void u16(uint16_t value) {
        data_.push_back(static_cast<uint8_t>(value >> 8));
        data_.push_back(static_cast<uint8_t>(value));
    }

    void u32(uint32_t value) {
        data_.push_back(static_cast<uint8_t>(value >> 24));
        data_.push_back(static_cast<uint8_t>(value >> 16));
        data_.push_back(static_cast<uint8_t>(value >> 8));
        data_.push_back(static_cast<uint8_t>(value));
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            data_.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    void string(const std::string& value) {
        if (value.size() > 0xffffu) {
            throw Error(ErrorKind::Protocol, "协议字符串过长");
        }
        u16(static_cast<uint16_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void bytes(const void* data, std::size_t size) {
        const auto* source = static_cast<const uint8_t*>(data);
        data_.insert(data_.end(), source, source + size);
    }

    std::vector<uint8_t> take() { return std::move(data_); }

private:
    std::vector<uint8_t> data_;
};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data)
        : data_(data), position_(0) {}

    uint8_t u8() {
        require(1);
        return data_[position_++];
    }

    uint16_t u16() {
        require(2);
        const uint16_t value =
            (static_cast<uint16_t>(data_[position_]) << 8) |
            static_cast<uint16_t>(data_[position_ + 1]);
        position_ += 2;
        return value;
    }

    uint32_t u32() {
        require(4);
        const uint32_t value =
            (static_cast<uint32_t>(data_[position_]) << 24) |
            (static_cast<uint32_t>(data_[position_ + 1]) << 16) |
            (static_cast<uint32_t>(data_[position_ + 2]) << 8) |
            static_cast<uint32_t>(data_[position_ + 3]);
        position_ += 4;
        return value;
    }

    uint64_t u64() {
        require(8);
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | data_[position_ + static_cast<std::size_t>(i)];
        }
        position_ += 8;
        return value;
    }

    std::string string(std::size_t maxLength) {
        const uint16_t length = u16();
        require(length);
        if (length > maxLength) {
            throw Error(ErrorKind::Protocol, "协议字符串超长");
        }
        std::string result(reinterpret_cast<const char*>(data_.data() + position_),
                           length);
        position_ += length;
        return result;
    }

    void copyRaw(std::size_t size, void* destination) {
        require(size);
        std::memcpy(destination, data_.data() + position_, size);
        position_ += size;
    }

    void copyBytes(std::size_t size, Sha256::Digest& digest) {
        if (size != digest.size()) {
            throw Error(ErrorKind::Protocol, "摘要长度错误");
        }
        copyRaw(size, digest.data());
    }

    const uint8_t* remainingData() const {
        return data_.data() + position_;
    }

    std::size_t remaining() const { return data_.size() - position_; }

    void skip(std::size_t count) {
        require(count);
        position_ += count;
    }

    void finish() const {
        if (position_ != data_.size()) {
            throw Error(ErrorKind::Protocol, "协议消息包含多余数据");
        }
    }

private:
    void require(std::size_t count) const {
        if (count > data_.size() - position_) {
            throw Error(ErrorKind::Protocol, "协议消息被截断");
        }
    }

    const std::vector<uint8_t>& data_;
    std::size_t position_;
};

void sendOrThrow(Socket& socket, const void* data, std::size_t size,
                 int timeoutMs, const std::atomic<bool>* abort) {
    std::string error;
    if (!socket.sendAll(data, size, timeoutMs, abort, error)) {
        if (abort != nullptr && abort->load()) {
            throw Error(ErrorKind::Cancelled, error);
        }
        throw Error(ErrorKind::Network, error);
    }
}

void sendOrThrowVectored(Socket& socket, const void* first,
                         std::size_t firstSize, const void* second,
                         std::size_t secondSize, int timeoutMs,
                         const std::atomic<bool>* abort) {
    std::string error;
    if (!socket.sendAllVectored(first, firstSize, second, secondSize, timeoutMs,
                                abort, error)) {
        if (abort != nullptr && abort->load()) {
            throw Error(ErrorKind::Cancelled, error);
        }
        throw Error(ErrorKind::Network, error);
    }
}

}  // namespace

TransferId makeTransferId() {
    TransferId id{};
    const std::string hex = randomHex(id.size());
    for (std::size_t i = 0; i < id.size(); ++i) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            return static_cast<uint8_t>(c - 'a' + 10);
        };
        id[i] = static_cast<uint8_t>((nibble(hex[i * 2]) << 4) |
                                     nibble(hex[i * 2 + 1]));
    }
    return id;
}

std::vector<uint8_t> makeHello(const Hello& hello) {
    Writer writer;
    writer.u16(hello.protocolVersion);
    writer.string(hello.deviceName);
    return writer.take();
}

Hello parseHello(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    Hello hello;
    hello.protocolVersion = reader.u16();
    hello.deviceName = reader.string(kMaxDeviceName);
    reader.finish();
    return hello;
}

std::vector<uint8_t> makeHelloAck(const HelloAck& ack) {
    Writer writer;
    writer.u8(static_cast<uint8_t>(ack.status));
    writer.string(ack.serverName);
    writer.string(ack.message);
    return writer.take();
}

HelloAck parseHelloAck(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    HelloAck ack;
    ack.status = static_cast<HelloStatus>(reader.u8());
    ack.serverName = reader.string(kMaxDeviceName);
    ack.message = reader.string(kMaxMessageString);
    reader.finish();
    return ack;
}

std::vector<uint8_t> makeStart(const Start& start) {
    Writer writer;
    writer.bytes(start.transferId.data(), start.transferId.size());
    writer.u32(start.fileCount);
    writer.u32(start.directoryCount);
    writer.u64(start.totalBytes);
    return writer.take();
}

Start parseStart(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    Start start;
    reader.copyRaw(start.transferId.size(), start.transferId.data());
    start.fileCount = reader.u32();
    start.directoryCount = reader.u32();
    start.totalBytes = reader.u64();
    if (static_cast<uint64_t>(start.fileCount) + start.directoryCount >
        kMaxManifestEntries) {
        throw Error(ErrorKind::Protocol, "文件数量超过上限");
    }
    reader.finish();
    return start;
}

std::vector<uint8_t> makeEntry(const Entry& entry) {
    if (entry.relativePath.size() > kMaxRelativePathBytes) {
        throw Error(ErrorKind::Protocol, "路径超过协议上限");
    }
    Writer writer;
    writer.u32(entry.index);
    writer.u8(static_cast<uint8_t>(entry.kind));
    writer.u64(entry.size);
    writer.bytes(entry.digest.data(), entry.digest.size());
    writer.string(entry.relativePath);
    return writer.take();
}

Entry parseEntry(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    Entry entry;
    entry.index = reader.u32();
    entry.kind = static_cast<EntryKind>(reader.u8());
    entry.size = reader.u64();
    reader.copyBytes(32, entry.digest);
    entry.relativePath = reader.string(kMaxRelativePathBytes);
    reader.finish();
    return entry;
}

std::vector<uint8_t> makeManifestEnd(uint32_t itemCount) {
    Writer writer;
    writer.u32(itemCount);
    return writer.take();
}

uint32_t parseManifestEnd(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const uint32_t count = reader.u32();
    reader.finish();
    return count;
}

std::vector<uint8_t> makeResume(const Resume& resume) {
    Writer writer;
    writer.u32(resume.index);
    writer.u64(resume.offset);
    return writer.take();
}

Resume parseResume(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    Resume resume;
    resume.index = reader.u32();
    resume.offset = reader.u64();
    reader.finish();
    return resume;
}

std::vector<uint8_t> makeResumeEnd(uint32_t itemCount) {
    Writer writer;
    writer.u32(itemCount);
    return writer.take();
}

uint32_t parseResumeEnd(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const uint32_t count = reader.u32();
    reader.finish();
    return count;
}

std::vector<uint8_t> makeFileBegin(uint32_t index, uint64_t offset) {
    Writer writer;
    writer.u32(index);
    writer.u64(offset);
    return writer.take();
}

void parseFileBegin(const std::vector<uint8_t>& payload, uint32_t& index,
                    uint64_t& offset) {
    Reader reader(payload);
    index = reader.u32();
    offset = reader.u64();
    reader.finish();
}

std::vector<uint8_t> makeFileData(uint32_t index, const void* data,
                                  std::size_t size) {
    if (size > kMaxFramePayload - 4) {
        throw Error(ErrorKind::Protocol, "数据块超过协议上限");
    }
    Writer writer;
    writer.u32(index);
    writer.bytes(data, size);
    return writer.take();
}

void parseFileData(const std::vector<uint8_t>& payload, uint32_t& index,
                   const uint8_t*& data, std::size_t& size) {
    if (payload.size() < 4) {
        throw Error(ErrorKind::Protocol, "数据帧被截断");
    }
    Reader reader(payload);
    index = reader.u32();
    data = reader.remainingData();
    size = reader.remaining();
}

std::vector<uint8_t> makeFileEnd(uint32_t index) {
    Writer writer;
    writer.u32(index);
    return writer.take();
}

uint32_t parseFileEnd(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const uint32_t index = reader.u32();
    reader.finish();
    return index;
}

std::vector<uint8_t> makeFileResult(const FileResult& result) {
    Writer writer;
    writer.u32(result.index);
    writer.u8(result.success ? 1 : 0);
    writer.u16(static_cast<uint16_t>(result.code));
    writer.string(result.message);
    return writer.take();
}

FileResult parseFileResult(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    FileResult result;
    result.index = reader.u32();
    result.success = reader.u8() != 0;
    result.code = static_cast<ErrorCode>(reader.u16());
    result.message = reader.string(kMaxMessageString);
    reader.finish();
    return result;
}

std::vector<uint8_t> makeDone(uint64_t totalBytes, uint32_t fileCount) {
    Writer writer;
    writer.u64(totalBytes);
    writer.u32(fileCount);
    return writer.take();
}

void parseDone(const std::vector<uint8_t>& payload, uint64_t& totalBytes,
               uint32_t& fileCount) {
    Reader reader(payload);
    totalBytes = reader.u64();
    fileCount = reader.u32();
    reader.finish();
}

std::vector<uint8_t> makeDoneAck(const std::string& message) {
    Writer writer;
    writer.string(message);
    return writer.take();
}

std::string parseDoneAck(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const std::string message = reader.string(kMaxMessageString);
    reader.finish();
    return message;
}

std::vector<uint8_t> makeError(ErrorCode code, const std::string& message) {
    Writer writer;
    writer.u16(static_cast<uint16_t>(code));
    writer.string(message);
    return writer.take();
}

ErrorMessage parseError(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    ErrorMessage error;
    error.code = static_cast<ErrorCode>(reader.u16());
    error.message = reader.string(kMaxMessageString);
    reader.finish();
    return error;
}

std::vector<uint8_t> makeCancel(const std::string& message) {
    Writer writer;
    writer.string(message);
    return writer.take();
}

std::string parseCancel(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const std::string message = reader.string(kMaxMessageString);
    reader.finish();
    return message;
}

std::vector<uint8_t> makeProgress(uint32_t index, uint64_t bytesDone,
                                  const std::string& message) {
    Writer writer;
    writer.u32(index);
    writer.u64(bytesDone);
    writer.string(message);
    return writer.take();
}

Progress parseProgress(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    Progress progress;
    progress.index = reader.u32();
    progress.bytesDone = reader.u64();
    progress.message = reader.string(kMaxMessageString);
    reader.finish();
    return progress;
}

std::vector<uint8_t> makePing(uint64_t token) {
    Writer writer;
    writer.u64(token);
    return writer.take();
}

uint64_t parsePingPong(const std::vector<uint8_t>& payload) {
    Reader reader(payload);
    const uint64_t token = reader.u64();
    reader.finish();
    return token;
}

Channel::Channel(Socket& socket, const std::atomic<bool>* abort)
    : socket_(socket), abort_(abort) {}

void Channel::send(MessageType type, const std::vector<uint8_t>& payload,
                   int timeoutMs) {
    if (payload.size() > kMaxFramePayload) {
        throw Error(ErrorKind::Protocol, "发送帧过大");
    }
    uint8_t header[kHeaderSize];
    std::memcpy(header, kMagic, 4);
    header[4] = static_cast<uint8_t>(kProtocolVersion >> 8);
    header[5] = static_cast<uint8_t>(kProtocolVersion);
    const uint16_t typeValue = static_cast<uint16_t>(type);
    header[6] = static_cast<uint8_t>(typeValue >> 8);
    header[7] = static_cast<uint8_t>(typeValue);
    const uint32_t length = static_cast<uint32_t>(payload.size());
    header[8] = static_cast<uint8_t>(length >> 24);
    header[9] = static_cast<uint8_t>(length >> 16);
    header[10] = static_cast<uint8_t>(length >> 8);
    header[11] = static_cast<uint8_t>(length);
    if (!payload.empty()) {
        sendOrThrowVectored(socket_, header, sizeof(header), payload.data(),
                            payload.size(), timeoutMs, abort_);
    } else {
        sendOrThrow(socket_, header, sizeof(header), timeoutMs, abort_);
    }
}

Frame Channel::read(int timeoutMs) {
    Frame frame;
    readInto(frame, timeoutMs);
    return frame;
}

void Channel::readInto(Frame& frame, int timeoutMs) {
    uint8_t header[kHeaderSize]{};
    std::string error;
    if (!socket_.recvAll(header, sizeof(header), timeoutMs, abort_, error)) {
        if (abort_ != nullptr && abort_->load()) {
            throw Error(ErrorKind::Cancelled, error);
        }
        throw Error(ErrorKind::Network, error);
    }
    if (std::memcmp(header, kMagic, 4) != 0) {
        throw Error(ErrorKind::Protocol, "收到无效的数据流（magic 不匹配）");
    }
    const uint16_t version =
        static_cast<uint16_t>((header[4] << 8) | header[5]);
    if (version != kProtocolVersion) {
        throw Error(ErrorKind::Protocol, "协议版本不兼容");
    }
    const uint16_t type = static_cast<uint16_t>((header[6] << 8) | header[7]);
    const uint32_t length =
        (static_cast<uint32_t>(header[8]) << 24) |
        (static_cast<uint32_t>(header[9]) << 16) |
        (static_cast<uint32_t>(header[10]) << 8) |
        static_cast<uint32_t>(header[11]);
    if (length > kMaxFramePayload) {
        throw Error(ErrorKind::Protocol, "收到超大协议帧");
    }
    if (type < static_cast<uint16_t>(MessageType::Hello) ||
        type > static_cast<uint16_t>(MessageType::Pong)) {
        throw Error(ErrorKind::Protocol, "收到未知消息类型");
    }
    frame.type = static_cast<MessageType>(type);
    frame.payload.resize(length);
    if (length != 0 &&
        !socket_.recvAll(frame.payload.data(), length, timeoutMs, abort_,
                         error)) {
        if (abort_ != nullptr && abort_->load()) {
            throw Error(ErrorKind::Cancelled, error);
        }
        throw Error(ErrorKind::Network, error);
    }
}

}  // namespace proto
}  // namespace landrop
