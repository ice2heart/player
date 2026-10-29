#include "track_cache.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace oap {

namespace {
constexpr const char* kManifestName = "manifest.tsv";
constexpr const char* kManifestHeader = "oap-cache 2";
constexpr const char* kListTag = "list"; // second line: list, kind, id, name, artist
constexpr size_t kListColumns = 5;
constexpr const char* kChapterTag = "chapter"; // a line of a server book's chapter: chapter, start, title
constexpr size_t kChapterColumns = 3;
constexpr const char* kPartSuffix = ".part";
constexpr size_t kLegacyColumns = 7;
constexpr size_t kColumns = 8;
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

// The cells of a tab-separated line; a trailing tab ends an empty cell.
std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, '\t')) cells.push_back(cell);
    if (!line.empty() && line.back() == '\t') cells.emplace_back();
    return cells;
}

bool parseLine(const std::string& line, CachedTrack& track) {
    const std::vector<std::string> cells = splitTabs(line);
    if (cells.size() != kLegacyColumns && cells.size() != kColumns) return false;
    double duration = 0;
    int64_t lastPlayed = 0;
    if (!unescape(cells[0], track.id) || !unescape(cells[1], track.file) || !unescape(cells[2], track.title) ||
        !parseNumber(cells[3], duration) || !unescape(cells[4], track.changed) ||
        (cells[5] != "0" && cells[5] != "1") || !parseInteger(cells[6], lastPlayed))
        return false;
    track.duration = duration;
    track.pinned = cells[5] == "1";
    track.lastPlayed = lastPlayed;
    track.bitrate = 0;
    if (cells.size() == kColumns) {
        const auto& cell = cells[7];
        auto [end, error] = std::from_chars(cell.data(), cell.data() + cell.size(), track.bitrate);
        if (error != std::errc{} || end != cell.data() + cell.size() || track.bitrate < 0) return false;
    }
    return !track.id.empty() && plainFileName(track.file);
}

const char* kindName(RemoteSource::Kind kind) { return kind == RemoteSource::Kind::Album ? "album" : "playlist"; }

bool parseListLine(const std::string& line, CachedList& list) {
    const std::vector<std::string> cells = splitTabs(line);
    if (cells.size() != kListColumns || cells[0] != kListTag) return false;
    if (cells[1] == "album") list.source.kind = RemoteSource::Kind::Album;
    else if (cells[1] == "playlist") list.source.kind = RemoteSource::Kind::Playlist;
    else return false;
    return unescape(cells[2], list.source.id) && unescape(cells[3], list.name) && unescape(cells[4], list.artist) &&
           !list.source.id.empty();
}

bool parseChapterLine(const std::string& line, RemoteChapter& chapter) {
    const std::vector<std::string> cells = splitTabs(line);
    return cells.size() == kChapterColumns && cells[0] == kChapterTag && parseNumber(cells[1], chapter.start) &&
           unescape(cells[2], chapter.title);
}

// The text of a manifest; a list with an empty id if any line is wrong.
CachedList parseManifest(const std::string& text) {
    CachedList list;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kManifestHeader || !std::getline(in, line) || !parseListLine(line, list))
        return {};
    while (std::getline(in, line)) {
        RemoteChapter chapter; // (a track line has seven or eight cells, so an id of "chapter" is not taken for this)
        if (parseChapterLine(line, chapter)) {
            if (!list.tracks.empty()) return {}; // chapters come before the tracks
            list.chapters.push_back(std::move(chapter));
            continue;
        }
        CachedTrack track;
        if (!parseLine(line, track)) return {};
        list.tracks.push_back(std::move(track));
    }
    return list;
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

