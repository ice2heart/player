#pragma once

#include <map>
#include <string>

namespace oap {

using SettingsMap = std::map<std::string, std::string>;

// Where a Settings object is read from and written to: a YAML file, a TSV file
// (app/config/), or a test fake. Core never sees the file format.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual SettingsMap load() = 0; // empty when there is no file yet
    virtual bool save(const SettingsMap& values) = 0; // false: not written (the reason is printed)
};

// Flat key/value map with dotted keys ("speech.rate", "server.url"), kept in
// memory. load() and save() go through the store; without one it is memory only.
// The store must outlive the Settings.
class Settings {
public:
    Settings() = default;
    explicit Settings(SettingsStore* store) : store_(store) {}

    void load();
    bool save() const; // false when the store could not write it
    void clear() { values_.clear(); }
    void clearExcept(const std::string& prefix); // keeps the keys starting with `prefix`

    int getInt(const std::string& key, int fallback) const;
    void setInt(const std::string& key, int value) { values_[key] = std::to_string(value); }
    std::string getString(const std::string& key, const std::string& fallback = {}) const;
    void setString(const std::string& key, const std::string& value) { values_[key] = value; }
    const SettingsMap& values() const { return values_; }

private:
    SettingsStore* store_ = nullptr;
    SettingsMap values_;
};

// The server section of config.yaml: a server is set up when it has a URL.
constexpr const char* kServerPrefix = "server.";
bool serverConfigured(const Settings& config);

// The "key<TAB>value" lines of state.tsv. Lines without a tab are skipped.
SettingsMap parseTsv(const std::string& text);
std::string formatTsv(const SettingsMap& values);

} // namespace oap
