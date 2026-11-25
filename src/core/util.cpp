#include "util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#endif

#include "common.h"
#include "platform.h"

namespace landrop {
namespace {

bool isWindowsReservedName(const std::string& component) {
    std::string stem = component;
    const auto dot = stem.find('.');
    if (dot != std::string::npos) {
        stem.resize(dot);
    }
    stem = toLowerAscii(stem);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
        return true;
    }
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 ||
                             stem.compare(0, 3, "lpt") == 0)) {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    return false;
}

std::string formatWithOneDecimal(long double value) {
    std::ostringstream stream;
    if (value >= 100.0L) {
        stream.setf(std::ios::fixed);
        stream.precision(0);
    } else if (value >= 10.0L) {
        stream.setf(std::ios::fixed);
        stream.precision(1);
    } else {
        stream.setf(std::ios::fixed);
        stream.precision(2);
    }
    stream << value;
    std::string text = stream.str();
    while (text.size() > 2 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::string trim(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

bool isValidUtf8(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        uint32_t codepoint = 0;
        if ((c & 0xe0) == 0xc0) {
            extra = 1;
            codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            codepoint = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= value.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            const unsigned char cc = static_cast<unsigned char>(value[i + j]);
            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (cc & 0x3f);
        }
        if ((extra == 1 && codepoint < 0x80) ||
            (extra == 2 && codepoint < 0x800) ||
            (extra == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string truncateUtf8(std::string value, std::size_t maxBytes) {
    if (value.size() <= maxBytes) {
        return value;
    }
    value.resize(maxBytes);
    while (!value.empty() && !isValidUtf8(value)) {
        value.pop_back();
    }
    return value;
}

std::string formatBytes(uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    long double value = static_cast<long double>(bytes);
    int unit = 0;
    while (value >= 1024.0L && unit < 5) {
        value /= 1024.0L;
        ++unit;
    }
    if (unit == 0) {
        return std::to_string(bytes) + " B";
    }
    return formatWithOneDecimal(value) + " " + kUnits[unit];
}

std::string formatRate(double bytesPerSecond) {
    if (!std::isfinite(bytesPerSecond) || bytesPerSecond <= 0.0) {
        return "0 B/s";
    }
    return formatBytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "--";
    }
    const uint64_t total = static_cast<uint64_t>(seconds + 0.5);
    if (total < 60) {
        return std::to_string(total) + "s";
    }
    const uint64_t minutes = total / 60;
    const uint64_t secs = total % 60;
    if (minutes < 60) {
        return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
    }
    const uint64_t hours = minutes / 60;
    return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
}

std::string formatClockTime(int64_t unixSeconds) {
    const std::time_t value = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", local.tm_hour,
                  local.tm_min, local.tm_sec);
    return buffer;
}

std::string randomHex(std::size_t byteCount) {
    std::random_device rd;
    std::mt19937_64 generator((static_cast<uint64_t>(rd()) << 32) ^
                              static_cast<uint64_t>(rd()) ^
                              static_cast<uint64_t>(std::time(nullptr)));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(byteCount * 2);
    for (std::size_t i = 0; i < byteCount; ++i) {
        const uint8_t byte = static_cast<uint8_t>(generator() & 0xffu);
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

fs::path pathFromUtf8(const std::string& utf8) {
#if defined(__cpp_lib_char8_t)
    return fs::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
#else
    return fs::u8path(utf8);
#endif
}

std::string pathToUtf8(const fs::path& path) {
    return genericPathToUtf8(path);
}

std::string genericPathToUtf8(const fs::path& path) {
    return path.generic_u8string();
}

fs::path expandUserPath(const fs::path& path) {
#ifdef _WIN32
    // Windows has no shell tilde convention; keep the path exactly as typed.
    return path;
#else
    const std::string text = pathToUtf8(path);
    if (text.empty() || text.front() != '~') {
        return path;
    }
    const std::size_t slash = text.find('/');
    const std::string account =
        slash == std::string::npos ? text.substr(1)
                                   : text.substr(1, slash - 1);
    const std::string remainder =
        slash == std::string::npos ? std::string() : text.substr(slash);
    if (account.empty()) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') {
            return path;
        }
        return pathFromUtf8(std::string(home) + remainder);
    }
    std::vector<char> buffer(4096);
    for (;;) {
        passwd entry{};
        passwd* result = nullptr;
        const int status = getpwnam_r(account.c_str(), &entry, buffer.data(),
                                      buffer.size(), &result);
        if (status == ERANGE) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (status != 0 || result == nullptr || result->pw_dir == nullptr ||
            *result->pw_dir == '\0') {
            return path;
        }
        return pathFromUtf8(std::string(result->pw_dir) + remainder);
    }
#endif
}

bool validateRelativePath(const std::string& relativePath,
                          std::string& errorMessage) {
    if (relativePath.empty()) {
        errorMessage = "路径为空";
        return false;
    }
    if (relativePath.size() > kMaxRelativePathBytes) {
        errorMessage = "路径过长";
        return false;
    }
    if (!isValidUtf8(relativePath)) {
        errorMessage = "路径不是有效的 UTF-8";
        return false;
    }
    if (relativePath.front() == '/' || relativePath.front() == '\\') {
        errorMessage = "不允许绝对路径";
        return false;
    }
    if (relativePath.find('\\') != std::string::npos) {
        errorMessage = "路径中不允许反斜杠";
        return false;
    }
    std::size_t start = 0;
    while (start <= relativePath.size()) {
        const std::size_t slash = relativePath.find('/', start);
        const std::size_t end =
            slash == std::string::npos ? relativePath.size() : slash;
        const std::string component = relativePath.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") {
            errorMessage = "路径包含非法片段";
            return false;
        }
        for (unsigned char c : component) {
            if (c < 0x20 || c == 0x7f) {
                errorMessage = "路径包含控制字符";
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
        if (start == relativePath.size()) {
            errorMessage = "路径不能以斜杠结尾";
            return false;
        }
    }
    return true;
}

fs::path appendRelativePath(const fs::path& root,
                            const std::string& relativePath) {
    fs::path result = root;
    std::size_t start = 0;
    while (start < relativePath.size()) {
        const std::size_t slash = relativePath.find('/', start);
        const std::size_t end =
            slash == std::string::npos ? relativePath.size() : slash;
        result /= pathFromUtf8(relativePath.substr(start, end - start));
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return result;
}

std::string caseFoldForPathComparison(std::string value) {
#ifdef _WIN32
    return platform::caseFoldForComparison(value);
#else
    return value;
#endif
}

std::string manifestNameKey(std::string value) {
#ifdef _WIN32
    return platform::caseFoldForComparison(value);
#else
    // Keep the historical portable behavior for top-level names: treat ASCII
    // case variants as the same name even though Linux would allow both.
    return toLowerAscii(std::move(value));
#endif
}

std::string pathCollisionKey(const fs::path& path) {
    return caseFoldForPathComparison(pathToUtf8(path));
}

std::string toLowerAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool endsWithIgnoreCase(std::string_view value, std::string_view suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(
            value[value.size() - suffix.size() + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

// Kept in this translation unit because callers may want a platform-agnostic
// filename validation result before attempting disk I/O.
bool isSafeWindowsComponent(const std::string& component) {
    if (component.empty()) return false;
    for (char c : component) {
        switch (c) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '|':
            case '?':
            case '*':
                return false;
            default:
                break;
        }
    }
    if (component.back() == ' ' || component.back() == '.') {
        return false;
    }
    return !isWindowsReservedName(component);
}

}  // namespace landrop
