#include "core/platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include "core/util.h"

namespace landrop {
namespace platform {

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        // UI text may originate from an older peer or a localized OS error.
        // Recover with replacement characters instead of dropping the whole
        // message from the interface.
        length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string win32ErrorMessage(unsigned long code) {
    auto format = [&](DWORD flags, HMODULE module) -> std::string {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                flags,
            module, static_cast<DWORD>(code),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::string text;
        if (length != 0 && buffer != nullptr) {
            text = wideToUtf8(std::wstring(buffer, length));
            while (!text.empty() &&
                   (text.back() == '\r' || text.back() == '\n' ||
                    text.back() == ' ' || text.back() == '\t')) {
                text.pop_back();
            }
        }
        if (buffer != nullptr) {
            LocalFree(buffer);
        }
        return text;
    };

    std::string message = format(FORMAT_MESSAGE_FROM_SYSTEM, nullptr);
    if (message.empty()) {
        HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
        if (ws2 != nullptr) {
            message = format(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM,
                             ws2);
        }
    }
    if (message.empty()) {
        message = "Windows 错误 " + std::to_string(code);
    }
    return message;
}

std::string caseFoldForComparison(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty()) {
        return landrop::toLowerAscii(utf8);
    }
    const int needed = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) {
        return landrop::toLowerAscii(utf8);
    }
    std::wstring folded(static_cast<std::size_t>(needed), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
        static_cast<int>(wide.size()), folded.data(), needed, nullptr, nullptr,
        0);
    if (written <= 0) {
        return landrop::toLowerAscii(utf8);
    }
    folded.resize(static_cast<std::size_t>(written));
    const std::string result = wideToUtf8(folded);
    return result.empty() ? landrop::toLowerAscii(utf8) : result;
}

bool initializeNetwork(std::string& error) {
    static bool initialized = false;
    if (initialized) {
        return true;
    }
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        error = "Winsock 初始化失败，错误码 " + std::to_string(result);
        return false;
    }
    initialized = true;
    return true;
}

void shutdownNetwork() { WSACleanup(); }

fs::path configDirectory() {
    wchar_t* buffer = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE,
                                       nullptr, &buffer))) {
        result = fs::path(buffer) / L"LanDrop";
        CoTaskMemFree(buffer);
    } else {
        const std::wstring fallback = [] {
            wchar_t value[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"APPDATA", value,
                                                     MAX_PATH);
            return n > 0 && n < MAX_PATH ? std::wstring(value, n) : std::wstring();
        }();
        result = fs::path(fallback.empty() ? L"." : fallback) / L"LanDrop";
    }
    std::error_code ec;
    fs::create_directories(result, ec);
    return result;
}

fs::path defaultReceiveDirectory() {
    wchar_t* buffer = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE,
                                       nullptr, &buffer))) {
        base = fs::path(buffer);
        CoTaskMemFree(buffer);
    } else {
        wchar_t profile[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile,
                                                 MAX_PATH);
        base = fs::path(n > 0 && n < MAX_PATH ? std::wstring(profile, n)
                                              : std::wstring(L"."));
        base /= L"Downloads";
    }
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / L"LanDrop";
}

bool isReparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.native().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string errorCodeMessage(const std::error_code& error) {
    if (error.value() == 0) {
        return {};
    }
    if (error.category() == std::system_category()) {
        return win32ErrorMessage(
            static_cast<unsigned long>(error.value()));
    }
    return error.message();
}

std::string hostName() {
    wchar_t buffer[256]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &size)) {
        return "Windows PC";
    }
    return wideToUtf8(std::wstring(buffer, size));
}

std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> storage;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3; ++attempt) {
        storage.resize(size);
        auto* addresses =
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const ULONG rc = GetAdaptersAddresses(
            AF_INET, flags, nullptr, addresses, &size);
        if (rc == NO_ERROR) {
            for (auto* adapter = addresses; adapter != nullptr;
                 adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) {
                    continue;
                }
                for (auto* unicast = adapter->FirstUnicastAddress;
                     unicast != nullptr; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr == nullptr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET) {
                        continue;
                    }
                    char text[INET_ADDRSTRLEN]{};
                    const auto* sin = reinterpret_cast<sockaddr_in*>(
                        unicast->Address.lpSockaddr);
                    if (InetNtopA(AF_INET, &sin->sin_addr, text,
                                  sizeof(text)) == nullptr) {
                        continue;
                    }
                    const std::string ip(text);
                    if (ip == "127.0.0.1" || ip == "0.0.0.0") {
                        continue;
                    }
                    if (std::find(result.begin(), result.end(), ip) ==
                        result.end()) {
                        result.push_back(ip);
                    }
                }
            }
            break;
        }
        if (rc != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }
    return result;
}

std::vector<std::string> broadcastAddresses() {
    std::vector<std::string> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> storage;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3; ++attempt) {
        storage.resize(size);
        auto* addresses =
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const ULONG rc =
            GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
        if (rc == NO_ERROR) {
            for (auto* adapter = addresses; adapter != nullptr;
                 adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) {
                    continue;
                }
                for (auto* unicast = adapter->FirstUnicastAddress;
                     unicast != nullptr; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr == nullptr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET) {
                        continue;
                    }
                    const auto* sin = reinterpret_cast<sockaddr_in*>(
                        unicast->Address.lpSockaddr);
                    const uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    if (ip == 0 || (ip >> 24) == 127) {
                        continue;
                    }
                    const int prefix = unicast->OnLinkPrefixLength;
                    if (prefix <= 0 || prefix >= 32) {
                        continue;
                    }
                    const uint32_t mask =
                        prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));
                    const uint32_t broadcast = ip | ~mask;
                    in_addr address{};
                    address.s_addr = htonl(broadcast);
                    char text[INET_ADDRSTRLEN]{};
                    if (InetNtopA(AF_INET, &address, text, sizeof(text)) !=
                        nullptr) {
                        const std::string value(text);
                        if (std::find(result.begin(), result.end(), value) ==
                            result.end()) {
                            result.push_back(value);
                        }
                    }
                }
            }
            break;
        }
        if (rc != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }
    if (std::find(result.begin(), result.end(), "255.255.255.255") ==
        result.end()) {
        result.push_back("255.255.255.255");
    }
    return result;
}

bool openInFileManager(const fs::path& path, std::string& error) {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open",
                                          path.native().c_str(), nullptr,
                                          nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32) {
        error = "无法打开文件夹（ShellExecute 错误 " +
                std::to_string(reinterpret_cast<std::intptr_t>(result)) + "）";
        return false;
    }
    return true;
}

void sleepMilliseconds(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace platform
}  // namespace landrop
