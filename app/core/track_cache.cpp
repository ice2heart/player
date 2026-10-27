#include "track_cache.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace oap {

namespace {
constexpr const char* kManifestName = "manifest.tsv";
constexpr const char* kManifestHeader = "oap-cache 2";
constexpr const char* kListTag = "list"; // second line: list, kind, id, name, artist
constexpr size_t kListColumns = 5;
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

const char* kindName(RemoteSource::Kind kind) { return kind == RemoteSource::Kind::Album ? "album" : "playlist"; }

bool parseListLine(const std::string& line, CachedList& list) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, '\t')) cells.push_back(cell);
    if (!line.empty() && line.back() == '\t') cells.emplace_back();
    if (cells.size() != kListColumns || cells[0] != kListTag) return false;
    if (cells[1] == "album") list.source.kind = RemoteSource::Kind::Album;
    else if (cells[1] == "playlist") list.source.kind = RemoteSource::Kind::Playlist;
    else return false;
    return unescape(cells[2], list.source.id) && unescape(cells[3], list.name) && unescape(cells[4], list.artist) &&
           !list.source.id.empty();
}

// A manifest file; a list with an empty id if it is missing or any line is wrong.
CachedList readManifest(const fs::path& file) {
    CachedList list;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line != kManifestHeader || !std::getline(in, line) ||
        !parseListLine(line, list))
        return {};
    while (std::getline(in, line)) {
        CachedTrack track;
        if (!parseLine(line, track)) return {};
        list.tracks.push_back(std::move(track));
    }
    if (in.bad()) return {};
    return list;
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

fs::path TrackCache::listFolder(const RemoteSource& list) const {
    // The kind and ":" make the name differ for an album and a playlist with one id.
    return enabled() ? serverDir_ / safeName(std::string(kindName(list.kind)) + ":" + list.id) : fs::path();
}

fs::path TrackCache::manifestFile(const RemoteSource& list) const {
    return enabled() ? listFolder(list) / kManifestName : fs::path();
}

CachedList TrackCache::readFolder(const fs::path& folder) const {
    CachedList list = readManifest(folder / kManifestName);
    if (list.source.id.empty() || listFolder(list.source).filename() != folder.filename()) return {};
    return list;
}

CachedList TrackCache::load(const RemoteSource& list) const {
    return enabled() ? readFolder(listFolder(list)) : CachedList();
}

bool TrackCache::save(const CachedList& list) const {
    if (!enabled() || list.source.id.empty()) return false;
    const std::vector<CachedTrack>& tracks = list.tracks;
    std::error_code ec;
    fs::create_directories(listFolder(list.source), ec);
    if (ec) return false;
    fs::path file = manifestFile(list.source);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << kManifestHeader << '\n';
        out << kListTag << '\t' << kindName(list.source.kind) << '\t' << escape(list.source.id) << '\t'
            << escape(list.name) << '\t' << escape(list.artist) << '\n';
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

namespace {
// The path of a manifest entry if its file is complete: not a `.part`, present and not empty.
std::optional<fs::path> completeIn(const fs::path& folder, const CachedTrack& t) {
    if (t.file.ends_with(kPartSuffix)) return std::nullopt;
    fs::path path = folder / t.file;
    std::error_code ec;
    auto size = fs::file_size(path, ec); // fails for a missing file and for a folder
    if (ec || size == 0) return std::nullopt;
    return path;
}
} // namespace

std::optional<fs::path> TrackCache::find(const std::string& trackId) const {
    if (!enabled()) return std::nullopt;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        for (const CachedTrack& t : readFolder(it->path()).tracks)
            if (t.id == trackId)
                if (auto path = completeIn(it->path(), t)) return path;
    }
    return std::nullopt;
}

std::optional<fs::path> TrackCache::completeFile(const RemoteSource& list, const CachedTrack& track) const {
    if (!enabled()) return std::nullopt;
    return completeIn(listFolder(list), track);
}

std::vector<CachedList> TrackCache::lists() const {
    std::vector<CachedList> out;
    if (!enabled()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        CachedList list = readFolder(it->path());
        if (list.source.id.empty()) continue;
        std::erase_if(list.tracks, [&](const CachedTrack& t) { return !completeIn(it->path(), t); });
        if (!list.tracks.empty()) out.push_back(std::move(list));
    }
    std::sort(out.begin(), out.end(), [](const CachedList& a, const CachedList& b) {
        return std::tuple(lowerCase(a.name), a.source.kind, a.source.id) <
               std::tuple(lowerCase(b.name), b.source.kind, b.source.id);
    });
    return out;
}

bool TrackCache::remove(const RemoteSource& list) const {
    fs::path folder = listFolder(list);
    // listFolder() is always one name below the server's folder; check anyway before deleting a tree.
    if (folder.empty() || folder.filename().empty() || folder.parent_path() != serverDir_) return false;
    std::error_code ec;
    fs::remove_all(folder, ec);
    return !fs::exists(folder, ec);
}

std::map<std::string, fs::path> TrackCache::index() const {
    std::map<std::string, fs::path> out;
    if (!enabled()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        for (const CachedTrack& t : readFolder(it->path()).tracks)
            if (auto path = completeIn(it->path(), t)) out.emplace(t.id, *path);
    }
    return out;
}

} // namespace oap
