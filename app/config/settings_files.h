#pragma once

#include "config/stores.h"
#include "core/settings.h"

#include <filesystem>

namespace oap {

// What a front end hands to App: config.yaml and state.tsv, loaded.
class SettingsFiles {
public:
    SettingsFiles(const std::filesystem::path& configFile, const std::filesystem::path& stateFile);
    SettingsFiles(const SettingsFiles&) = delete; // the Settings point at the stores
    SettingsFiles& operator=(const SettingsFiles&) = delete;

    Settings& config() { return config_; }
    Settings& state() { return state_; }

private:
    YamlStore configStore_;
    TsvStore stateStore_;
    Settings config_{&configStore_};
    Settings state_{&stateStore_};
};

// The default --state: state.tsv in the folder of config.yaml.
inline std::filesystem::path defaultStateFile(const std::filesystem::path& configFile) {
    return configFile.parent_path() / "state.tsv";
}

} // namespace oap
