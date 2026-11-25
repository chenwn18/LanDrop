#include "socket.h"

#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace landrop {
namespace {

constexpr int kSelectSliceMs = 200;
constexpr int kSocketBufferBytes = 1024 * 1024;

bool isWouldBlock(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS ||
           code == WSAEALREADY;
#else
    return code == EWOULDBLOCK || code == EINPROGRESS || code == EALREADY;
#endif
}

bool isInterrupted(int code) {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string socketErrorText(int code) {
    if (code == 0) {
        return "未知网络错误";
    }
#ifdef _WIN32
    // FormatMessageA / std::system_category() return text in the active
    // Windows code page. The rest of the application and the wire protocol
    // are UTF-8, so format as wide text and convert explicitly.
    return "Winsock 错误 " + std::to_string(code) + ": " +
           platform::win32ErrorMessage(static_cast<unsigned long>(code));
#else
    char buffer[256]{};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char* text = strerror_r(code, buffer, sizeof(buffer));
    return text != nullptr ? std::string(text) : std::string("网络错误");
#else
    if (strerror_r(code, buffer, sizeof(buffer)) == 0) {
        return buffer;
    }
    return "网络错误 " + std::to_string(code);
#endif
#endif
}

std::string formatClosedError() { return "连接已被对端关闭"; }

}  // namespace

Socket::Socket() : socket_(kInvalidSocket), localPort_(0) {
}

Socket::Socket(Native native)
    : socket_(native), localPort_(0) {
    if (native != kInvalidSocket) {
        sockaddr_in address{};
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(native, reinterpret_cast<sockaddr*>(&address),
                        &length) == 0) {
            localPort_ = ntohs(address.sin_port);
        }
    }
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : socket_(other.socket_),
      localPort_(other.localPort_),
      remoteAddress_(std::move(other.remoteAddress_)) {
    other.socket_ = kInvalidSocket;
    other.localPort_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = other.socket_;
        localPort_ = other.localPort_;
        remoteAddress_ = std::move(other.remoteAddress_);
        other.socket_ = kInvalidSocket;
        other.localPort_ = 0;
    }
    return *this;
}

bool Socket::valid() const { return socket_ != kInvalidSocket; }

uint16_t Socket::localPort() const { return localPort_; }

void Socket::setNonBlocking(Native native) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(native, FIONBIO, &mode);
#else
    const int flags = fcntl(native, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(native, F_SETFL, flags | O_NONBLOCK);
    }
    const int descriptorFlags = fcntl(native, F_GETFD, 0);
    if (descriptorFlags >= 0) {
        fcntl(native, F_SETFD, descriptorFlags | FD_CLOEXEC);
    }
#endif
}

void Socket::closeNative(Native native) {
    if (native == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(native);
#else
    ::close(native);
#endif
}

void Socket::close() {
    if (socket_ != kInvalidSocket) {
        closeNative(socket_);
        socket_ = kInvalidSocket;
        localPort_ = 0;
    }
}

void Socket::configureConnectedSocket() {
    int enabled = 1;
#ifdef _WIN32
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    const int bufferSize = kSocketBufferBytes;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
#else
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    int bufferSize = kSocketBufferBytes;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
#endif
}

Socket::WaitStatus Socket::waitSocket(
    int timeoutMs, bool forWrite, const std::atomic<bool>* abort,
    std::string& error) const {
    if (socket_ == kInvalidSocket) {
        error = "套接字未连接";
        return WaitStatus::Error;
    }
    if (abort != nullptr && abort->load()) {
        error = "操作已取消";
        return WaitStatus::Aborted;
    }

    // timeoutMs == 0 requests a single non-blocking poll, not an immediate
    // timeout. The sender uses this while streaming to notice an Error frame
    // from the receiver.
    const bool immediate = timeoutMs <= 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeoutMs));
    for (;;) {
        if (abort != nullptr && abort->load()) {
            error = "操作已取消";
            return WaitStatus::Aborted;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!immediate && now >= deadline) {
            error = "网络操作超时";
            return WaitStatus::Timeout;
        }
        int slice = 0;
        if (!immediate) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now)
                    .count();
            slice = static_cast<int>(std::min<int64_t>(
                kSelectSliceMs, std::max<int64_t>(1, remaining)));
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval timeout{};
        timeout.tv_sec = slice / 1000;
        timeout.tv_usec = (slice % 1000) * 1000;
#ifdef _WIN32
        const int result = select(0, forWrite ? nullptr : &set,
                                  forWrite ? &set : nullptr, nullptr, &timeout);
#else
        const int result = select(socket_ + 1, forWrite ? nullptr : &set,
                                  forWrite ? &set : nullptr, nullptr, &timeout);
