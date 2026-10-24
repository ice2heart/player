#include "settings_files.h"

namespace oap {

SettingsFiles::SettingsFiles(const std::filesystem::path& configFile, const std::filesystem::path& stateFile)
    : configStore_(configFile), stateStore_(stateFile) {
    config_.load();
    state_.load();
}

} // namespace oap
