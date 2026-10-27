#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oap {

// One downloaded track in a list's manifest. A track is in the manifest only
// once its file is complete (a download writes `*.part` and renames it first).
struct CachedTrack {
    std::string id;           // the server's track id
    std::string file;         // file name inside the list's folder, never a path
    std::string title;
    double duration = 0;      // seconds
    std::string changed;      // the server's `changed` time of the list when it was downloaded
    bool pinned = false;      // a download the user asked for; the automatic cache evicts only unpinned tracks
    int64_t lastPlayed = 0;   // wall time (seconds since 1970) it last played, 0: never; for eviction
};

// Tracks kept on disk for one server: <root>/<server key>/<list id>/, where a
// list is an album or a playlist. Each list folder has its audio files and a
// `manifest.tsv`, written atomically. Plain `std::filesystem`: the cache needs
// no clock and no network.
class TrackCache {
public:
    // `serverUrl` and `user` give the folder name; an empty URL makes a cache that stores and finds nothing.
    TrackCache(std::filesystem::path root, const std::string& serverUrl, const std::string& user);

    // A folder name for a server: a readable host part and a hash of the URL
    // (without a trailing slash or any login in it) and the user name. It
    // holds no password, token or stream URL. Empty when `serverUrl` is empty.
    static std::string serverKey(const std::string& serverUrl, const std::string& user);

    // A server's id as one folder or file name: anything but letters, digits
    // `.`, `-` and `_` becomes %XX, and a name that is only dots is encoded too.
    static std::string safeName(const std::string& id);

    bool enabled() const { return !serverDir_.empty(); }
    const std::filesystem::path& serverFolder() const { return serverDir_; } // empty when not enabled
    std::filesystem::path listFolder(const std::string& listId) const;       // empty when not enabled
    std::filesystem::path manifestFile(const std::string& listId) const;

    // The manifest of a list. A missing, unreadable or corrupt manifest is
    // empty: it is never an error and never partly read.
    std::vector<CachedTrack> load(const std::string& listId) const;
    // Replaces the manifest (all-or-nothing: the old one stays if this fails). False on failure.
    bool save(const std::string& listId, const std::vector<CachedTrack>& tracks) const;

    // The complete cached file of a track, whichever list it is in; none when
    // it is not cached, its file is missing or empty, or it is a `.part` file.
    std::optional<std::filesystem::path> find(const std::string& trackId) const;

    // The file of a manifest entry of `listId` if it is complete (the rules of find()).
    std::optional<std::filesystem::path> completeFile(const std::string& listId, const CachedTrack& track) const;

    // Every track find() would return, by track id, from one pass over the
    // manifests: for a caller that asks often and keeps the answer in memory.
    std::map<std::string, std::filesystem::path> index() const;

private:
    std::filesystem::path serverDir_;
};

} // namespace oap
