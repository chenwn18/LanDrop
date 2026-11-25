#include "settings.h"

#include <sstream>

#include "platform.h"
#include "util.h"

namespace landrop {
namespace {

bool parseEnabled(const std::string& value, bool& enabled) {
    const std::string normalized = toLowerAscii(trim(value));
    if (normalized == "1" || normalized == "true" || normalized == "yes" ||
        normalized == "on") {
        enabled = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" ||
        normalized == "off") {
        enabled = false;
        return true;
    }
    return false;
}

}  // namespace

fs::path SettingsStore::settingsPath() {
    return platform::configDirectory() / pathFromUtf8("settings.ini");
}

AppSettings SettingsStore::load() {
    AppSettings settings;
    settings.receiveDirectory = platform::defaultReceiveDirectory();
    settings.allowIncoming = true;

    const fs::path path = settingsPath();
    FileReader reader;
    std::string error;
    if (!reader.open(path, error)) {
        return settings;
    }
    if (reader.size() == 0 || reader.size() > 64 * 1024) {
        return settings;
    }
    std::string contents(static_cast<std::size_t>(reader.size()), '\0');
    std::size_t obtained = 0;
    if (!reader.read(contents.data(), contents.size(), obtained, error) ||
        obtained != contents.size()) {
        return settings;
    }

    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = toLowerAscii(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "receive_dir" && !value.empty() &&
            isValidUtf8(value)) {
            fs::path candidate = pathFromUtf8(value);
            if (candidate.is_absolute()) {
                settings.receiveDirectory = candidate;
            }
        } else if (key == "allow_incoming") {
            bool enabled = settings.allowIncoming;
            if (parseEnabled(value, enabled)) {
                settings.allowIncoming = enabled;
            }
        }
    }
    return settings;
}

bool SettingsStore::save(const AppSettings& settings, std::string& error) {
    if (settings.receiveDirectory.empty()) {
        error = "接收目录为空";
        return false;
    }
    const std::string directory = genericPathToUtf8(settings.receiveDirectory);
    if (!isValidUtf8(directory) ||
        directory.find('\n') != std::string::npos ||
        directory.find('\r') != std::string::npos) {
        error = "接收目录包含无法保存的字符";
        return false;
    }
    std::ostringstream stream;
    stream << "# LanDrop settings\n";
    stream << "receive_dir=" << directory << "\n";
    stream << "allow_incoming=" << (settings.allowIncoming ? "1" : "0")
           << "\n";
    const std::string contents = stream.str();
    return writeFileAtomic(settingsPath(), contents.data(), contents.size(),
                           error);
}

}  // namespace landrop
