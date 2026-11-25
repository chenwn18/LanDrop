#include "discovery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "common.h"
#include "platform.h"
#include "state.h"
#include "util.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace landrop {
namespace {

constexpr uint8_t kDiscoveryMagic[4] = {'L', 'D', 'S', 'C'};
constexpr uint16_t kDiscoveryVersion = 1;
constexpr auto kBeaconInterval = std::chrono::milliseconds(2500);
constexpr std::size_t kMaxPeerNameBytes = 120;

#ifdef _WIN32
using UdpSocket = SOCKET;
constexpr UdpSocket kInvalidUdp = INVALID_SOCKET;
#else
using UdpSocket = int;
constexpr UdpSocket kInvalidUdp = -1;
#endif

int udpError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool udpWouldBlock(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK;
#else
    return code == EWOULDBLOCK || code == EAGAIN;
#endif
}

std::string udpErrorText(int code) {
#ifdef _WIN32
    return "Winsock 错误 " + std::to_string(code);
#else
    return std::strerror(code);
#endif
}

void closeUdp(UdpSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

std::vector<uint8_t> beaconsBytes(const std::string& instanceId,
                                  const std::string& deviceName,
                                  uint16_t tcpPort) {
    std::vector<uint8_t> output;
    output.insert(output.end(), std::begin(kDiscoveryMagic),
                  std::end(kDiscoveryMagic));
    output.push_back(static_cast<uint8_t>(kDiscoveryVersion >> 8));
    output.push_back(static_cast<uint8_t>(kDiscoveryVersion));
    output.push_back(static_cast<uint8_t>(tcpPort >> 8));
    output.push_back(static_cast<uint8_t>(tcpPort));

    std::array<uint8_t, 16> idBytes{};
    for (std::size_t i = 0; i < 16 && i * 2 + 1 < instanceId.size(); ++i) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        idBytes[i] = static_cast<uint8_t>(
            (nibble(instanceId[i * 2]) << 4) | nibble(instanceId[i * 2 + 1]));
    }
    output.insert(output.end(), idBytes.begin(), idBytes.end());
    const std::string name = truncateUtf8(deviceName, kMaxPeerNameBytes);
    const uint16_t nameLength = static_cast<uint16_t>(name.size());
    output.push_back(static_cast<uint8_t>(nameLength >> 8));
    output.push_back(static_cast<uint8_t>(nameLength));
    output.insert(output.end(), name.begin(), name.end());
    return output;
}

struct Beacon {
    uint16_t tcpPort = 0;
    std::string id;
    std::string name;
};

bool parseBeacon(const uint8_t* data, std::size_t size, Beacon& beacon) {
    if (size < 4 + 2 + 2 + 16 + 2) {
        return false;
    }
    if (std::memcmp(data, kDiscoveryMagic, 4) != 0) {
        return false;
    }
    const uint16_t version =
        static_cast<uint16_t>((data[4] << 8) | data[5]);
    if (version != kDiscoveryVersion) {
        return false;
    }
    beacon.tcpPort = static_cast<uint16_t>((data[6] << 8) | data[7]);
    if (beacon.tcpPort == 0) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    beacon.id.clear();
    beacon.id.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const uint8_t value = data[8 + static_cast<std::size_t>(i)];
        beacon.id.push_back(kHex[value >> 4]);
        beacon.id.push_back(kHex[value & 0x0f]);
    }
    const uint16_t nameLength =
        static_cast<uint16_t>((data[24] << 8) | data[25]);
    if (nameLength > kMaxPeerNameBytes ||
        size != 26 + static_cast<std::size_t>(nameLength)) {
        return false;
    }
    beacon.name.assign(reinterpret_cast<const char*>(data + 26), nameLength);
    if (!isValidUtf8(beacon.name)) {
        return false;
    }
    return true;
}

}  // namespace

class DiscoveryService::Impl {
public:
    Impl(CoreState& state, std::string instanceId, std::string deviceName,
         uint16_t tcpPort, uint16_t discoveryPort)
        : state_(state),
          instanceId_(std::move(instanceId)),
          deviceName_(std::move(deviceName)),
          tcpPort_(tcpPort),
          discoveryPort_(discoveryPort) {}

    ~Impl() { stop(); }

