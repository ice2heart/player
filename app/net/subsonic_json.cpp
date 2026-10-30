#include "subsonic_json.h"
#include "core/text.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>

namespace oap::subsonic {

namespace {

constexpr unsigned kHighSurrogateFirst = 0xD800, kHighSurrogateLast = 0xDBFF;
constexpr unsigned kLowSurrogateFirst = 0xDC00, kLowSurrogateLast = 0xDFFF;
constexpr unsigned kFirstSupplementary = 0x10000;
constexpr int kSurrogateBits = 10;
constexpr std::size_t kEscapeLength = 6; // \uXXXX

// The four hex digits after "\u" at `pos`, or -1.
long hex4(std::string_view s, std::size_t pos) {
    if (pos + 4 > s.size()) return -1;
    long value = 0;
    for (std::size_t i = pos; i < pos + 4; ++i) {
        char c = s[i];
        int digit = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

bool isSurrogate(long unit) { return unit >= long(kHighSurrogateFirst) && unit <= long(kLowSurrogateLast); }

// Null when the key is missing or `node` is not a mapping.
YAML::Node child(const YAML::Node& node, const char* key) {
    if (!node.IsDefined() || !node.IsMap()) return YAML::Node();
    const YAML::Node& map = node;
    YAML::Node value = map[key];
    return value.IsDefined() ? value : YAML::Node();
}

// Empty for a missing key or a JSON null (yaml-cpp would give "null").
std::string text(const YAML::Node& node, const char* key) {
    YAML::Node value = child(node, key);
    return value.IsScalar() ? value.Scalar() : std::string();
}
int integer(const YAML::Node& node, const char* key) { return child(node, key).as<int>(0); }
double number(const YAML::Node& node, const char* key) { return child(node, key).as<double>(0); }

// Subsonic lists are arrays; a converted XML answer may hold one item as a plain object.
template <class F> void forEach(const YAML::Node& list, F f) {
    if (list.IsSequence())
        for (const auto& item : list) f(item);
    else if (list.IsMap())
        f(list);
}

struct Envelope {
    Error error = Error::Server;
    YAML::Node response; // "subsonic-response", when error is None
};

Envelope open(std::string_view body) {
    Envelope envelope;
    try {
        const YAML::Node doc = YAML::Load(jsonForYaml(body));
        YAML::Node response = child(doc, "subsonic-response"); // Null for an HTML page or other JSON
        if (text(response, "status") == "ok") {
            envelope.error = Error::None;
            envelope.response = response;
        } else if (integer(child(response, "error"), "code") == kWrongCredentials) {
            envelope.error = Error::BadLogin;
        }
    } catch (const YAML::Exception&) {
        envelope.error = Error::Server;
    }
    return envelope;
}

// Opens the answer and reads one list out of it with `item`.
template <class T, class Item>
Parsed<T> parseList(std::string_view body, const char* container, const char* list, Item item) {
    Parsed<T> parsed;
    Envelope envelope = open(body);
    parsed.error = envelope.error;
    if (envelope.error != Error::None) return parsed;
    try {
        forEach(child(child(envelope.response, container), list), [&](const YAML::Node& node) {
            parsed.items.push_back(item(node));
        });
    } catch (const YAML::Exception&) {
        parsed = {};
        parsed.error = Error::Server;
    }
    return parsed;
}

RemoteAlbum album(const YAML::Node& node) {
    RemoteAlbum album;
    album.id = text(node, "id");
    album.name = text(node, "name");
    if (album.name.empty()) album.name = text(node, "title");
    album.artist = text(node, "artist");
    album.songCount = integer(node, "songCount");
    album.genre = text(node, "genre");
    album.year = integer(node, "year");
    album.created = text(node, "created");
    album.musicBrainzId = text(node, "musicBrainzId");
    return album;
}

RemoteTrack track(const YAML::Node& node) {
    RemoteTrack track;
    track.id = text(node, "id");
    track.title = text(node, "title");
    track.artist = text(node, "artist");
    track.album = text(node, "album");
    track.albumId = text(node, "albumId");
    track.starred = !text(node, "starred").empty();
    track.duration = number(node, "duration");
    track.track = integer(node, "track");
    track.suffix = text(node, "suffix");
    track.size = static_cast<long long>(number(node, "size"));
    return track;
}

RemoteArtist artist(const YAML::Node& node) {
    RemoteArtist artist;
    artist.id = text(node, "id");
    artist.name = text(node, "name");
    artist.albumCount = integer(node, "albumCount");
    return artist;
}

RemoteFolder musicFolder(const YAML::Node& node) {
    RemoteFolder folder;
    folder.id = text(node, "id");
    folder.name = text(node, "name");
    return folder;
}

RemotePlaylist playlist(const YAML::Node& node) {
    RemotePlaylist playlist;
    playlist.id = text(node, "id");
    playlist.name = text(node, "name");
    playlist.songCount = integer(node, "songCount");
    playlist.changed = text(node, "changed");
    return playlist;
}

} // namespace

std::string jsonForYaml(std::string_view json) {
    std::string out;
    out.reserve(json.size());
    for (std::size_t i = 0; i < json.size();) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            out += json[i++];
            continue;
        }
        long unit = json[i + 1] == 'u' ? hex4(json, i + 2) : -1;
        if (!isSurrogate(unit)) { // any other escape, including "\\", passes as it is
            out.append(json.substr(i, 2));
            i += 2;
            continue;
        }
        std::size_t next = i + kEscapeLength;
        long low = json.substr(next, 2) == "\\u" ? hex4(json, next + 2) : -1;
        if (unit <= long(kHighSurrogateLast) && low >= long(kLowSurrogateFirst) && low <= long(kLowSurrogateLast)) {
            unsigned long codePoint = kFirstSupplementary + ((unsigned long)(unit - kHighSurrogateFirst) << kSurrogateBits) +
                                      (unsigned long)(low - kLowSurrogateFirst);
            char escape[11];
            std::snprintf(escape, sizeof escape, "\\U%08lX", codePoint);
            out += escape;
            i = next + kEscapeLength;
        } else {
            out += "\\uFFFD";
            i = next;
        }
    }
    return out;
}

PingResult parsePing(std::string_view body) {
    PingResult result;
    Envelope envelope = open(body);
    result.error = envelope.error;
    if (envelope.error != Error::None) return result;
    try {
        const YAML::Node& response = envelope.response;
        result.info.type = text(response, "type"); // OpenSubsonic: "navidrome", ...
        if (result.info.type.empty()) result.info.type = "subsonic";
        // Navidrome says "0.64.2 (a1b2c3d)": keep the version, drop the build.
        std::string version = text(response, "serverVersion");
        result.info.version = version.empty() ? text(response, "version") : version.substr(0, version.find(' '));
    } catch (const YAML::Exception&) {
        result = {};
        result.error = Error::Server;
    }
    return result;
}

Error parseStatus(std::string_view body) { return open(body).error; }

Parsed<Bookmark> parseBookmarks(std::string_view body) {
    Parsed<Bookmark> parsed;
    auto envelope = open(body);
    parsed.error = envelope.error;
    if (parsed.error != Error::None) return parsed;
    forEach(child(child(envelope.response, "bookmarks"), "bookmark"), [&](const YAML::Node& node) {
        try {
            const auto id = text(child(node, "entry"), "id");
            const auto stamp = text(node, "changed");
            // A full ISO timestamp with a timezone is required; parseIsoTime also
            // accepts bare dates and normalises invalid calendar days.
            if (stamp.size() < 20 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T' ||
                stamp[13] != ':' || stamp[16] != ':') return;
            for (auto i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18})
                if (!std::isdigit(static_cast<unsigned char>(stamp[std::size_t(i)]))) return;
            const auto year = std::stoi(stamp.substr(0, 4));
            const auto month = std::stoi(stamp.substr(5, 2));
            const auto day = std::stoi(stamp.substr(8, 2));
            if (!std::chrono::year_month_day{std::chrono::year{year}, std::chrono::month{unsigned(month)}, std::chrono::day{unsigned(day)}}.ok()) return;
            std::size_t zone = 19;
            if (stamp[zone] == '.') {
                ++zone;
                const auto begin = zone;
                while (zone < stamp.size() && std::isdigit(static_cast<unsigned char>(stamp[zone]))) ++zone;
                if (zone == begin) return;
            }
            if (zone == stamp.size() || (stamp[zone] != 'Z' && stamp[zone] != '+' && stamp[zone] != '-')) return;
            if (stamp[zone] == 'Z') {
                if (zone + 1 != stamp.size()) return;
            } else {
                const std::string offset = stamp.substr(zone + 1);
                if (offset.size() != 5 || offset[2] != ':' ||
                    !std::isdigit(static_cast<unsigned char>(offset[0])) || !std::isdigit(static_cast<unsigned char>(offset[1])) ||
                    !std::isdigit(static_cast<unsigned char>(offset[3])) || !std::isdigit(static_cast<unsigned char>(offset[4])) ||
                    std::stoi(offset.substr(0, 2)) > 23 || std::stoi(offset.substr(3, 2)) > 59) return;
            }
            const auto seconds = parseIsoTime(stamp);
            const auto position = child(node, "position").as<std::int64_t>();
            if (id.empty() || seconds <= 0 || position < 0) return;
            std::int64_t fraction = 0;
            auto dot = stamp.find('.');
            if (dot != std::string::npos) {
                int scale = 100;
                for (std::size_t i = dot + 1; i < stamp.size() && scale && std::isdigit(static_cast<unsigned char>(stamp[i])); ++i, scale /= 10)
                    fraction += (stamp[i] - '0') * scale;
            }
            if (seconds > (std::numeric_limits<std::int64_t>::max() - fraction) / 1000) return;
            parsed.items.push_back({id, position, seconds * 1000 + fraction});
        } catch (const YAML::Exception&) { /* Ignore an invalid bookmark, not its neighbours. */ }
    });
    return parsed;
}

