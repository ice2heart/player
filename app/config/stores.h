#pragma once

#include "core/settings.h"

#include <filesystem>
#include <string>

namespace oap {

// state.tsv: playback positions and the book to resume, as "key<TAB>value" lines.
class TsvStore : public SettingsStore {
public:
    explicit TsvStore(std::filesystem::path file) : file_(std::move(file)) {}
    SettingsMap load() override;
    void save(const SettingsMap& values) override;

private:
    std::filesystem::path file_;
};

// config.yaml: nested YAML on disk, dotted keys in memory ("speech: {rate: 220}"
// is "speech.rate"). A file that cannot be parsed is reported and never
// overwritten, so a typo does not cost the user the server set up in it.
class YamlStore : public SettingsStore {
public:
    explicit YamlStore(std::filesystem::path file) : file_(std::move(file)) {}
    SettingsMap load() override;
    void save(const SettingsMap& values) override;

private:
    std::filesystem::path file_;
    bool unreadable_ = false;
};

// YAML text -> dotted keys. Null values are left out; list items get their index
// as key ("servers.0.url"). Throws YAML::Exception on a syntax error, or when
// the document is not a mapping (an empty one is fine).
SettingsMap flattenYaml(const std::string& text);
// Dotted keys -> YAML text, with a header comment, the known keys in a fixed
// order and the others after them. Keys numbered 0..n-1 become a list again.
std::string nestYaml(const SettingsMap& values);

} // namespace oap
