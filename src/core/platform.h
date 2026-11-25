#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace landrop {
namespace fs = std::filesystem;

namespace platform {

// Process-wide network initialization (Winsock on Windows).  Safe to call more
// than once from the application bootstrap.
bool initializeNetwork(std::string& error);
void shutdownNetwork();

fs::path configDirectory();
fs::path defaultReceiveDirectory();
// True for Windows reparse points (including junctions) and symlinks. Used to
// prevent recursive folder loops and reparse-point escapes.
bool isReparsePoint(const fs::path& path);
// Error text safe for the UTF-8 UI/protocol on every platform. In particular,
// avoid std::error_code::message() on Windows, which is returned in the active
// ANSI code page.
std::string errorCodeMessage(const std::error_code& error);
std::string hostName();
std::vector<std::string> localIpv4Addresses();
std::vector<std::string> broadcastAddresses();

// Windows filesystems are case-insensitive for the whole Unicode range.
// Returns an uppercase UTF-8 form suitable only for comparison/hashing, never
// for display or file access. On POSIX the input is returned unchanged.
std::string caseFoldForComparison(const std::string& utf8);

bool openInFileManager(const fs::path& path, std::string& error);
void sleepMilliseconds(int milliseconds);

#ifdef _WIN32
std::string wideToUtf8(const std::wstring& value);
std::wstring utf8ToWide(const std::string& value);
std::string win32ErrorMessage(unsigned long code);
#endif

}  // namespace platform

class FileReader {
public:
    FileReader();
    ~FileReader();
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;

    bool open(const fs::path& path, std::string& error);
    bool seek(uint64_t offset, std::string& error);
    bool read(void* buffer, std::size_t size, std::size_t& bytesRead,
              std::string& error);
    void close();
    bool isOpen() const { return handleIsValid(); }
    uint64_t size() const { return size_; }

private:
    bool handleIsValid() const;
    std::intptr_t handle_;
    uint64_t size_;
};

class FileWriter {
public:
    FileWriter();
    ~FileWriter();
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    FileWriter(FileWriter&& other) noexcept;
    FileWriter& operator=(FileWriter&& other) noexcept;

    // Opens (or creates) the file and positions it at `offset`, truncating any
    // bytes beyond that offset.  Resuming therefore starts from a known-good
    // prefix.
    bool openAt(const fs::path& path, uint64_t offset, std::string& error);
    bool writeAll(const void* buffer, std::size_t size, std::string& error);
    bool sync(std::string& error);
    void close();
    bool isOpen() const { return handleIsValid(); }

private:
    bool handleIsValid() const;
    std::intptr_t handle_;
};

bool fileExists(const fs::path& path);
bool directoryExists(const fs::path& path);
bool ensureDirectories(const fs::path& path, std::string& error);
bool atomicReplaceFile(const fs::path& source, const fs::path& destination,
                       std::string& error);
bool removeFileIfExists(const fs::path& path, std::string& error);
bool writeFileAtomic(const fs::path& path, const void* data, std::size_t size,
                     std::string& error);

}  // namespace landrop
