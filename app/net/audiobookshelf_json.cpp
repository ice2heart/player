#include "audiobookshelf_json.h"

#include "net/subsonic_json.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>

namespace oap::audiobookshelf {

namespace {

constexpr const char* kAppName = "audiobookshelf";
constexpr const char* kTypeName = "Audiobookshelf";
constexpr const char* kBookMediaType = "book";
constexpr char kIdSeparator = '/'; // "<item id>/<ino>"
constexpr long long kMillisPerSecond = 1000, kSecondsPerDay = 86400, kSecondsPerHour = 3600, kSecondsPerMinute = 60;
constexpr long long kDaysToEpochFromCivilEra = 719468; // days from 0000-03-01 to 1970-01-01
constexpr long long kDaysPerEra = 146097;               // 400 years

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

template <class F> void forEach(const YAML::Node& list, F f) {
    if (list.IsSequence())
        for (const auto& item : list) f(item);
}

// The document, or Null when the body is not JSON. Sets `error` to Server then.
YAML::Node open(std::string_view body, Error& error) {
    try {
        YAML::Node doc = YAML::Load(subsonic::jsonForYaml(body));
        if (doc.IsMap()) return doc;
    } catch (const YAML::Exception&) {
    }
    error = Error::Server;
    return YAML::Node();
}

RemoteAlbum book(const YAML::Node& node) {
    const YAML::Node metadata = child(child(node, "media"), "metadata");
    RemoteAlbum album;
    album.id = text(node, "id");
    album.name = text(metadata, "title");
    album.artist = text(metadata, "authorName");
    album.songCount = integer(child(node, "media"), "numAudioFiles");
    const YAML::Node genres = child(metadata, "genres");
    if (genres.IsSequence() && genres.size() > 0 && genres[0].IsScalar()) album.genre = genres[0].Scalar();
    album.year = integer(metadata, "publishedYear");
    album.created = isoTime(static_cast<long long>(number(node, "addedAt")));
    return album;
}

// Opens the answer and reads the objects of one array with `item`.
template <class T, class Item> Parsed<T> parseArray(std::string_view body, const char* list, Item item, int* total = nullptr) {
    Parsed<T> parsed;
    YAML::Node doc = open(body, parsed.error);
    if (parsed.error != Error::None) return parsed;
    try {
        forEach(child(doc, list), [&](const YAML::Node& node) { parsed.items.push_back(item(node)); });
        if (total) *total = integer(doc, "total");
    } catch (const YAML::Exception&) {
        parsed = {};
        parsed.error = Error::Server;
    }
    return parsed;
}

} // namespace

PingResult parseStatus(std::string_view body) {
    PingResult result;
    YAML::Node doc = open(body, result.error);
    if (result.error != Error::None) return result;
    if (text(doc, "app") != kAppName) { // some other web page that answers JSON
        result.error = Error::Server;
        return result;
    }
    result.info.type = kTypeName;
    result.info.version = text(doc, "serverVersion");
    return result;
}

Error parseMe(std::string_view body) {
    Error error = Error::None;
    YAML::Node doc = open(body, error);
    if (error == Error::None && text(doc, "id").empty()) error = Error::Server;
    return error;
}

Parsed<Library> parseLibraries(std::string_view body) {
    struct Entry {
        Library library;
        int order;
    };
    auto parsed = parseArray<Entry>(body, "libraries", [](const YAML::Node& node) {
        return Entry{{text(node, "id"), text(node, "name")}, text(node, "mediaType") == kBookMediaType ? integer(node, "displayOrder") : -1};
    });
    Parsed<Library> result;
    result.error = parsed.error;
    std::vector<Entry>& entries = parsed.items;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.order < 0; }), entries.end());
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
    for (Entry& entry : entries) result.items.push_back(std::move(entry.library));
    return result;
}

Parsed<RemoteAlbum> parseBookList(std::string_view body, int offset, int size) {
    int total = 0;
    auto parsed = parseArray<RemoteAlbum>(body, "results", book, &total);
    parsed.more = parsed.error == Error::None && offset + size < total;
    return parsed;
}

Parsed<RemoteArtist> parseAuthors(std::string_view body) {
    return parseArray<RemoteArtist>(body, "authors", [](const YAML::Node& node) {
        RemoteArtist artist;
        artist.id = text(node, "id");
        artist.name = text(node, "name");
        artist.albumCount = integer(node, "numBooks");
        return artist;
    });
}

Parsed<RemoteAlbum> parseAuthorBooks(std::string_view body) { return parseArray<RemoteAlbum>(body, "libraryItems", book); }

BookResult parseBook(std::string_view body) {
    BookResult result;
    YAML::Node doc = open(body, result.error);
    if (result.error != Error::None) return result;
    try {
        const std::string itemId = text(doc, "id");
        const YAML::Node media = child(doc, "media");
        if (itemId.empty() || !media.IsMap()) {
            result.error = Error::Server;
            return result;
        }
        const RemoteAlbum album = book(doc);
        forEach(child(media, "tracks"), [&](const YAML::Node& node) {
            RemoteTrack track;
            const YAML::Node file = child(node, "metadata");
            std::string name = text(file, "filename");
            const std::string ext = text(file, "ext"); // ".mp3"
            if (!ext.empty() && name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                name.resize(name.size() - ext.size());
            track.id = itemId + kIdSeparator + text(node, "ino");
            track.title = name;
            track.artist = album.artist;
            track.album = album.name;
            track.albumId = itemId;
            track.duration = number(node, "duration");
            track.track = integer(node, "index");
            track.suffix = ext.empty() ? ext : ext.substr(1);
            track.size = static_cast<long long>(number(file, "size"));
            result.book.tracks.push_back(std::move(track));
        });
        forEach(child(media, "chapters"), [&](const YAML::Node& node) {
            result.book.chapters.push_back({text(node, "title"), number(node, "start")});
        });
    } catch (const YAML::Exception&) {
        result = {};
        result.error = Error::Server;
    }
    return result;
}

// Days since 1970 to a civil date (Howard Hinnant's algorithm), so no OS time functions are needed.
std::string isoTime(long long millis) {
    if (millis <= 0) return {};
    const long long seconds = millis / kMillisPerSecond;
    long long days = seconds / kSecondsPerDay + kDaysToEpochFromCivilEra;
    const long long rest = seconds % kSecondsPerDay;
    const long long era = days / kDaysPerEra;
    const long long dayOfEra = days - era * kDaysPerEra;
    const long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const long long dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const long long monthShifted = (5 * dayOfYear + 2) / 153; // March is 0
    const long long day = dayOfYear - (153 * monthShifted + 2) / 5 + 1;
    const long long month = monthShifted < 10 ? monthShifted + 3 : monthShifted - 9;
    const long long year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    char out[32];
    std::snprintf(out, sizeof out, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", year, month, day, rest / kSecondsPerHour,
                  rest % kSecondsPerHour / kSecondsPerMinute, rest % kSecondsPerMinute);
    return out;
}

} // namespace oap::audiobookshelf
