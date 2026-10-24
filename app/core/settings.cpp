#include "settings.h"

#include <sstream>

namespace oap {

void Settings::load() {
    if (store_) values_ = store_->load();
}

void Settings::save() const {
    if (store_) store_->save(values_);
}

void Settings::clearExcept(const std::string& prefix) {
    std::erase_if(values_, [&](const auto& kv) { return kv.first.rfind(prefix, 0) != 0; });
}

int Settings::getInt(const std::string& key, int fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

std::string Settings::getString(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

bool serverConfigured(const Settings& config) { return !config.getString("server.url").empty(); }

SettingsMap parseTsv(const std::string& text) {
    SettingsMap values;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab != std::string::npos) values[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return values;
}

std::string formatTsv(const SettingsMap& values) {
    std::string out;
    for (const auto& [key, value] : values) out += key + '\t' + value + '\n';
    return out;
}

} // namespace oap