#endif
        if (result > 0) {
            return WaitStatus::Ready;
        }
        if (result == 0) {
            if (immediate) {
                error = "网络操作超时";
                return WaitStatus::Timeout;
            }
            continue;
        }
        const int code = lastSocketError();
        if (isInterrupted(code)) {
            continue;
        }
        error = "等待网络事件失败: " + socketErrorText(code);
        return WaitStatus::Error;
    }
}

bool Socket::waitReadable(int timeoutMs, const std::atomic<bool>* abort,
                          bool& aborted, std::string& error) const {
    aborted = false;
    const WaitStatus status =
        waitSocket(timeoutMs, false, abort, error);
    if (status == WaitStatus::Ready) {
        return true;
    }
    if (status == WaitStatus::Aborted) {
        aborted = true;
    }
    return false;
}

bool Socket::connectTo(const std::string& host, uint16_t port, int timeoutMs,
                       const std::atomic<bool>* abort, std::string& error) {
    close();
    if (abort != nullptr && abort->load()) {
        error = "操作已取消";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::string service = std::to_string(port);
    addrinfo* addresses = nullptr;
    const int resolve = getaddrinfo(host.c_str(), service.c_str(), &hints,
                                    &addresses);
    if (resolve != 0) {
#ifdef _WIN32
        error = "无法解析地址 " + host + "，错误码 " + std::to_string(resolve);
#else
        error = "无法解析地址 " + host + ": " + gai_strerror(resolve);
#endif
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeoutMs));
    std::string lastError = "无法连接到目标设备";
    for (addrinfo* candidate = addresses; candidate != nullptr;
         candidate = candidate->ai_next) {
        if (abort != nullptr && abort->load()) {
            lastError = "操作已取消";
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            lastError = "连接超时";
            break;
        }
        Native native = ::socket(candidate->ai_family, candidate->ai_socktype,
                                 candidate->ai_protocol);
        if (native == kInvalidSocket) {
            lastError = "创建套接字失败: " +
                        socketErrorText(lastSocketError());
            continue;
        }
        setNonBlocking(native);
        int result = ::connect(native, candidate->ai_addr,
                               static_cast<decltype(sizeof(sockaddr))>(
                                   candidate->ai_addrlen));
        if (result != 0) {
            const int code = lastSocketError();
            if (!isWouldBlock(code)) {
                lastError = "连接失败: " + socketErrorText(code);
                closeNative(native);
                continue;
            }
            socket_ = native;
            const int remaining = static_cast<int>(
                std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count()));
            std::string waitError;
            const WaitStatus status =
                waitSocket(remaining, true, abort, waitError);
            if (status != WaitStatus::Ready) {
                lastError = waitError.empty() ? "连接超时" : waitError;
                closeNative(native);
                socket_ = kInvalidSocket;
                if (status == WaitStatus::Aborted) {
                    break;
                }
                continue;
            }
            int soError = 0;
#ifdef _WIN32
            int length = sizeof(soError);
#else
            socklen_t length = sizeof(soError);
#endif
            if (getsockopt(native, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&soError), &length) != 0 ||
                soError != 0) {
                if (soError == 0) {
                    soError = lastSocketError();
                }
                lastError = "连接失败: " + socketErrorText(soError);
                closeNative(native);
                socket_ = kInvalidSocket;
                continue;
            }
        }
        socket_ = native;
        configureConnectedSocket();
        sockaddr_in localAddress{};
#ifdef _WIN32
        int addressLength = sizeof(localAddress);
#else
        socklen_t addressLength = sizeof(localAddress);
#endif
        if (getsockname(native, reinterpret_cast<sockaddr*>(&localAddress),
                        &addressLength) == 0) {
            localPort_ = ntohs(localAddress.sin_port);
        }
        freeaddrinfo(addresses);
        return true;
    }
    freeaddrinfo(addresses);
    error = lastError;
    return false;
}

bool Socket::sendAll(const void* data, std::size_t size, int timeoutMs,
                     const std::atomic<bool>* abort, std::string& error) {
    if (!valid()) {
        error = "套接字未连接";
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeoutMs));
    while (sent < size) {
        if (abort != nullptr && abort->load()) {
            error = "操作已取消";
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "发送数据超时";
            return false;
        }
        const std::size_t request =
            std::min<std::size_t>(size - sent, INT_MAX);
#ifdef _WIN32
        const int result = ::send(
            socket_, reinterpret_cast<const char*>(bytes + sent),
            static_cast<int>(request), 0);
#else
        const int result = static_cast<int>(
            ::send(socket_, bytes + sent, request, MSG_NOSIGNAL));
#endif
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            error = "网络发送返回 0 字节";
            return false;
        }
        const int code = lastSocketError();
        if (isInterrupted(code)) {
            continue;
        }
        if (isWouldBlock(code)) {
            std::string waitError;
            const int remaining = static_cast<int>(
                std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count()));
            const WaitStatus status =
                waitSocket(remaining, true, abort, waitError);
            if (status == WaitStatus::Ready) {
                continue;
            }
            error = waitError.empty() ? "网络发送超时" : waitError;
            return false;
        }
        error = "发送数据失败: " + socketErrorText(code);
        return false;
    }
    return true;
}

