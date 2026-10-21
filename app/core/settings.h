#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace oap {

// Flat key/value store saved as "key<TAB>value" lines. An empty path keeps it in memory only.
class Settings {
public:
    explicit Settings(std::filesystem::path file) : file_(std::move(file)) {}

    void load();
    void save() const;
    void clear() { values_.clear(); }

    int getInt(const std::string& key, int fallback) const;
    void setInt(const std::string& key, int value) { values_[key] = std::to_string(value); }
    std::string getString(const std::string& key, const std::string& fallback = {}) const;
    void setString(const std::string& key, const std::string& value) { values_[key] = value; }

private:
    std::filesystem::path file_;
    std::map<std::string, std::string> values_;
};

} // namespace oap