TrackCache::TrackCache(TextFiles& files, fs::path root, const std::string& serverUrl, const std::string& user)
    : files_(&files) {
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

CachedList TrackCache::parseFolder(const fs::path& folder, const std::string& text) const {
    CachedList list = parseManifest(text);
    if (list.source.id.empty() || listFolder(list.source).filename() != folder.filename()) return {};
    return list;
}

CachedList TrackCache::readFolder(const fs::path& folder) const {
    auto text = files_->read(folder / kManifestName);
    return text ? parseFolder(folder, *text) : CachedList();
}

CachedList TrackCache::load(const RemoteSource& list) const {
    return enabled() ? readFolder(listFolder(list)) : CachedList();
}

bool TrackCache::save(const CachedList& list) const {
    if (!enabled() || list.source.id.empty()) return false;
    const fs::path folder = listFolder(list.source);
    std::error_code ec;
    // A manifest that is there and was read but is not valid: what it listed is lost, so what it
    // left is cleaned up below. One that could not be read at all (an I/O error, a permission) may be
    // fine: it is not overwritten, and nothing is deleted.
    bool damaged = false;
    if (fs::exists(manifestFile(list.source), ec)) {
        auto old = files_->read(manifestFile(list.source));
        if (!old) return false;
        damaged = parseFolder(folder, *old).source.id.empty();
    }
    std::string text;
    text += kManifestHeader;
    text += '\n';
    text += std::string(kListTag) + '\t' + kindName(list.source.kind) + '\t' + escape(list.source.id) + '\t' +
            escape(list.name) + '\t' + escape(list.artist) + '\n';
    for (const RemoteChapter& c : list.chapters) {
        char start[32];
        std::snprintf(start, sizeof start, "%.3f", c.start);
        text += std::string(kChapterTag) + '\t' + start + '\t' + escape(c.title) + '\n';
    }
    for (const CachedTrack& t : list.tracks) {
        char duration[32];
        std::snprintf(duration, sizeof duration, "%.3f", t.duration);
        text += escape(t.id) + '\t' + escape(t.file) + '\t' + escape(t.title) + '\t' + duration + '\t' +
                escape(t.changed) + '\t' + (t.pinned ? '1' : '0') + '\t' + std::to_string(t.lastPlayed) + '\t' +
                std::to_string(t.bitrate) + '\n';
    }
    if (!files_->writeAtomically(manifestFile(list.source), text)) return false;
    if (damaged) removeUnlisted(folder, list);
    return true;
}

// Deletes the regular files of `folder` that the manifest does not name: not the manifest, not a `.part`.
void TrackCache::removeUnlisted(const fs::path& folder, const CachedList& list) const {
    std::set<std::string> listed{kManifestName};
    for (const CachedTrack& t : list.tracks) listed.insert(t.file);
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        std::string name = it->path().filename().string();
        if (listed.count(name) || name.ends_with(kPartSuffix) || name.ends_with(".tmp")) continue;
        if (it->is_regular_file(entryEc)) fs::remove(it->path(), entryEc); // never follows a link out of the folder
    }
}

