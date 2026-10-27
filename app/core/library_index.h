#pragma once

#include "services.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace oap {

// What kind of server object a seenOn reference names.
enum class SeenKind { Album, Playlist, Track };

// The library in memory: every book, album and podcast the device knows, local,
// on a server or downloaded, kept between runs by a LibraryStore. Plain core:
// no file or database access, no clock (callers pass the wall time).
//
// Items are changed in memory and written by save(), which hands the store only
// the items that really changed and the ones that went (flash wear).
//
// seenOn: a server reference is `<server key>:<kind>:<id>` (the key is
// TrackCache::serverKey(), the kind album, playlist or track, the id the
// server's). It holds no URL and no token. An item or track holds each
// reference at most once.
class LibraryIndex {
public:
    // Loads everything from `store`, which must outlive the index.
    explicit LibraryIndex(LibraryStore& store);

    // `<key>:<kind>:<id>`.
    static std::string seenRef(const std::string& serverKey, SeenKind kind, const std::string& id);
    // The first reference of `refs` that belongs to `serverKey` (its key matches exactly).
    static std::optional<std::string> refOnServer(const std::vector<std::string>& refs, const std::string& serverKey);

    // All items, by id.
    const std::map<std::string, IndexItem>& items() const { return items_; }

    // Lookups; null when there is none.
    const IndexItem* find(const std::string& id) const;
    // The item holding a track at `location` (a path, or a server track id), and the track.
    struct TrackHit {
        const IndexItem* item = nullptr;
        const IndexTrack* track = nullptr;
    };
    TrackHit findByLocation(const std::string& location) const;
    // The item that has `ref` in its seenOn.
    const IndexItem* findBySeen(const std::string& ref) const;
    // The track that has `ref` in its seenOn.
    TrackHit trackBySeen(const std::string& ref) const;

    // Adds `item`, or merges it into the item with the same id: the old first-seen time,
    // last-played time (unless the new one is later) and seenOn references are kept, and
    // so are the seenOn of tracks with the same location. A zero firstSeen becomes `now`.
    // The item is saved only if it differs from what it was.
    void upsert(IndexItem item, int64_t now);
    // Removes the item; false if there is none.
    bool remove(const std::string& id);
    // Adds a reference to the item (once); false if the item is unknown.
    bool addSeenOn(const std::string& id, const std::string& ref);

    // A scan of one source: `items` (all of that source) are upserted, and the items of
    // that source that are not among them are removed.
    void replaceSource(IndexItem::Source source, const std::vector<IndexItem>& items, int64_t now);
    // A sync of one server: every reference to `serverKey` is dropped from every item and
    // track, server items left without a reference are removed, then `items` are upserted
    // (they carry their new references). The one exception: a reference of a local or
    // downloaded item that one of `items` carries too stays, so an album that is merged with a
    // server album stays merged when the server renames it.
    void replaceServer(const std::string& serverKey, const std::vector<IndexItem>& items, int64_t now);

    // Writes the changed items and deletes the removed ones; nothing is called when
    // nothing changed. False if the store failed (the changes are kept for the next try).
    bool save();

private:
    void markChanged(const std::string& id);
    void reindex() const;

    LibraryStore* store_;
    std::map<std::string, IndexItem> items_;
    std::set<std::string> changed_;
    std::set<std::string> removed_;

    // Lookup tables, rebuilt on demand after a change.
    mutable bool indexed_ = false;
    mutable std::map<std::string, std::pair<std::string, size_t>> byLocation_; // location -> item id, track index
    mutable std::map<std::string, std::string> bySeen_;                        // ref -> item id
    mutable std::map<std::string, std::pair<std::string, size_t>> trackBySeen_;
};

} // namespace oap
