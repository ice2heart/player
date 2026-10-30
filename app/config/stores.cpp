#include "stores.h"

#include "atomic_file.h"
#include "core/settings.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace oap {

namespace {

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// The order keys are written in, so the file reads like the Settings menu.
const char* const kKeyOrder[] = {
    "speech.rate",   "volume",          "verbosity",        "skip.back",   "skip.forward",
    "rewind_on_resume", "buttons.long_press_ms", "menu.wrap", "power.auto_sleep_min", "streaming.bitrate",
    "cache.limit_gb", "server.type",   "server.name",     "server.url",       "server.user", "server.password",
};

// The order of the keys in an entry of the `servers:` list.
const char* const kServerKeyOrder[] = {"type", "name", "url", "user", "password", "api_key", "audiobook_folder", "library"};

// The last part of "servers.1.url": "url".
std::string lastPart(const std::string& key) { return key.substr(key.rfind('.') + 1); }

bool inServers(const std::string& key) { return key.rfind(kServersPrefix, 0) == 0; }

// Always quoted, so an all-digit password or API key is never read as a number by anyone.
bool isSecret(const std::string& key) {
    if (key != "server.password" && key != "server.api_key" && !inServers(key)) return false;
    std::string name = lastPart(key);
    return name == "password" || name == "api_key";
}

const char* const kHeader =
    "# Open Audio Player settings. The player rewrites this file when a setting is\n"
    "# changed in the Settings menu; comments added here are not kept.\n"
    "# Keys: speech.rate, volume, verbosity, rewind_on_resume,\n"
    "#   buttons.long_press_ms, menu.wrap, power.auto_sleep_min, streaming.bitrate (kbps,\n"
    "#   0 for the original file), cache.limit_gb (gigabytes of automatically cached tracks,\n"
    "#   0 for none), and server: type, name, url, user, password (quote it),\n"
    "#   audiobook_folder (the name of the server's music folder that holds audiobooks; you set it, the player never writes it).\n"
    "#   Several servers: a servers: list, each entry with type (subsonic or audiobookshelf), name, url, user,\n"
    "#   password (quote it), api_key (quote it), audiobook_folder and library. An old server: section still works.\n";

void flatten(const YAML::Node& node, const std::string& key, SettingsMap& out) {
    auto child = [&key](const std::string& name) { return key.empty() ? name : key + "." + name; };
    switch (node.Type()) {
    case YAML::NodeType::Map:
        for (const auto& kv : node)
            if (kv.first.IsScalar() && !kv.first.Scalar().empty()) flatten(kv.second, child(kv.first.Scalar()), out);
        break;
    case YAML::NodeType::Sequence:
        for (size_t i = 0; i < node.size(); ++i) flatten(node[i], child(std::to_string(i)), out);
        break;
    case YAML::NodeType::Scalar:
        if (!key.empty()) out[key] = node.Scalar();
        break;
    default: // Null, Undefined
        break;
    }
}

struct Tree {
    std::string value;
    bool leaf = false;
    std::vector<std::pair<std::string, Tree>> children; // in insertion order
};

Tree& childOf(Tree& tree, const std::string& name) {
    for (auto& [n, t] : tree.children)
        if (n == name) return t;
    return tree.children.emplace_back(name, Tree{}).second;
}

// "a" and "a.b" cannot both be written; the section wins over the single value.
void insert(Tree& root, const std::string& key, const std::string& value) {
    Tree* node = &root;
    size_t start = 0;
    for (size_t dot; (dot = key.find('.', start)) != std::string::npos; start = dot + 1) {
        node = &childOf(*node, key.substr(start, dot - start));
        node->leaf = false;
    }
    Tree& last = childOf(*node, key.substr(start));
    if (last.children.empty()) last.leaf = true, last.value = value;
}

// Children named 0, 1, ... n-1 were a list: their order by number, else nothing.
std::optional<std::vector<const Tree*>> asList(const Tree& tree) {
    std::vector<const Tree*> items(tree.children.size(), nullptr);
    for (const auto& [name, child] : tree.children) {
        if (name.empty() || name.size() > 9 || !std::all_of(name.begin(), name.end(), isDigit)) return std::nullopt;
        size_t i = std::stoul(name);
        if (i >= items.size() || items[i] || std::to_string(i) != name) return std::nullopt;
        items[i] = &child;
    }
    return items;
}

bool isPlainInteger(const std::string& v) {
    size_t digits = v.rfind('-', 0) == 0 ? 1 : 0;
    if (v.size() == digits || v.size() > digits + 9) return false;
    if (!std::all_of(v.begin() + long(digits), v.end(), isDigit)) return false;
    return v == std::to_string(std::stol(v)); // no leading zeros or "-0"
}

void emit(YAML::Emitter& out, const Tree& tree, const std::string& key) {
    if (tree.leaf) {
        if (isPlainInteger(tree.value) && !isSecret(key)) out << tree.value;
        else out << YAML::DoubleQuoted << tree.value;
        return;
    }
    auto child = [&key](const std::string& name) { return key.empty() ? name : key + "." + name; };
    if (auto list = asList(tree); list && !list->empty()) {
        out << YAML::BeginSeq;
        for (size_t i = 0; i < list->size(); ++i) emit(out, *(*list)[i], child(std::to_string(i)));
        out << YAML::EndSeq;
        return;
    }
    out << YAML::BeginMap;
    for (const auto& [name, sub] : tree.children) {
        out << YAML::Key << name << YAML::Value;
        emit(out, sub, child(name));
    }
    out << YAML::EndMap;
}

size_t rank(const std::string& key) {
    auto it = std::find_if(std::begin(kKeyOrder), std::end(kKeyOrder), [&](const char* k) { return key == k; });
    return size_t(it - std::begin(kKeyOrder));
}

// Where a key sorts: its place in kKeyOrder, then for "servers.N.field" the place of the field,
// so every entry is written in the same key order (the entries keep their own order).
std::pair<size_t, size_t> sortKey(const std::string& key) {
    if (!inServers(key)) return {rank(key), 0};
    std::string field = lastPart(key);
    auto it = std::find_if(std::begin(kServerKeyOrder), std::end(kServerKeyOrder), [&](const char* f) { return field == f; });
    return {rank(key), size_t(it - std::begin(kServerKeyOrder))};
}

} // namespace

