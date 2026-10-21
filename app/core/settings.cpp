#include "settings.h"

#include <fstream>

namespace oap {

void Settings::load() {
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab != std::string::npos) values_[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

void Settings::save() const {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream out(file_);
    for (const auto& [key, value] : values_) out << key << '\t' << value << '\n';
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

} // namespace oap
