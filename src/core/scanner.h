#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "protocol.h"

namespace landrop {

namespace fs = std::filesystem;

struct ScannedEntry {
    proto::EntryKind kind = proto::EntryKind::File;
    std::string relativePath;
    fs::path sourcePath;
    uint64_t size = 0;
    Sha256::Digest digest{};
};

struct ScanResult {
    std::vector<ScannedEntry> entries;
    uint32_t fileCount = 0;
    uint32_t directoryCount = 0;
    uint64_t totalBytes = 0;
    std::vector<std::string> warnings;
};

enum class ScanStage { Discovering, Hashing };

struct ScanProgress {
    ScanStage stage = ScanStage::Discovering;
    uint64_t bytesDone = 0;
    uint64_t totalBytes = 0;
    uint32_t filesDone = 0;
    uint32_t fileCount = 0;
    std::string currentPath;
};

class FileScanner {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;
    using WarningCallback = std::function<void(const std::string&)>;

    static ScanResult scanAndHash(const std::vector<fs::path>& selectedPaths,
                                  const std::atomic<bool>& cancelled,
                                  const ProgressCallback& progress,
                                  const WarningCallback& warning);
};

}  // namespace landrop
