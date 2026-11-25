#include "core/platform.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#include "core/util.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#endif

namespace landrop {

FileReader::FileReader() : handle_(-1), size_(0) {}

FileReader::~FileReader() { close(); }

FileReader::FileReader(FileReader&& other) noexcept
    : handle_(other.handle_), size_(other.size_) {
    other.handle_ = -1;
    other.size_ = 0;
}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        size_ = other.size_;
        other.handle_ = -1;
        other.size_ = 0;
    }
    return *this;
}

bool FileReader::handleIsValid() const {
#ifdef _WIN32
    return handle_ != -1 &&
           reinterpret_cast<HANDLE>(handle_) != INVALID_HANDLE_VALUE;
#else
    return handle_ >= 0;
#endif
}

bool FileReader::open(const fs::path& path, std::string& error) {
    close();
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.native().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "无法打开文件: " + pathToUtf8(path) + " — " +
                platform::win32ErrorMessage(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        error = "无法读取文件大小: " + pathToUtf8(path) + " — " +
                platform::win32ErrorMessage(GetLastError());
        CloseHandle(handle);
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(handle);
    size_ = static_cast<uint64_t>(size.QuadPart);
#else
    int fd;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        error = "无法打开文件: " + pathToUtf8(path) + " — " +
                std::strerror(errno);
        return false;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < 0) {
        error = "无法读取文件大小: " + pathToUtf8(path) + " — " +
                std::strerror(errno);
        ::close(fd);
        return false;
    }
    handle_ = fd;
    size_ = static_cast<uint64_t>(info.st_size);
#endif
    return true;
}

bool FileReader::seek(uint64_t offset, std::string& error) {
    if (!isOpen()) {
        error = "文件未打开";
        return false;
    }
#ifdef _WIN32
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), position, nullptr,
                          FILE_BEGIN)) {
        error = "定位文件失败 — " + platform::win32ErrorMessage(GetLastError());
        return false;
    }
#else
    if (lseek(static_cast<int>(handle_), static_cast<off_t>(offset),
              SEEK_SET) < 0) {
        error = "定位文件失败 — " + std::string(std::strerror(errno));
        return false;
    }
#endif
    return true;
}

bool FileReader::read(void* buffer, std::size_t size, std::size_t& bytesRead,
                      std::string& error) {
    bytesRead = 0;
    if (!isOpen()) {
        error = "文件未打开";
        return false;
    }
    auto* output = static_cast<std::uint8_t*>(buffer);
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(handle_);
    while (bytesRead < size) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(size - bytesRead, 0x10000000u));
        DWORD obtained = 0;
        if (!ReadFile(handle, output + bytesRead, request, &obtained,
                      nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;
            }
            error = "读取文件失败 — " + platform::win32ErrorMessage(code);
            return false;
        }
        if (obtained == 0) {
            break;
        }
        bytesRead += obtained;
    }
#else
    const int fd = static_cast<int>(handle_);
    while (bytesRead < size) {
        const ssize_t result =
            ::read(fd, output + bytesRead, size - bytesRead);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "读取文件失败 — " + std::string(std::strerror(errno));
            return false;
        }
        if (result == 0) {
            break;
        }
        bytesRead += static_cast<std::size_t>(result);
    }
#endif
    return true;
}

