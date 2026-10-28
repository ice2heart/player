#pragma once

#include <map>
#include <string>
#include <vector>

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
    void clearExcept(const std::vector<std::string>& prefixes); // keeps the keys starting with any of `prefixes`
    void clearPrefix(const std::string& prefix); // removes the keys starting with `prefix`
    void erase(const std::string& key) { values_.erase(key); } // removes that key only

    int getInt(const std::string& key, int fallback) const;
    void setInt(const std::string& key, int value) { values_[key] = std::to_string(value); }
    std::string getString(const std::string& key, const std::string& fallback = {}) const;
    void setString(const std::string& key, const std::string& value) { values_[key] = value; }
    const SettingsMap& values() const { return values_; }

private:
    SettingsStore* store_ = nullptr;
    SettingsMap values_;
};

// The server keys of config.yaml: the `servers:` list and the older single `server:` section.
// Factory reset keeps both.
constexpr const char* kServerPrefix = "server.";
constexpr const char* kServersPrefix = "servers.";

struct ServerConfig {
    std::string type;            // "subsonic" (also when left out) or "audiobookshelf", lower case
    std::string name;            // as set; empty when not set
    std::string url, user;       // url without trailing slashes
    std::string password;        // subsonic
    std::string apiKey;          // audiobookshelf
    std::string audiobookFolder; // subsonic
    std::string library;         // audiobookshelf
};

// The servers set up, in order: the `servers:` list, else a lone `server:` section as a list of
// one. An entry without a url is left out, and so is one with the same url and user as an
// earlier entry (TrackCache::serverKey). Core prints nothing: a line for the user about what was
// ignored (`server:` next to `servers:`, a repeated server) is added to `warnings` when given.
std::vector<ServerConfig> serverConfigs(const Settings& config, std::vector<std::string>* warnings = nullptr);

// A server is set up when serverConfigs() is not empty.
bool serverConfigured(const Settings& config);

// The "key<TAB>value" lines of state.tsv. Lines without a tab are skipped.
SettingsMap parseTsv(const std::string& text);
std::string formatTsv(const SettingsMap& values);

} // namespace oap
