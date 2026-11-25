#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace landrop {

class Socket {
public:
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    bool connectTo(const std::string& host, uint16_t port, int timeoutMs,
                   const std::atomic<bool>* abort, std::string& error);
    bool sendAll(const void* data, std::size_t size, int timeoutMs,
                 const std::atomic<bool>* abort, std::string& error);
    bool recvAll(void* data, std::size_t size, int timeoutMs,
                 const std::atomic<bool>* abort, std::string& error);
    bool waitReadable(int timeoutMs, const std::atomic<bool>* abort,
                      bool& aborted, std::string& error) const;
    void close();
    bool valid() const;
    uint16_t localPort() const;
    const std::string& remoteAddress() const { return remoteAddress_; }

    static std::unique_ptr<Socket> createListener(uint16_t port,
                                                  uint16_t& boundPort,
                                                  std::string& error);
    std::unique_ptr<Socket> acceptConnection(int timeoutMs,
                                             const std::atomic<bool>* abort,
                                             bool& timedOut,
                                             bool& aborted,
                                             std::string& error) const;

#ifdef _WIN32
    using Native = SOCKET;
    static constexpr Native kInvalidSocket = INVALID_SOCKET;
    explicit Socket(Native native);
#else
    using Native = int;
    static constexpr Native kInvalidSocket = -1;
    explicit Socket(Native native);
#endif

    Native native() const { return socket_; }

private:
    enum class WaitStatus { Ready, Timeout, Aborted, Error };

    WaitStatus waitSocket(int timeoutMs, bool forWrite,
                          const std::atomic<bool>* abort,
                          std::string& error) const;
    void configureConnectedSocket();
    static void setNonBlocking(Native socket);
    static void closeNative(Native socket);

    Native socket_;
    uint16_t localPort_;
    std::string remoteAddress_;
};

}  // namespace landrop
