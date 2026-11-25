#pragma once

#include <filesystem>
#include <string>

namespace landrop {

namespace fs = std::filesystem;

struct AppSettings {
    fs::path receiveDirectory;
    bool allowIncoming = true;
};

class SettingsStore {
public:
    static fs::path settingsPath();
    static AppSettings load();
    static bool save(const AppSettings& settings, std::string& error);
};

}  // namespace landrop