Parsed<RemoteAlbum> parseAlbumList(std::string_view body, int pageSize) {
    auto parsed = parseList<RemoteAlbum>(body, "albumList2", "album", album);
    parsed.more = parsed.error == Error::None && pageSize > 0 && int(parsed.items.size()) >= pageSize;
    return parsed;
}

VersionResult parseCatalogueVersion(std::string_view body) {
    VersionResult result;
    Envelope envelope = open(body);
    result.error = envelope.error;
    if (envelope.error != Error::None) return result;
    try {
        result.version = text(child(envelope.response, "artists"), "lastModified");
    } catch (const YAML::Exception&) {
        result = {};
        result.error = Error::Server;
    }
    return result;
}

Parsed<RemoteArtist> parseArtists(std::string_view body) {
    Parsed<RemoteArtist> parsed;
    auto indexes = parseList<YAML::Node>(body, "artists", "index", [](const YAML::Node& node) { return node; });
    parsed.error = indexes.error;
    try {
        for (const auto& index : indexes.items)
            forEach(child(index, "artist"), [&](const YAML::Node& node) { parsed.items.push_back(artist(node)); });
    } catch (const YAML::Exception&) {
        parsed = {};
        parsed.error = Error::Server;
    }
    return parsed;
}

Parsed<RemoteFolder> parseMusicFolders(std::string_view body) {
    return parseList<RemoteFolder>(body, "musicFolders", "musicFolder", musicFolder);
}
Parsed<RemoteAlbum> parseArtist(std::string_view body) { return parseList<RemoteAlbum>(body, "artist", "album", album); }
Parsed<RemoteTrack> parseStarred(std::string_view body) {
    return parseList<RemoteTrack>(body, "starred2", "song", track);
}
Parsed<RemoteTrack> parseSong(std::string_view body) {
    Parsed<RemoteTrack> parsed;
    Envelope envelope = open(body);
    parsed.error = envelope.error;
    if (parsed.error != Error::None) return parsed;
    try {
        RemoteTrack song = track(child(envelope.response, "song"));
        if (song.id.empty()) parsed.error = Error::Server;
        else parsed.items.push_back(std::move(song));
    } catch (const YAML::Exception&) { parsed.error = Error::Server; }
    return parsed;
}
Parsed<RemoteTrack> parseAlbum(std::string_view body) { return parseList<RemoteTrack>(body, "album", "song", track); }
Parsed<RemotePlaylist> parsePlaylists(std::string_view body) {
    return parseList<RemotePlaylist>(body, "playlists", "playlist", playlist);
}
CreatedPlaylist parseCreatedPlaylist(std::string_view body) {
    CreatedPlaylist result;
    Envelope envelope = open(body);
    result.error = envelope.error;
    if (envelope.error == Error::None) result.id = text(child(envelope.response, "playlist"), "id");
    return result;
}
Parsed<RemoteTrack> parsePlaylist(std::string_view body) { return parseList<RemoteTrack>(body, "playlist", "entry", track); }

Parsed<std::string> parseExtensions(std::string_view body) {
    Parsed<std::string> parsed;
    Envelope envelope = open(body);
    parsed.error = envelope.error;
    if (envelope.error != Error::None) return parsed;
    try {
        forEach(child(envelope.response, "openSubsonicExtensions"),
                [&](const YAML::Node& node) { parsed.items.push_back(text(node, "name")); });
    } catch (const YAML::Exception&) {
        parsed = {};
        parsed.error = Error::Server;
    }
    return parsed;
}

} // namespace oap::subsonic