    bool start(std::string& error) {
        if (running_) {
            return true;
        }
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidUdp) {
            error = "无法创建设备发现套接字: " +
                    udpErrorText(udpError());
            return false;
        }
        int enabled = 1;
#ifdef _WIN32
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        setsockopt(socket_, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        u_long mode = 1;
        ioctlsocket(socket_, FIONBIO, &mode);
#else
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
        const int fdFlags = fcntl(socket_, F_GETFD, 0);
        if (fdFlags >= 0) {
            fcntl(socket_, F_SETFD, fdFlags | FD_CLOEXEC);
        }
        const int statusFlags = fcntl(socket_, F_GETFL, 0);
        if (statusFlags >= 0) {
            fcntl(socket_, F_SETFL, statusFlags | O_NONBLOCK);
        }
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(discoveryPort_);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != 0) {
            error = "无法监听发现端口 " + std::to_string(discoveryPort_) + ": " +
                    udpErrorText(udpError());
            closeUdp(socket_);
            socket_ = kInvalidUdp;
            return false;
        }
        stop_.store(false);
        try {
            thread_ = std::thread([this]() {
                try {
                    run();
                } catch (...) {
                    // Discovery is optional. Never let a failed allocation or
                    // malformed packet terminate the whole application.
                }
            });
        } catch (const std::exception& exception) {
            error = std::string("无法启动设备发现线程: ") + exception.what();
            closeUdp(socket_);
            socket_ = kInvalidUdp;
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() {
        if (!running_) {
            return;
        }
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (socket_ != kInvalidUdp) {
            closeUdp(socket_);
            socket_ = kInvalidUdp;
        }
        running_ = false;
    }

    bool running() const { return running_; }

private:
    void run() {
        const auto payload =
            beaconsBytes(instanceId_, deviceName_, tcpPort_);
        auto lastBeacon = std::chrono::steady_clock::time_point{};
        auto lastBroadcastRefresh = std::chrono::steady_clock::time_point{};
        std::vector<std::string> broadcastTargets;
        uint8_t buffer[512];

        while (!stop_.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (lastBeacon.time_since_epoch().count() == 0 ||
                now - lastBeacon >= kBeaconInterval) {
                lastBeacon = now;
                if (broadcastTargets.empty() ||
                    now - lastBroadcastRefresh > std::chrono::seconds(30)) {
                    broadcastTargets = platform::broadcastAddresses();
                    lastBroadcastRefresh = now;
                }
                for (const auto& target : broadcastTargets) {
                    sockaddr_in destination{};
                    destination.sin_family = AF_INET;
                    destination.sin_port = htons(discoveryPort_);
                    if (inet_pton(AF_INET, target.c_str(),
                                  &destination.sin_addr) != 1) {
                        continue;
                    }
#ifdef _WIN32
                    sendto(socket_,
                           reinterpret_cast<const char*>(payload.data()),
                           static_cast<int>(payload.size()), 0,
                           reinterpret_cast<sockaddr*>(&destination),
                           sizeof(destination));
#else
                    sendto(socket_, payload.data(), payload.size(), 0,
                           reinterpret_cast<sockaddr*>(&destination),
                           sizeof(destination));
#endif
                }
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket_, &readSet);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 300000;
#ifdef _WIN32
            const int selected =
                select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int selected =
                select(socket_ + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (selected <= 0) {
                continue;
            }
            // Bound one burst so a UDP flood cannot prevent stop() from
            // joining this thread or monopolize the discovery worker.
            for (int drained = 0; drained < 64 && !stop_.load(); ++drained) {
                sockaddr_in source{};
#ifdef _WIN32
                int sourceLength = sizeof(source);
#else
                socklen_t sourceLength = sizeof(source);
#endif
                const int received = static_cast<int>(recvfrom(
                    socket_, reinterpret_cast<char*>(buffer), sizeof(buffer),
                    0, reinterpret_cast<sockaddr*>(&source), &sourceLength));
                if (received < 0) {
                    const int code = udpError();
                    if (udpWouldBlock(code) || stop_.load()) {
                        break;
                    }
                    break;
                }
                Beacon beacon;
                if (!parseBeacon(buffer, static_cast<std::size_t>(received),
                                 beacon)) {
                    continue;
                }
                if (beacon.id == instanceId_) {
                    continue;
                }
                char sourceIp[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &source.sin_addr, sourceIp,
                              sizeof(sourceIp)) == nullptr) {
                    continue;
                }
                state_.peerSeen(beacon.id, beacon.name, sourceIp,
                                beacon.tcpPort);
            }
        }
    }

    CoreState& state_;
    std::string instanceId_;
    std::string deviceName_;
    uint16_t tcpPort_;
    uint16_t discoveryPort_;
    UdpSocket socket_ = kInvalidUdp;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool running_ = false;
};

DiscoveryService::DiscoveryService(CoreState& state, std::string instanceId,
                                   std::string deviceName, uint16_t tcpPort,
                                   uint16_t discoveryPort)
    : impl_(std::make_unique<Impl>(state, std::move(instanceId),
                                   std::move(deviceName), tcpPort,
                                   discoveryPort)) {}

DiscoveryService::~DiscoveryService() = default;

bool DiscoveryService::start(std::string& error) {
    return impl_->start(error);
}

void DiscoveryService::stop() { impl_->stop(); }

bool DiscoveryService::running() const { return impl_->running(); }

}  // namespace landrop
