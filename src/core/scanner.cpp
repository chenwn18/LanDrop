#include "scanner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <stack>
#include <utility>

#include "common.h"
#include "platform.h"
#include "util.h"

namespace landrop {
namespace {

constexpr std::size_t kHashBufferSize = 1024 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

[[noreturn]] void fail(const std::string& message) {
    throw Error(ErrorKind::LocalIo, message);
}

void checkCancelled(const std::atomic<bool>& cancelled) {
    if (cancelled.load()) {
        throw Error(ErrorKind::Cancelled, "操作已取消");
    }
}

std::string safeRootName(const fs::path& absolutePath) {
    // A drag source such as "/home/user/folder/" has an empty filename();
    // walk up past trailing separators so the folder keeps its real name.
    fs::path cleaned = absolutePath;
    while (cleaned.filename().empty() && cleaned.has_parent_path() &&
           cleaned.parent_path() != cleaned) {
        cleaned = cleaned.parent_path();
    }
    std::string name = pathToUtf8(cleaned.filename());
    if (!name.empty()) {
        return name;
    }
    const std::string root = pathToUtf8(absolutePath.root_name());
    if (!root.empty()) {
        std::string cleaned;
        for (char c : root) {
            if (c != ':' && c != '\\' && c != '/') {
                cleaned.push_back(c);
            }
        }
        if (!cleaned.empty()) {
            return cleaned + "_root";
        }
    }
    return "root";
}

std::string addSuffix(const std::string& name, int number) {
    const fs::path path = pathFromUtf8(name);
    const std::string stem = pathToUtf8(path.stem());
    const std::string extension = pathToUtf8(path.extension());
    return stem + " (" + std::to_string(number) + ")" + extension;
}

bool validateTopLevelComponent(const std::string& name, std::string& error) {
    if (name.empty()) {
        error = "名称为空";
        return false;
    }
    if (!isValidUtf8(name)) {
        error = "名称不是有效的 UTF-8";
        return false;
    }
    if (name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        error = "名称包含非法字符";
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) {
            error = "名称包含控制字符";
            return false;
        }
    }
    return true;
}

struct PendingDirectory {
    fs::path source;
    std::string relativePath;
};

}  // namespace

