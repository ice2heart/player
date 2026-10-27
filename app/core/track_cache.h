#pragma once

#include "library.h"

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

// An album or a playlist in the cache: what it is, what to call it, and its tracks.
struct CachedList {
    RemoteSource source; // album or playlist, and the server's id
    std::string name;    // what the list is called, spoken in Downloaded
    std::string artist;  // an album's artist, may be empty
    std::vector<CachedTrack> tracks;
};

// Tracks kept on disk for one server: <root>/<server key>/<kind>%3A<list id>/,
// where a list is an album or a playlist (the kind keeps an album and a
// playlist with the same id apart). Each list folder has its audio files and a
// `manifest.tsv`, written atomically, that also holds the list's kind, id, name
// and artist. Manifest format 2; a format 1 manifest (before lists had names,
// never shipped) is read as empty. Plain `std::filesystem`: the cache needs no
// clock and no network.
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
    std::filesystem::path listFolder(const RemoteSource& list) const;       // empty when not enabled
    std::filesystem::path manifestFile(const RemoteSource& list) const;

    // The manifest of a list. A missing, unreadable or corrupt manifest gives a
    // list without tracks (and without a name): it is never an error and never partly read.
    CachedList load(const RemoteSource& list) const;
    // Replaces the manifest (all-or-nothing: the old one stays if this fails). False on failure.
    bool save(const CachedList& list) const;

    // The lists with a readable manifest, in the order of their names (letter
    // case ignored), then kind and id. Tracks whose file is not complete are
    // left out of them, and so are lists with no complete track. A manifest
    // that does not belong to the folder it is in (a copied folder) is skipped.
    // With `pinnedOnly`, only the lists with at least one complete pinned track:
    // what the user downloaded, not what the automatic cache happens to hold.
    std::vector<CachedList> lists(bool pinnedOnly = false) const;

    // Deletes the list's folder with everything in it. Never touches anything
    // outside this server's folder. True if the folder is gone afterwards.
    bool remove(const RemoteSource& list) const;

    // The complete cached file of a track, whichever list it is in; none when
    // it is not cached, its file is missing or empty, or it is a `.part` file.
    std::optional<std::filesystem::path> find(const std::string& trackId) const;

    // The file of a manifest entry of `list` if it is complete (the rules of find()).
    std::optional<std::filesystem::path> completeFile(const RemoteSource& list, const CachedTrack& track) const;

    // Maintenance of the automatic cache, in one pass over the manifests. First
    // `played` (track id -> wall time it played) is written into the tracks'
    // `lastPlayed` where it is later than what the manifest has. Then, while the
    // complete files of unpinned tracks add up to more than `limitBytes`, the
    // least recently played of them (ties: list folder, then track id) are
    // deleted with their manifest entries. Pinned tracks never go and do not
    // count; a file in `keep` (the one the player has open) is not deleted. A list
    // left with no tracks loses its folder, unless a `.part` is in it (a transfer
    // that may be resumed), then only its manifest is emptied. Returns the number
    // of tracks deleted. Manifests are rewritten only where something changed.
    size_t trim(std::uintmax_t limitBytes, const std::map<std::string, int64_t>& played,
                const std::vector<std::filesystem::path>& keep) const;

    // Every track find() would return, by track id, from one pass over the
    // manifests: for a caller that asks often and keeps the answer in memory.
    std::map<std::string, std::filesystem::path> index() const;

private:
    // The manifest in a list folder of this server; empty if it is not valid or is another list's (a copied folder).
    CachedList readFolder(const std::filesystem::path& folder) const;

    std::filesystem::path serverDir_;
};

} // namespace oap