void FileReader::close() {
#ifdef _WIN32
    if (handleIsValid()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
#else
    if (handle_ >= 0) {
        ::close(static_cast<int>(handle_));
    }
#endif
    handle_ = -1;
    size_ = 0;
}

FileWriter::FileWriter() : handle_(-1) {}

FileWriter::~FileWriter() { close(); }

FileWriter::FileWriter(FileWriter&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

FileWriter& FileWriter::operator=(FileWriter&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

bool FileWriter::handleIsValid() const {
#ifdef _WIN32
    return handle_ != -1 &&
           reinterpret_cast<HANDLE>(handle_) != INVALID_HANDLE_VALUE;
#else
    return handle_ >= 0;
#endif
}

bool FileWriter::openAt(const fs::path& path, uint64_t offset,
                        std::string& error) {
    close();
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.native().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "无法创建临时文件: " + pathToUtf8(path) + " — " +
                platform::win32ErrorMessage(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        error = "无法读取临时文件大小 — " + platform::win32ErrorMessage(GetLastError());
        CloseHandle(handle);
        return false;
    }
    if (static_cast<uint64_t>(size.QuadPart) < offset) {
        error = "续传数据短于预期，临时文件已损坏";
        CloseHandle(handle);
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle)) {
        error = "无法准备续传文件 — " + platform::win32ErrorMessage(GetLastError());
        CloseHandle(handle);
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    int fd;
    do {
        fd = ::open(path.c_str(),
                    O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        error = "无法创建临时文件: " + pathToUtf8(path) + " — " +
                std::strerror(errno);
        return false;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < 0) {
        error = "无法读取临时文件大小 — " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    if (static_cast<uint64_t>(info.st_size) < offset) {
        error = "续传数据短于预期，临时文件已损坏";
        ::close(fd);
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(offset)) != 0 ||
        lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        error = "无法准备续传文件 — " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    handle_ = fd;
#endif
    return true;
}

bool FileWriter::writeAll(const void* buffer, std::size_t size,
                          std::string& error) {
    if (!isOpen()) {
        error = "文件未打开";
        return false;
    }
    const auto* input = static_cast<const std::uint8_t*>(buffer);
    std::size_t written = 0;
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(handle_);
    while (written < size) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(size - written, 0x10000000u));
        DWORD count = 0;
        if (!WriteFile(handle, input + written, request, &count, nullptr)) {
            error = "写入文件失败 — " + platform::win32ErrorMessage(GetLastError());
            return false;
        }
        if (count == 0) {
            error = "写入文件失败：磁盘未写入任何数据";
            return false;
        }
        written += count;
    }
#else
    const int fd = static_cast<int>(handle_);
    while (written < size) {
        const ssize_t result = ::write(fd, input + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "写入文件失败 — " + std::string(std::strerror(errno));
            return false;
        }
        if (result == 0) {
            error = "写入文件失败：磁盘未写入任何数据";
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
#endif
    return true;
}

bool FileWriter::sync(std::string& error) {
    if (!isOpen()) {
        error = "文件未打开";
        return false;
    }
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(handle_))) {
        error = "刷新文件到磁盘失败 — " + platform::win32ErrorMessage(GetLastError());
        return false;
    }
#else
    if (fsync(static_cast<int>(handle_)) != 0) {
        error = "刷新文件到磁盘失败 — " + std::string(std::strerror(errno));
        return false;
    }
#endif
    return true;
}

void FileWriter::close() {
#ifdef _WIN32
    if (handleIsValid()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
#else
    if (handle_ >= 0) {
        ::close(static_cast<int>(handle_));
    }
#endif
    handle_ = -1;
}

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool directoryExists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool ensureDirectories(const fs::path& path, std::string& error) {
    if (path.empty()) {
        error = "目录路径为空";
        return false;
    }
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return true;
    }
    fs::create_directories(path, ec);
    if (ec) {
        error = "无法创建目录: " + pathToUtf8(path) + " — " + platform::errorCodeMessage(ec);
        return false;
    }
    return true;
}

bool atomicReplaceFile(const fs::path& source, const fs::path& destination,
                       std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(source.native().c_str(), destination.native().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "无法重命名文件: " + pathToUtf8(destination) + " — " +
                platform::win32ErrorMessage(GetLastError());
        return false;
    }
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        error = "无法重命名文件: " + pathToUtf8(destination) + " — " +
                std::strerror(errno);
        return false;
    }
    // Flush the destination directory entry so a crash right after success
    // cannot leave the verified file missing. Failure is non-fatal: the
    // rename itself already succeeded.
    const fs::path parent = destination.parent_path();
    if (!parent.empty()) {
        int dirFd;
        do {
            dirFd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        } while (dirFd < 0 && errno == EINTR);
        if (dirFd >= 0) {
            (void)fsync(dirFd);
            ::close(dirFd);
        }
    }
#endif
    return true;
}

bool removeFileIfExists(const fs::path& path, std::string& error) {
    std::error_code ec;
    const bool removed = fs::remove(path, ec);
    if (ec) {
        error = "无法删除文件: " + pathToUtf8(path) + " — " + platform::errorCodeMessage(ec);
        return false;
    }
    (void)removed;
    return true;
}

bool writeFileAtomic(const fs::path& path, const void* data, std::size_t size,
                     std::string& error) {
    const fs::path parent = path.parent_path();
    if (!parent.empty() && !ensureDirectories(parent, error)) {
        return false;
    }
    fs::path temporary = path;
    temporary += pathFromUtf8(".tmp." + randomHex(6));
    {
        FileWriter writer;
        if (!writer.openAt(temporary, 0, error)) {
            return false;
        }
        if (size != 0 && !writer.writeAll(data, size, error)) {
            writer.close();
            removeFileIfExists(temporary, error);
            return false;
        }
        if (!writer.sync(error)) {
            writer.close();
            removeFileIfExists(temporary, error);
            return false;
        }
    }
    if (!atomicReplaceFile(temporary, path, error)) {
        removeFileIfExists(temporary, error);
        return false;
    }
    return true;
}

}  // namespace landrop
