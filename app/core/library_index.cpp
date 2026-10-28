#include "library_index.h"

#include <algorithm>
#include <set>

#include "track_cache.h"

namespace oap {

namespace {

const char* kindName(SeenKind kind) {
    switch (kind) {
    case SeenKind::Album: return "album";
    case SeenKind::Playlist: return "playlist";
    case SeenKind::Track: return "track";
    }
    return "";
}

// A server key never holds ':', so the key is what comes before the first one.
bool onServer(const std::string& ref, const std::string& serverKey) {
    return !serverKey.empty() && ref.size() > serverKey.size() && ref[serverKey.size()] == ':' &&
           ref.compare(0, serverKey.size(), serverKey) == 0;
}

void addOnce(std::vector<std::string>& refs, const std::string& ref) {
    if (std::find(refs.begin(), refs.end(), ref) == refs.end()) refs.push_back(ref);
}

// Returns true if anything was removed.
// References in `keep` stay.
bool dropServer(std::vector<std::string>& refs, const std::string& serverKey, const std::set<std::string>* keep = nullptr) {
    auto gone = std::remove_if(refs.begin(), refs.end(),
                               [&](const std::string& r) { return onServer(r, serverKey) && !(keep && keep->count(r)); });
    bool any = gone != refs.end();
    refs.erase(gone, refs.end());
    return any;
}

} // namespace

LibraryIndex::LibraryIndex(LibraryStore& store) : store_(&store) {
    for (auto& item : store.load()) {
        std::string id = item.id;
        items_[id] = std::move(item);
    }
    for (auto& playlist : store.loadPlaylists()) {
        std::string ref = seenRef(playlist.server, SeenKind::Playlist, playlist.id);
        playlists_[ref] = std::move(playlist);
    }
}

std::string LibraryIndex::seenRef(const std::string& serverKey, SeenKind kind, const std::string& id) {
    return serverKey + ":" + kindName(kind) + ":" + id;
}

std::optional<std::string> LibraryIndex::refOnServer(const std::vector<std::string>& refs, const std::string& serverKey) {
    for (const auto& ref : refs)
        if (onServer(ref, serverKey)) return ref;
    return std::nullopt;
}

void LibraryIndex::reindex() const {
    if (indexed_) return;
    byLocation_.clear();
    bySeen_.clear();
    trackBySeen_.clear();
    for (const auto& [id, item] : items_) {
        for (const auto& ref : item.seenOn) bySeen_.emplace(ref, id);
        for (size_t i = 0; i < item.tracks.size(); ++i) {
            const IndexTrack& track = item.tracks[i];
            if (!track.location.empty()) byLocation_.emplace(track.location, std::make_pair(id, i));
            for (const auto& ref : track.seenOn) trackBySeen_.emplace(ref, std::make_pair(id, i));
        }
    }
    indexed_ = true;
}

const IndexItem* LibraryIndex::find(const std::string& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

LibraryIndex::TrackHit LibraryIndex::findByLocation(const std::string& location) const {
    reindex();
    auto it = byLocation_.find(location);
    if (it == byLocation_.end()) return {};
    const IndexItem& item = items_.at(it->second.first);
    return {&item, &item.tracks[it->second.second]};
}

const IndexItem* LibraryIndex::findBySeen(const std::string& ref) const {
    reindex();
    auto it = bySeen_.find(ref);
    return it == bySeen_.end() ? nullptr : &items_.at(it->second);
}

LibraryIndex::TrackHit LibraryIndex::trackBySeen(const std::string& ref) const {
    reindex();
    auto it = trackBySeen_.find(ref);
    if (it == trackBySeen_.end()) return {};
    const IndexItem& item = items_.at(it->second.first);
    return {&item, &item.tracks[it->second.second]};
}

void LibraryIndex::markChanged(const std::string& id) {
    changed_.insert(id);
    removed_.erase(id);
    indexed_ = false;
}

void LibraryIndex::upsert(IndexItem item, int64_t now) {
    // A clock that was never set (before kEarliestPlayTime) is no time: firstSeen stays 0 and a
    // later upsert, with a real clock, fills it in. Else the item would never be "recently added".
    if (item.firstSeen == 0 && now >= kEarliestPlayTime) item.firstSeen = now;
    auto old = items_.find(item.id);
    if (old != items_.end()) {
        const IndexItem& before = old->second;
        if (before.firstSeen) item.firstSeen = before.firstSeen;
        item.lastPlayed = std::max(item.lastPlayed, before.lastPlayed);
        for (const auto& ref : before.seenOn) addOnce(item.seenOn, ref);
        for (auto& track : item.tracks) {
            auto same = std::find_if(before.tracks.begin(), before.tracks.end(),
                                     [&](const IndexTrack& t) { return t.location == track.location; });
            if (same == before.tracks.end()) continue;
            for (const auto& ref : same->seenOn) addOnce(track.seenOn, ref);
        }
        if (item == before) return;
    }
    std::string id = item.id;
    items_[id] = std::move(item);
    markChanged(id);
}

bool LibraryIndex::remove(const std::string& id) {
    if (items_.erase(id) == 0) return false;
    changed_.erase(id);
    removed_.insert(id);
    indexed_ = false;
    return true;
}

bool LibraryIndex::addSeenOn(const std::string& id, const std::string& ref) {
    auto it = items_.find(id);
    if (it == items_.end()) return false;
    auto& refs = it->second.seenOn;
    if (std::find(refs.begin(), refs.end(), ref) != refs.end()) return true;
    refs.push_back(ref);
    markChanged(id);
    return true;
}

void LibraryIndex::replaceSource(IndexItem::Source source, const std::vector<IndexItem>& items, int64_t now) {
    std::set<std::string> keep;
    for (const auto& item : items) keep.insert(item.id);
    std::vector<std::string> gone;
    for (const auto& [id, item] : items_)
        if (item.source == source && !keep.count(id)) gone.push_back(id);
    for (const auto& id : gone) remove(id);
    for (const auto& item : items) upsert(item, now);
}

void LibraryIndex::replaceServer(const std::string& serverKey, const std::vector<IndexItem>& items, int64_t now) {
    std::set<std::string> incoming, incomingRefs;
    for (const auto& item : items) {
        incoming.insert(item.id);
        incomingRefs.insert(item.seenOn.begin(), item.seenOn.end());
    }
    // What the touched items were, so an item that comes back the same is not written again.
    std::map<std::string, IndexItem> before;
    std::vector<std::string> gone;
    for (auto& [id, item] : items_) {
        IndexItem copy = item;
        // A local or downloaded item keeps a reference the server still has: matched by
        // reference first, so a renamed album is still the same entry.
        bool touched = item.source == IndexItem::Source::Server ? dropServer(item.seenOn, serverKey)
                                                                : dropServer(item.seenOn, serverKey, &incomingRefs);
        for (auto& track : item.tracks) touched = dropServer(track.seenOn, serverKey) || touched;
        if (!touched) continue;
        before.emplace(id, std::move(copy));
        if (item.source == IndexItem::Source::Server && item.seenOn.empty() && !incoming.count(id)) gone.push_back(id);
    }
    for (const auto& id : gone) {
        before.erase(id);
        remove(id);
    }
    std::set<std::string> alreadyChanged;
    for (const auto& [id, item] : before)
        if (changed_.count(id)) alreadyChanged.insert(id);
    for (const auto& [id, item] : before) markChanged(id);
    for (const auto& item : items) upsert(item, now);
    for (const auto& [id, item] : before)
        if (!alreadyChanged.count(id) && items_.at(id) == item) changed_.erase(id);
}

bool LibraryIndex::markPlayed(const std::string& id, int64_t when) {
    auto it = items_.find(id);
    if (it == items_.end()) return false;
    if (when > it->second.lastPlayed) {
        it->second.lastPlayed = when;
        played_.insert(id);
    }
    return true;
}

bool LibraryIndex::savePlayTimes() {
    for (const auto& id : played_)
        if (items_.count(id)) changed_.insert(id);
    return save(); // on failure the play times stay marked for the next try
}

void LibraryIndex::replacePlaylists(const std::string& serverKey, const std::vector<IndexPlaylist>& playlists) {
    std::map<std::string, IndexPlaylist> next;
    for (const auto& [ref, playlist] : playlists_)
        if (playlist.server != serverKey) next.emplace(ref, playlist);
    for (IndexPlaylist playlist : playlists) {
        playlist.server = serverKey;
        std::string ref = seenRef(serverKey, SeenKind::Playlist, playlist.id);
        next[ref] = std::move(playlist);
    }
    if (next == playlists_) return;
    playlists_ = std::move(next);
    playlistsChanged_ = true;
}

void LibraryIndex::addPlaylist(IndexPlaylist playlist) {
    std::string ref = seenRef(playlist.server, SeenKind::Playlist, playlist.id);
    auto it = playlists_.find(ref);
    if (it != playlists_.end() && it->second == playlist) return;
    playlists_[ref] = std::move(playlist);
    playlistsChanged_ = true;
}

std::set<std::string> LibraryIndex::playlistServers() const {
    std::set<std::string> servers;
    for (const auto& [ref, playlist] : playlists_) servers.insert(playlist.server);
    return servers;
}

bool LibraryIndex::saveItems() {
    if (changed_.empty() && removed_.empty()) return true;
    std::vector<IndexItem> changed;
    for (const auto& id : changed_) changed.push_back(items_.at(id));
    std::vector<std::string> removed(removed_.begin(), removed_.end());
    if (!store_->save(changed, removed)) return false;
    for (const auto& id : changed_) played_.erase(id); // written whole, play time and all
    changed_.clear();
    removed_.clear();
    return true;
}

bool LibraryIndex::savePlaylists() {
    if (!playlistsChanged_) return true;
    std::vector<IndexPlaylist> all;
    for (const auto& [ref, playlist] : playlists_) all.push_back(playlist);
    if (!store_->savePlaylists(all)) return false;
    playlistsChanged_ = false;
    return true;
}

bool LibraryIndex::save() {
    bool items = saveItems();
    bool playlists = savePlaylists();
    return items && playlists;
}

} // namespace oap
