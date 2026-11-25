#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace landrop {

inline constexpr const char* kAppName = "LanDrop";
inline constexpr const char* kAppVersion = "1.0.0";
inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint16_t kDefaultTcpPort = 47653;
inline constexpr uint16_t kDefaultDiscoveryPort = 47654;
inline constexpr std::size_t kMaxFramePayload = 1024 * 1024;
// Fill a complete protocol frame per write. The four-byte file index is part
// of every FileData payload, so the data chunk is four bytes smaller.
inline constexpr std::size_t kTransferChunkSize = kMaxFramePayload - 4;
inline constexpr std::size_t kMaxRelativePathBytes = 4096;
inline constexpr uint32_t kMaxManifestEntries = 200000;
inline constexpr std::size_t kMaxManifestPathBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxManifestPathComponents = 1000000;
inline constexpr std::size_t kMaxManifestPathDepth = 128;

enum class ErrorKind {
    NoError,
    Cancelled,
    Timeout,
    Network,
    Protocol,
    Rejected,
    Busy,
    LocalIo,
    Integrity,
    Internal,
};

class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}

    ErrorKind kind() const noexcept { return kind_; }

private:
    ErrorKind kind_;
};

inline bool isRetriable(ErrorKind kind) {
    return kind == ErrorKind::Network || kind == ErrorKind::Timeout ||
           kind == ErrorKind::Busy;
}

}  // namespace landrop
