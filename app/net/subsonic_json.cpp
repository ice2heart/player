#include "subsonic_json.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>

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
Parsed<RemoteTrack> parseAlbum(std::string_view body) { return parseList<RemoteTrack>(body, "album", "song", track); }
Parsed<RemotePlaylist> parsePlaylists(std::string_view body) {
    return parseList<RemotePlaylist>(body, "playlists", "playlist", playlist);
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