namespace {
struct CompleteFile {
    fs::path path;
    std::uintmax_t size;
};
// The file of a manifest entry if it is complete: not a `.part`, present and not empty. One stat.
std::optional<CompleteFile> completeInfo(const fs::path& folder, const CachedTrack& t) {
    if (t.file.ends_with(kPartSuffix)) return std::nullopt;
    fs::path path = folder / t.file;
    std::error_code ec;
    auto size = fs::file_size(path, ec); // fails for a missing file and for a folder
    if (ec || size == 0) return std::nullopt;
    return CompleteFile{std::move(path), size};
}
std::optional<fs::path> completeIn(const fs::path& folder, const CachedTrack& t) {
    if (auto info = completeInfo(folder, t)) return std::move(info->path);
    return std::nullopt;
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

std::vector<CachedList> TrackCache::lists(bool pinnedOnly) const {
    std::vector<CachedList> out;
    if (!enabled()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        CachedList list = readFolder(it->path());
        if (list.source.id.empty()) continue;
        std::erase_if(list.tracks, [&](const CachedTrack& t) { return !completeIn(it->path(), t); });
        bool wanted = pinnedOnly ? std::any_of(list.tracks.begin(), list.tracks.end(),
                                               [](const CachedTrack& t) { return t.pinned; })
                                 : !list.tracks.empty();
        if (wanted) out.push_back(std::move(list));
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

namespace {
bool hasPartFile(const fs::path& folder) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().ends_with(kPartSuffix)) return true;
    return false;
}
} // namespace

size_t TrackCache::trim(std::uintmax_t limitBytes, const std::map<std::string, int64_t>& played,
                        const std::vector<fs::path>& keep, TrimInfo* info) const {
    if (!enabled()) return 0;
    struct Folder {
        fs::path path;
        CachedList list;
        bool changed = false;
    };
    // A complete unpinned track (`track` is its index in the manifest) or a `.part`
    // (`track` == kPart): an abandoned transfer, which is evicted before any track.
    constexpr size_t kPart = std::numeric_limits<size_t>::max();
    struct Candidate {
        size_t folder, track;
        fs::path file;
        std::uintmax_t size;
        int64_t lastPlayed;
        std::string id;
    };
    std::vector<Folder> folders;
    std::vector<Candidate> candidates;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(serverDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        Folder folder{it->path(), readFolder(it->path()), false};
        const size_t first = candidates.size();
        for (size_t i = 0; i < folder.list.tracks.size(); ++i) {
            CachedTrack& t = folder.list.tracks[i];
            // The latest play wins, also over a later time a wrong clock once wrote. A time
            // before kEarliestPlayTime is a clock that was not set: ignored.
            if (auto at = played.find(t.id);
                at != played.end() && at->second >= kEarliestPlayTime && at->second != t.lastPlayed) {
                t.lastPlayed = at->second;
                folder.changed = true;
            }
            if (t.pinned) continue;
            if (auto file = completeInfo(folder.path, t)) {
                candidates.push_back({folders.size(), i, file->path, file->size, t.lastPlayed, t.id});
                total += file->size;
            }
        }
        std::error_code partEc;
        for (fs::directory_iterator part(folder.path, partEc), partEnd; !partEc && part != partEnd; part.increment(partEc)) {
            std::string name = part->path().filename().string();
            std::error_code sizeEc;
            if (!name.ends_with(kPartSuffix) || !part->is_regular_file(sizeEc)) continue;
            auto size = part->file_size(sizeEc);
            if (sizeEc) continue;
            candidates.push_back({folders.size(), kPart, part->path(), size, std::numeric_limits<int64_t>::min(), name});
            total += size;
        }
        // A folder without a valid manifest matters only for the parts in it.
        if (!folder.list.source.id.empty() || candidates.size() > first) folders.push_back(std::move(folder));
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        return std::tuple(a.lastPlayed, folders[a.folder].path.filename().string(), a.id) <
               std::tuple(b.lastPlayed, folders[b.folder].path.filename().string(), b.id);
    });
    std::set<std::pair<size_t, size_t>> gone;
    std::vector<std::pair<std::string, fs::path>> evicted;
    for (const Candidate& c : candidates) {
        if (total <= limitBytes) break;
        if (std::find(keep.begin(), keep.end(), c.file) != keep.end()) continue;
        std::error_code removeEc;
        fs::remove(c.file, removeEc);
        if (fs::exists(c.file, removeEc)) continue; // still there: keep its entry
        total -= c.size;
        folders[c.folder].changed = true;
        if (c.track != kPart) {
            gone.emplace(c.folder, c.track);
            evicted.emplace_back(c.id, c.file);
        }
    }
    for (size_t f = 0; f < folders.size(); ++f) {
        Folder& folder = folders[f];
        if (!folder.changed) continue;
        std::vector<CachedTrack> left;
        for (size_t i = 0; i < folder.list.tracks.size(); ++i)
            if (!gone.count({f, i})) left.push_back(std::move(folder.list.tracks[i]));
        folder.list.tracks = std::move(left);
        if (folder.list.source.id.empty()) {
            std::error_code removeEc;
            fs::remove(folder.path, removeEc); // only parts were in it: empty now, else it stays
        } else if (folder.list.tracks.empty() && !hasPartFile(folder.path)) {
            remove(folder.list.source);
        } else {
            save(folder.list);
        }
    }
    if (info) *info = {std::move(evicted), total};
    return gone.size();
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
