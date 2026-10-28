#include "album_merge.h"

#include "library.h"
#include "library_index.h"
#include "text.h"

#include <algorithm>
#include <numeric>
#include <set>

namespace oap {

namespace {

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x2009 || c == 0x3000; }

// The text in lower case: without spaces, or (keepSpaces) with runs of spaces made one and
// none at either end.
std::string foldedText(const std::string& text, bool keepSpaces) {
    std::string out;
    bool pendingSpace = false;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = nextCodePoint(text, i);
        if (isSpace(c)) {
            pendingSpace = keepSpaces && !out.empty();
        } else {
            if (pendingSpace) out += ' ';
            pendingSpace = false;
            appendUtf8(out, foldCase(c));
        }
    }
    return out;
}

std::string nameKey(const std::string& text) { return foldedText(text, false); }

// A local folder with no tags is named by its folder; a download saved without a name has only
// the placeholder the menus speak, which is not a name to match.
const std::string& albumName(const IndexItem& item) {
    static const std::string none;
    if (!item.album.empty()) return item.album;
    return item.source == IndexItem::Source::Cached ? none : item.title;
}

int rank(IndexItem::Source source) {
    switch (source) {
    case IndexItem::Source::Local: return 0;
    case IndexItem::Source::Cached: return 1;
    case IndexItem::Source::Server: return 2;
    }
    return 3;
}

} // namespace

std::string artistKey(const std::string& name) { return foldedText(name, true); }

std::string artistSortName(const std::string& name) {
    std::string key = artistKey(name);
    for (const char* article : kIgnoredArticles) {
        std::string prefix = std::string(article) + " ";
        if (key.size() > prefix.size() && key.starts_with(prefix)) return key.substr(prefix.size());
    }
    return key;
}

bool artistLess(const std::string& a, const std::string& b) {
    std::string x = artistSortName(a), y = artistSortName(b);
    if (x != y) return naturalLess(x, y);
    return naturalLess(a, b);
}

int trackCountOf(const IndexItem& item) { return item.trackCount > 0 ? item.trackCount : int(item.tracks.size()); }

bool sameAlbumByName(const IndexItem& a, const IndexItem& b) {
    if (!a.musicBrainzId.empty() && a.musicBrainzId == b.musicBrainzId) return true;
    std::string nameA = nameKey(albumName(a));
    if (nameA.empty() || nameA != nameKey(albumName(b))) return false;
    int countA = trackCountOf(a);
    return countA > 0 && countA == trackCountOf(b) && nameKey(a.artist) == nameKey(b.artist);
}

std::vector<MergedAlbum> mergeAlbums(const std::map<std::string, IndexItem>& items) {
    std::vector<const IndexItem*> music;
    for (const auto& [id, item] : items)
        if (item.kind == IndexItem::Kind::Music) music.push_back(&item);

    // Union-find over the items: those that hold one reference are one album.
    std::vector<size_t> parent(music.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto root = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::map<std::string, size_t> holder;
    for (size_t i = 0; i < music.size(); ++i)
        for (const auto& ref : music[i]->seenOn) {
            auto [it, fresh] = holder.emplace(ref, i);
            if (!fresh) parent[root(i)] = root(it->second);
        }

    std::map<size_t, MergedAlbum> groups;
    for (size_t i = 0; i < music.size(); ++i) {
        MergedAlbum& group = groups[root(i)];
        group.items.push_back(music[i]);
        if (music[i]->source == IndexItem::Source::Server) group.onServer = true;
    }
    std::vector<MergedAlbum> out;
    for (auto& [r, group] : groups) {
        std::stable_sort(group.items.begin(), group.items.end(),
                         [](const IndexItem* a, const IndexItem* b) { return rank(a->source) < rank(b->source); });
        group.primary = group.items.front();
        out.push_back(std::move(group));
    }
    std::sort(out.begin(), out.end(), [](const MergedAlbum& a, const MergedAlbum& b) { return a.primary->id < b.primary->id; });
    return out;
}

std::vector<AlbumLink> linkByName(const std::string& serverKey, const std::map<std::string, IndexItem>& items) {
    std::set<std::string> held; // references a local item already holds
    std::vector<const IndexItem*> candidates, serverItems;
    for (const auto& [id, item] : items) {
        if (item.kind != IndexItem::Kind::Music) continue;
        if (item.source == IndexItem::Source::Server) {
            if (LibraryIndex::refOnServer(item.seenOn, serverKey)) serverItems.push_back(&item);
            continue;
        }
        if (item.source == IndexItem::Source::Local)
            for (const auto& ref : item.seenOn) held.insert(ref);
        if (!LibraryIndex::refOnServer(item.seenOn, serverKey)) candidates.push_back(&item);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const IndexItem* a, const IndexItem* b) { return rank(a->source) < rank(b->source); });

    std::vector<AlbumLink> links;
    std::set<const IndexItem*> taken;
    for (const IndexItem* server : serverItems) {
        std::string ref = *LibraryIndex::refOnServer(server->seenOn, serverKey);
        if (held.count(ref)) continue;
        for (const IndexItem* local : candidates) {
            if (taken.count(local) || !sameAlbumByName(*local, *server)) continue;
            taken.insert(local);
            links.push_back({local->id, ref});
            break;
        }
    }
    return links;
}

std::string albumInfoText(const MergedAlbum& album) {
    const IndexItem& item = *album.primary;
    std::string text;
    switch (item.source) {
    case IndexItem::Source::Local: text = "On the device."; break;
    case IndexItem::Source::Cached: text = "Downloaded."; break;
    case IndexItem::Source::Server: text = "On the server."; break;
    }
    if (album.onServer && item.source != IndexItem::Source::Server) text += " Also on the server.";
    if (!item.artist.empty()) text += " " + item.artist + ".";
    int count = trackCountOf(item);
    if (count > 0) text += " " + std::to_string(count) + (count == 1 ? " track." : " tracks.");
    return text;
}

} // namespace oap
