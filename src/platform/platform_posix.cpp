#include "core/platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/util.h"

namespace landrop {
namespace platform {
namespace {

fs::path homeDirectory() {
    const char* home = std::getenv("HOME");
    return home != nullptr && *home != '\0' ? fs::path(home)
                                            : fs::path(".");
}

fs::path configuredDownloadDirectory() {
    fs::path config;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        config = fs::path(xdg) / "user-dirs.dirs";
    } else {
        config = homeDirectory() / ".config" / "user-dirs.dirs";
    }
    std::ifstream input(config);
    if (!input) {
        return {};
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::string prefix = "XDG_DOWNLOAD_DIR=";
        if (line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        std::string value = trim(line.substr(prefix.size()));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        const std::string homeToken = "$HOME";
        if (value.compare(0, homeToken.size(), homeToken) == 0) {
            value = pathToUtf8(homeDirectory()) + value.substr(homeToken.size());
        }
        if (!value.empty()) {
            return pathFromUtf8(value);
        }
    }
    return {};
}

std::string ipv4Text(const sockaddr* address) {
    if (address == nullptr || address->sa_family != AF_INET) {
        return {};
    }
    char text[INET_ADDRSTRLEN]{};
    const auto* sin = reinterpret_cast<const sockaddr_in*>(address);
    if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) == nullptr) {
        return {};
    }
    return text;
}

}  // namespace

bool initializeNetwork(std::string&) { return true; }

void shutdownNetwork() {}

fs::path configDirectory() {
    fs::path result;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        result = fs::path(xdg) / "landrop";
    } else {
        result = homeDirectory() / ".config" / "landrop";
    }
    std::error_code ec;
    fs::create_directories(result, ec);
    return result;
}

fs::path defaultReceiveDirectory() {
    fs::path downloads = configuredDownloadDirectory();
    if (downloads.empty()) {
        downloads = homeDirectory() / "Downloads";
        std::error_code ec;
        if (!fs::is_directory(downloads, ec)) {
            downloads = homeDirectory();
        }
    }
    return downloads / "LanDrop";
}

std::string hostName() {
    char buffer[256]{};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "Linux PC";
    }
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
}

bool isReparsePoint(const fs::path&) { return false; }

std::string errorCodeMessage(const std::error_code& error) {
    return error.message();
}

std::string caseFoldForComparison(const std::string& utf8) { return utf8; }

std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> result;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return result;
    }
    for (ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((item->ifa_flags & IFF_UP) == 0 ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const std::string ip = ipv4Text(item->ifa_addr);
        if (!ip.empty() &&
            std::find(result.begin(), result.end(), ip) == result.end()) {
            result.push_back(ip);
        }
    }
    freeifaddrs(interfaces);
    return result;
}

std::vector<std::string> broadcastAddresses() {
    std::vector<std::string> result;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        result.push_back("255.255.255.255");
        return result;
    }
    for (ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_netmask == nullptr ||
            item->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((item->ifa_flags & IFF_UP) == 0 ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        const auto* mask =
            reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
        in_addr broadcast{};
        broadcast.s_addr = address->sin_addr.s_addr | ~mask->sin_addr.s_addr;
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &broadcast, text, sizeof(text)) != nullptr) {
            const std::string value(text);
            if (value != "255.255.255.255" &&
                std::find(result.begin(), result.end(), value) ==
                    result.end()) {
                result.push_back(value);
            }
        }
    }
    freeifaddrs(interfaces);
    result.push_back("255.255.255.255");
    return result;
}

bool openInFileManager(const fs::path& path, std::string& error) {
    const pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork 失败: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // Double-fork so an xdg-open wrapper that waits for the launched file
        // manager cannot block the GUI thread. The middle child exits at once.
        const pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild > 0) {
            _exit(0);
        }
        setsid();
        execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            error = std::string("等待启动器失败: ") + std::strerror(errno);
            return false;
        }
    }
    return true;
}

void sleepMilliseconds(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace platform
}  // namespace landrop
