#include "track_cache.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace oap {

namespace {
constexpr const char* kManifestName = "manifest.tsv";
constexpr const char* kManifestHeader = "oap-cache 1";
constexpr const char* kPartSuffix = ".part";
constexpr size_t kColumns = 7;
constexpr size_t kHostLimit = 40; // characters of the host kept in a server key
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

bool plainChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_';
}

std::string hex(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

// The URL without a trailing slash, its scheme and any `user:password@` in front of the host.
std::string hostAndPath(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (auto scheme = url.find("://"); scheme != std::string::npos) url.erase(0, scheme + 3);
    auto slash = url.find('/');
    if (auto at = url.find('@'); at != std::string::npos && (slash == std::string::npos || at < slash))
        url.erase(0, at + 1);
    return url;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

// False for an escape that is not one of ours.
bool unescape(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        if (++i == s.size()) return false;
        switch (s[i]) {
        case '\\': out += '\\'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        default: return false;
        }
    }
    return true;
}

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return *end == '\0';
}

bool parseInteger(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, 10);
    return *end == '\0';
}

// A file name that stays inside the list's folder.
bool plainFileName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos && name != kManifestName;
}

bool parseLine(const std::string& line, CachedTrack& track) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, '\t')) cells.push_back(cell);
    if (!line.empty() && line.back() == '\t') cells.emplace_back();
    if (cells.size() != kColumns) return false;
    double duration = 0;
    int64_t lastPlayed = 0;
    if (!unescape(cells[0], track.id) || !unescape(cells[1], track.file) || !unescape(cells[2], track.title) ||
        !parseNumber(cells[3], duration) || !unescape(cells[4], track.changed) ||
        (cells[5] != "0" && cells[5] != "1") || !parseInteger(cells[6], lastPlayed))
        return false;
    track.duration = duration;
    track.pinned = cells[5] == "1";
    track.lastPlayed = lastPlayed;
    return !track.id.empty() && plainFileName(track.file);
}

// The tracks of a manifest file; empty if it is missing or any line is wrong.
std::vector<CachedTrack> readManifest(const fs::path& file) {
    std::vector<CachedTrack> tracks;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line != kManifestHeader) return tracks;
    while (std::getline(in, line)) {
        CachedTrack track;
        if (!parseLine(line, track)) return {};
        tracks.push_back(std::move(track));
    }
    if (in.bad()) return {};
    return tracks;
}
} // namespace

TrackCache::TrackCache(fs::path root, const std::string& serverUrl, const std::string& user) {
    std::string key = serverKey(serverUrl, user);
    if (!key.empty() && !root.empty()) serverDir_ = root / key;
}

std::string TrackCache::serverKey(const std::string& serverUrl, const std::string& user) {
    std::string rest = hostAndPath(serverUrl);
    if (rest.empty()) return "";
    uint64_t hash = kFnvOffset;
    for (unsigned char c : rest + '\n' + user) hash = (hash ^ c) * kFnvPrime;
    std::string host;
    for (unsigned char c : rest.substr(0, rest.find('/'))) {
        if (host.size() == kHostLimit) break;
        host += plainChar(c) && c != '.' ? static_cast<char>(c) : '_';
    }
    return host + "-" + hex(hash);
}

std::string TrackCache::safeName(const std::string& id) {
    std::string out;
    for (unsigned char c : id) {
        if (plainChar(c)) out += static_cast<char>(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    if (out.empty()) return "%00"; // an empty id still gets a name
    if (out.find_first_not_of('.') == std::string::npos) { // "." and ".." are not names
        std::string encoded;
        for (size_t i = 0; i < out.size(); ++i) encoded += "%2E";
        return encoded;
    }
    return out;
}

fs::path TrackCache::listFolder(const std::string& listId) const {
    return enabled() ? serverDir_ / safeName(listId) : fs::path();
}

fs::path TrackCache::manifestFile(const std::string& listId) const {
    return enabled() ? listFolder(listId) / kManifestName : fs::path();
}

std::vector<CachedTrack> TrackCache::load(const std::string& listId) const {
    return enabled() ? readManifest(manifestFile(listId)) : std::vector<CachedTrack>();
}

bool TrackCache::save(const std::string& listId, const std::vector<CachedTrack>& tracks) const {
    if (!enabled()) return false;
    std::error_code ec;
    fs::create_directories(listFolder(listId), ec);
    if (ec) return false;
    fs::path file = manifestFile(listId);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << kManifestHeader << '\n';
        for (const CachedTrack& t : tracks) {
            char duration[32];
            std::snprintf(duration, sizeof duration, "%.3f", t.duration);
            out << escape(t.id) << '\t' << escape(t.file) << '\t' << escape(t.title) << '\t' << duration << '\t'
                << escape(t.changed) << '\t' << (t.pinned ? '1' : '0') << '\t' << t.lastPlayed << '\n';
        }
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<fs::path> TrackCache::find(const std::string& trackId) const {
    if (!enabled()) return std::nullopt;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        for (const CachedTrack& t : readManifest(it->path() / kManifestName)) {
            if (t.id != trackId || t.file.ends_with(kPartSuffix)) continue;
            fs::path path = it->path() / t.file;
            std::error_code fileEc;
            auto size = fs::file_size(path, fileEc); // fails for a missing file and for a folder
            if (!fileEc && size > 0) return path;
        }
    }
    return std::nullopt;
}

} // namespace oap
