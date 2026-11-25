#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace landrop {

namespace fs = std::filesystem;

std::string trim(std::string value);
bool isValidUtf8(const std::string& value);
std::string truncateUtf8(std::string value, std::size_t maxBytes);
std::string formatBytes(uint64_t bytes);
std::string formatRate(double bytesPerSecond);
std::string formatDuration(double seconds);
std::string formatClockTime(int64_t unixSeconds);
std::string randomHex(std::size_t byteCount);

fs::path pathFromUtf8(const std::string& utf8);
std::string pathToUtf8(const fs::path& path);
std::string genericPathToUtf8(const fs::path& path);
// Expands a leading "~" or "~user" component with the account database.
// Windows has no shell tilde convention, so paths are returned unchanged
// there. Paths that do not start with '~' or name an unknown account are
// returned unchanged as well.
fs::path expandUserPath(const fs::path& path);

bool validateRelativePath(const std::string& relativePath,
                          std::string& errorMessage);
bool isSafeWindowsComponent(const std::string& component);
fs::path appendRelativePath(const fs::path& root,
                            const std::string& relativePath);
std::string caseFoldForPathComparison(std::string value);
std::string manifestNameKey(std::string value);
std::string pathCollisionKey(const fs::path& path);

std::string toLowerAscii(std::string value);
bool endsWithIgnoreCase(std::string_view value, std::string_view suffix);

}  // namespace landrop
