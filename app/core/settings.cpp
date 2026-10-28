#include "settings.h"

#include "track_cache.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace oap {

void Settings::load() {
    if (store_) values_ = store_->load();
}

bool Settings::save() const {
    return !store_ || store_->save(values_);
}

void Settings::clearExcept(const std::vector<std::string>& prefixes) {
    std::erase_if(values_, [&](const auto& kv) {
        return std::none_of(prefixes.begin(), prefixes.end(),
                            [&](const std::string& prefix) { return kv.first.rfind(prefix, 0) == 0; });
    });
}

void Settings::clearPrefix(const std::string& prefix) {
    std::erase_if(values_, [&](const auto& kv) { return kv.first.rfind(prefix, 0) == 0; });
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

namespace {

ServerConfig readServer(const Settings& config, const std::string& prefix) { // prefix: "server." or "servers.N."
    auto get = [&](const char* name) { return config.getString(prefix + name); };
    ServerConfig c;
    c.type = get("type");
    std::transform(c.type.begin(), c.type.end(), c.type.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (c.type.empty()) c.type = "subsonic";
    c.name = get("name");
    c.url = get("url");
    while (!c.url.empty() && c.url.back() == '/') c.url.pop_back();
    c.user = get("user");
    c.password = get("password");
    c.apiKey = get("api_key");
    c.audiobookFolder = get("audiobook_folder");
    c.library = get("library");
    return c;
}

} // namespace

std::vector<ServerConfig> serverConfigs(const Settings& config, std::vector<std::string>* warnings) {
    std::set<int> indices; // the numbers of the servers.N keys, in order
    const std::string prefix = kServersPrefix;
    for (const auto& [key, value] : config.values()) {
        if (key.rfind(prefix, 0) != 0) continue;
        std::string number = key.substr(prefix.size(), key.find('.', prefix.size()) - prefix.size());
        if (number.empty() || number.size() >= 9 ||
            !std::all_of(number.begin(), number.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
            continue;
        int i = std::stoi(number);
        if (std::to_string(i) == number) indices.insert(i); // "01" is not an index the file writer would make
    }
    std::vector<ServerConfig> all;
    if (!indices.empty()) {
        if (!config.getString("server.url").empty() && warnings)
            warnings->push_back("config.yaml has servers: and server:. server: is ignored.");
        for (int i : indices) all.push_back(readServer(config, prefix + std::to_string(i) + "."));
    } else {
        all.push_back(readServer(config, kServerPrefix));
    }

    std::vector<ServerConfig> out;
    std::set<std::string> keys;
    for (auto& c : all) {
        if (c.url.empty()) continue;
        if (!keys.insert(TrackCache::serverKey(c.url, c.user)).second) {
            if (warnings) warnings->push_back("A server with the same url and user is set up twice. The later one is left out.");
            continue;
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool serverConfigured(const Settings& config) { return !serverConfigs(config).empty(); }

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
