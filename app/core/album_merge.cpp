#include "album_merge.h"

#include "library.h"
#include "library_index.h"

#include <algorithm>
#include <numeric>
#include <set>

namespace oap {

namespace {

// Lower case for what albums are named in: ASCII, Latin-1 and Extended-A (Polish, Czech,
// Turkish...), Greek and Cyrillic. Other scripts are compared as they are.
uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if ((c >= 0x100 && c <= 0x137 && c != 0x130 && c != 0x131) || (c >= 0x14A && c <= 0x177)) return c % 2 == 0 ? c + 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c % 2 == 1 ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x2009 || c == 0x3000; }

void append(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// The text in lower case: without spaces, or (keepSpaces) with runs of spaces made one and
// none at either end.
std::string foldedText(const std::string& text, bool keepSpaces) {
    std::string out;
    bool pendingSpace = false;
    for (size_t i = 0; i < text.size();) {
        unsigned char b = text[i];
        size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + n > text.size()) n = 1;
        uint32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
        if (n > 1) {
            bool ok = true;
            for (size_t k = 1; k < n; ++k) {
                unsigned char cb = text[i + k];
                if ((cb & 0xC0) != 0x80) ok = false;
                c = (c << 6) | (cb & 0x3F);
            }
            if (!ok) {
                c = b;
                n = 1;
            }
        }
        i += n;
        if (isSpace(c)) {
            pendingSpace = keepSpaces && !out.empty();
        } else {
            if (pendingSpace) out += ' ';
            pendingSpace = false;
            append(out, fold(c));
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

bool artistLess(const std::string& a, const std::string& b) {
    auto sortName = [](const std::string& name) {
        std::string key = artistKey(name);
        for (const char* article : kIgnoredArticles) {
            std::string prefix = std::string(article) + " ";
            if (key.size() > prefix.size() && key.starts_with(prefix)) return key.substr(prefix.size());
        }
        return key;
    };
    std::string x = sortName(a), y = sortName(b);
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