SettingsMap flattenYaml(const std::string& text) {
    YAML::Node root = YAML::Load(text);
    // Anything else, such as an old settings.tsv (one long string), is not ours to overwrite.
    if (!root.IsMap() && !root.IsNull()) throw YAML::Exception(YAML::Mark::null_mark(), "expected keys and values");
    SettingsMap out;
    flatten(root, "", out);
    return out;
}

std::string nestYaml(const SettingsMap& values) {
    std::vector<std::pair<std::string, std::string>> ordered(values.begin(), values.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return sortKey(a.first) < sortKey(b.first); });
    Tree root;
    for (const auto& [key, value] : ordered) insert(root, key, value);
    std::string body;
    if (!root.children.empty()) {
        YAML::Emitter out;
        emit(out, root, "");
        body = std::string(out.c_str()) + "\n";
    }
    return kHeader + body;
}

SettingsMap TsvStore::load() { return parseTsv(readFile(file_).value_or("")); }

bool TsvStore::save(const SettingsMap& values) { return writeFileAtomically(file_, formatTsv(values)); }

SettingsMap YamlStore::load() {
    unreadable_ = false;
    auto text = readFile(file_);
    if (!text) {
        std::error_code ec;
        if (std::filesystem::exists(file_, ec) || ec) { // there, but not for us to read
            unreadable_ = true;
            std::fprintf(stderr, "%s: cannot read it. Using default settings; the file will not be changed.\n",
                         file_.c_str());
        }
        return {};
    }
    try {
        return flattenYaml(*text);
    } catch (const YAML::Exception& e) {
        unreadable_ = true;
        std::fprintf(stderr, "%s: %s. Using default settings; the file will not be changed.\n", file_.c_str(), e.what());
        return {};
    }
}

bool YamlStore::save(const SettingsMap& values) {
    if (unreadable_) {
        std::fprintf(stderr, "%s: not saved, the file has an error. Fix it and restart.\n", file_.c_str());
        return false;
    }
    return writeFileAtomically(file_, nestYaml(values));
}

} // namespace oap