bool Socket::recvAll(void* data, std::size_t size, int timeoutMs,
                     const std::atomic<bool>* abort, std::string& error) {
    if (!valid()) {
        error = "套接字未连接";
        return false;
    }
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeoutMs));
    while (received < size) {
        if (abort != nullptr && abort->load()) {
            error = "操作已取消";
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "接收数据超时";
            return false;
        }
        const std::size_t request =
            std::min<std::size_t>(size - received, INT_MAX);
#ifdef _WIN32
        const int result = ::recv(
            socket_, reinterpret_cast<char*>(bytes + received),
            static_cast<int>(request), 0);
#else
        const int result = static_cast<int>(
            ::recv(socket_, bytes + received, request, 0));
#endif
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            error = formatClosedError();
            return false;
        }
        const int code = lastSocketError();
        if (isInterrupted(code)) {
            continue;
        }
        if (isWouldBlock(code)) {
            std::string waitError;
            const int remaining = static_cast<int>(
                std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count()));
            const WaitStatus status =
                waitSocket(remaining, false, abort, waitError);
            if (status == WaitStatus::Ready) {
                continue;
            }
            error = waitError.empty() ? "网络接收超时" : waitError;
            return false;
        }
        error = "接收数据失败: " + socketErrorText(code);
        return false;
    }
    return true;
}

std::unique_ptr<Socket> Socket::createListener(uint16_t port,
                                               uint16_t& boundPort,
                                               std::string& error) {
    boundPort = 0;
    Native native = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (native == kInvalidSocket) {
        error = "创建监听套接字失败: " + socketErrorText(lastSocketError());
        return nullptr;
    }
    int enabled = 1;
#ifdef _WIN32
    // Windows SO_REUSEADDR permits two processes to listen on the same port.
    // That would silently split incoming connections between instances, so
    // use the exclusive option and let the second instance run send-only.
    setsockopt(native, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
    setsockopt(native, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(native, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        error = "无法监听端口 " + std::to_string(port) + ": " +
                socketErrorText(lastSocketError());
        closeNative(native);
        return nullptr;
    }
    if (listen(native, 32) != 0) {
        error = "监听端口失败: " + socketErrorText(lastSocketError());
        closeNative(native);
        return nullptr;
    }
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(native, reinterpret_cast<sockaddr*>(&address), &length) ==
        0) {
        boundPort = ntohs(address.sin_port);
    }
    setNonBlocking(native);
    auto listener = std::make_unique<Socket>(native);
    listener->localPort_ = boundPort;
    return listener;
}

std::unique_ptr<Socket> Socket::acceptConnection(
    int timeoutMs, const std::atomic<bool>* abort, bool& timedOut,
    bool& aborted, std::string& error) const {
    timedOut = false;
    aborted = false;
    if (socket_ == kInvalidSocket) {
        error = "监听套接字无效";
        return nullptr;
    }
    std::string waitError;
    const WaitStatus status = waitSocket(timeoutMs, false, abort, waitError);
    if (status == WaitStatus::Timeout) {
        timedOut = true;
        return nullptr;
    }
    if (status == WaitStatus::Aborted) {
        aborted = true;
        error = waitError;
        return nullptr;
    }
    if (status == WaitStatus::Error) {
        error = waitError;
        return nullptr;
    }

    sockaddr_in peer{};
#ifdef _WIN32
    int length = sizeof(peer);
#else
    socklen_t length = sizeof(peer);
#endif
    Native native = accept(socket_, reinterpret_cast<sockaddr*>(&peer), &length);
    if (native == kInvalidSocket) {
        const int code = lastSocketError();
        if (isWouldBlock(code)) {
            timedOut = true;
            return nullptr;
        }
        error = "接受连接失败: " + socketErrorText(code);
        return nullptr;
    }
    setNonBlocking(native);
    auto connection = std::make_unique<Socket>(native);
    connection->configureConnectedSocket();
    char addressText[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &peer.sin_addr, addressText,
                  sizeof(addressText)) != nullptr) {
        connection->remoteAddress_ = addressText;
    }
    return connection;
}

}  // namespace landrop