ScanResult FileScanner::scanAndHash(
    const std::vector<fs::path>& selectedPaths,
    const std::atomic<bool>& cancelled, const ProgressCallback& progress,
    const WarningCallback& warning) {
    if (selectedPaths.empty()) {
        fail("没有选择要发送的文件或文件夹");
    }

    ScanResult result;
    std::set<std::string> usedRootNames;
    std::set<std::string> usedSourcePaths;
    std::vector<PendingDirectory> directories;
    auto lastProgress = std::chrono::steady_clock::time_point{};

    auto reportDiscovery = [&](const fs::path& current) {
        const auto now = std::chrono::steady_clock::now();
        if (progress == nullptr ||
            (now - lastProgress < kProgressInterval && lastProgress.time_since_epoch().count() != 0)) {
            return;
        }
        lastProgress = now;
        ScanProgress update;
        update.stage = ScanStage::Discovering;
        update.currentPath = pathToUtf8(current);
        progress(update);
    };

    std::size_t totalPathBytes = 0;
    std::size_t totalPathComponents = 0;
    auto addEntry = [&](ScannedEntry entry) {
        if (result.entries.size() >= kMaxManifestEntries) {
            fail("文件数量超过上限（" + std::to_string(kMaxManifestEntries) + "）");
        }
        std::string pathError;
        if (!validateRelativePath(entry.relativePath, pathError)) {
            fail("无法发送路径 " + entry.relativePath + "：" + pathError);
        }
        const std::size_t pathComponents =
            1 + static_cast<std::size_t>(
                    std::count(entry.relativePath.begin(),
                               entry.relativePath.end(), '/'));
        if (pathComponents > kMaxManifestPathDepth ||
            entry.relativePath.size() >
                kMaxManifestPathBytes - totalPathBytes ||
            pathComponents > kMaxManifestPathComponents - totalPathComponents) {
            fail("传输清单的路径总量超过上限");
        }
        totalPathBytes += entry.relativePath.size();
        totalPathComponents += pathComponents;
        if (entry.kind == proto::EntryKind::File) {
            if (result.fileCount == UINT32_MAX ||
                result.totalBytes > UINT64_MAX - entry.size) {
                fail("文件总大小溢出");
            }
            ++result.fileCount;
            result.totalBytes += entry.size;
        } else {
            ++result.directoryCount;
        }
        result.entries.push_back(std::move(entry));
    };

    for (const auto& selected : selectedPaths) {
        checkCancelled(cancelled);
        std::error_code ec;
        fs::path absolute = fs::absolute(selected, ec);
        if (ec) {
            fail("无法解析路径 " + pathToUtf8(selected) + "：" + platform::errorCodeMessage(ec));
        }
        absolute = absolute.lexically_normal();
        const std::string sourceKey = pathCollisionKey(absolute);
        if (!usedSourcePaths.insert(sourceKey).second) {
            if (warning != nullptr) {
                warning("已忽略重复选择的路径: " + pathToUtf8(selected));
            }
            continue;
        }
        const fs::file_status status = fs::symlink_status(absolute, ec);
        if (ec || !fs::exists(status)) {
            fail("路径不存在或无法访问: " + pathToUtf8(selected));
        }
        if (fs::is_symlink(status) ||
            platform::isReparsePoint(absolute)) {
            fail("不支持发送符号链接/目录重解析点: " +
                 pathToUtf8(selected));
        }
        const bool isFile = fs::is_regular_file(status);
        const bool isDirectory = fs::is_directory(status);
        if (!isFile && !isDirectory) {
            fail("不支持的路径类型: " + pathToUtf8(selected));
        }

        std::string name = safeRootName(absolute);
        std::string nameError;
        if (!validateTopLevelComponent(name, nameError)) {
            fail("无法发送 " + pathToUtf8(selected) + "：" + nameError);
        }
        std::string key = manifestNameKey(name);
        if (!usedRootNames.insert(key).second) {
            int suffix = 2;
            std::string candidate;
            do {
                candidate = addSuffix(name, suffix++);
                key = manifestNameKey(candidate);
            } while (!usedRootNames.insert(key).second);
            name = candidate;
            if (warning != nullptr) {
                warning("顶层名称重复，已重命名为 " + name);
            }
        }

        reportDiscovery(absolute);
        if (isFile) {
            std::uintmax_t size = fs::file_size(absolute, ec);
            if (ec) {
                fail("无法读取文件大小 " + pathToUtf8(absolute) + "：" +
                     platform::errorCodeMessage(ec));
            }
            ScannedEntry entry;
            entry.kind = proto::EntryKind::File;
            entry.relativePath = name;
            entry.sourcePath = absolute;
            entry.size = static_cast<uint64_t>(size);
            addEntry(std::move(entry));
        } else {
            ScannedEntry root;
            root.kind = proto::EntryKind::Directory;
            root.relativePath = name;
            root.sourcePath = absolute;
            addEntry(std::move(root));
            directories.push_back({absolute, name});
        }
    }

    while (!directories.empty()) {
        checkCancelled(cancelled);
        PendingDirectory task = std::move(directories.back());
        directories.pop_back();

        reportDiscovery(task.source);
        std::error_code ec;
        fs::directory_iterator iterator(
            task.source, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            if (warning != nullptr) {
                warning("无法读取目录 " + pathToUtf8(task.source) + "：" +
                        platform::errorCodeMessage(ec));
            }
            continue;
        }
        struct Child {
            fs::path path;
            std::string name;
            fs::file_status status;
        };
        std::vector<Child> children;
        for (const auto& entry : iterator) {
            fs::file_status status = entry.symlink_status(ec);
            if (ec) {
                if (warning != nullptr) {
                    warning("无法读取 " + pathToUtf8(entry.path()) + "：" +
                            platform::errorCodeMessage(ec));
                }
                ec.clear();
                continue;
            }
            Child child;
            child.path = entry.path();
            child.name = pathToUtf8(entry.path().filename());
            child.status = status;
            children.push_back(std::move(child));
        }
        std::sort(children.begin(), children.end(),
                  [](const Child& a, const Child& b) {
                      return a.name < b.name;
                  });

        for (const auto& child : children) {
            checkCancelled(cancelled);
            std::string nameError;
            if (!validateTopLevelComponent(child.name, nameError)) {
                if (warning != nullptr) {
                    warning("跳过名称非法的项目 " + pathToUtf8(child.path) +
                            "：" + nameError);
                }
                continue;
            }
            if (fs::is_symlink(child.status) ||
                platform::isReparsePoint(child.path)) {
                if (warning != nullptr) {
                    warning("跳过符号链接/目录重解析点 " +
                            pathToUtf8(child.path));
                }
                continue;
            }
            const std::string relative = task.relativePath + "/" + child.name;
            if (fs::is_directory(child.status)) {
                ScannedEntry entry;
                entry.kind = proto::EntryKind::Directory;
                entry.relativePath = relative;
                entry.sourcePath = child.path;
                addEntry(std::move(entry));
                directories.push_back({child.path, relative});
            } else if (fs::is_regular_file(child.status)) {
                checkCancelled(cancelled);
                const std::uintmax_t size = fs::file_size(child.path, ec);
                if (ec) {
                    if (warning != nullptr) {
                        warning("无法读取文件大小 " + pathToUtf8(child.path) +
                                "：" + platform::errorCodeMessage(ec));
                    }
                    ec.clear();
                    continue;
                }
                ScannedEntry entry;
                entry.kind = proto::EntryKind::File;
                entry.relativePath = relative;
                entry.sourcePath = child.path;
                entry.size = static_cast<uint64_t>(size);
                addEntry(std::move(entry));
            } else {
                if (warning != nullptr) {
                    warning("跳过特殊文件 " + pathToUtf8(child.path));
                }
            }
        }
    }

    if (result.entries.empty()) {
        fail("没有找到可发送的文件或文件夹");
    }

    std::vector<uint8_t> buffer(kHashBufferSize);
    uint64_t bytesHashed = 0;
    uint32_t filesHashed = 0;
    lastProgress = std::chrono::steady_clock::time_point{};
    for (auto& entry : result.entries) {
        if (entry.kind != proto::EntryKind::File) {
            continue;
        }
        checkCancelled(cancelled);
        FileReader reader;
        std::string error;
        if (!reader.open(entry.sourcePath, error)) {
            fail(error);
        }
        if (reader.size() != entry.size) {
            fail("文件在发送准备期间发生变化，请重试: " +
                 pathToUtf8(entry.sourcePath));
        }
        Sha256 hasher;
        uint64_t fileRead = 0;
        for (;;) {
            checkCancelled(cancelled);
            std::size_t obtained = 0;
            if (!reader.read(buffer.data(), buffer.size(), obtained, error)) {
                fail(error);
            }
            if (obtained == 0) {
                break;
            }
            hasher.update(buffer.data(), obtained);
            fileRead += obtained;
            bytesHashed += obtained;
            const auto now = std::chrono::steady_clock::now();
            if (progress != nullptr &&
                (lastProgress.time_since_epoch().count() == 0 ||
                 now - lastProgress >= kProgressInterval ||
                 bytesHashed == result.totalBytes)) {
                lastProgress = now;
                ScanProgress update;
                update.stage = ScanStage::Hashing;
                update.bytesDone = bytesHashed;
                update.totalBytes = result.totalBytes;
                update.filesDone = filesHashed;
                update.fileCount = result.fileCount;
                update.currentPath = entry.relativePath;
                progress(update);
            }
        }
        if (fileRead != entry.size) {
            fail("文件在发送准备期间发生变化，请重试: " +
                 pathToUtf8(entry.sourcePath));
        }
        entry.digest = hasher.finish();
        ++filesHashed;
    }

    if (progress != nullptr) {
        ScanProgress update;
        update.stage = ScanStage::Hashing;
        update.bytesDone = result.totalBytes;
        update.totalBytes = result.totalBytes;
        update.filesDone = result.fileCount;
        update.fileCount = result.fileCount;
        progress(update);
    }
    return result;
}

}  // namespace landrop
