#pragma once

#include "services.h"

#include <map>
#include <string>
#include <vector>

namespace oap {

// One album as the user sees it (docs/streaming.md 4.8): the index items that are the same
// album are one entry, played from the preferred source. Plain core: no files, no clock.
//
// What the index stores is only the references: a local or downloaded item holds the
// `seenOn` reference of the server album it is the same as, and the server item holds its own
// id as its reference. Items that share a reference are one album. The merged view is
// computed from that whenever a menu is built.
struct MergedAlbum {
    const IndexItem* primary = nullptr;  // local, else downloaded (cached), else server
    std::vector<const IndexItem*> items; // every item of the album, primary first
    bool onServer = false;               // one of the items is a server item
};

// The music items of `items` grouped by shared references (items with no reference are
// alone), in the order of the primary item's id.
std::vector<MergedAlbum> mergeAlbums(const std::map<std::string, IndexItem>& items);

// Artists. Two names are the same artist when they differ only in case and spacing. An artist
// sorts by its name without a leading article ("The Band" under B): the server's
// `ignoredArticles` are not read by the client yet, so this is the default, "The".
inline constexpr const char* kIgnoredArticles[] = {"the"};
std::string artistKey(const std::string& name);
bool artistLess(const std::string& a, const std::string& b); // natural, case-insensitive, articles ignored

// The name rule: two albums are the same when they have the same MusicBrainz album id, or
// when artist, album and track count are equal with case and spaces ignored. An empty
// MusicBrainz id, an empty album or an unknown track count never match.
bool sameAlbumByName(const IndexItem& a, const IndexItem& b);

// Tracks of an item: a server item's `trackCount`, else the tracks it holds.
int trackCountOf(const IndexItem& item);

// `reference` is the seenOn reference of a server album that `itemId` is the same as.
struct AlbumLink {
    std::string itemId;
    std::string reference;
};

// Which local or downloaded items of `items` are the server albums of `items` (the server
// items with a reference on `serverKey`), by the name rule. A server album that a non-server item already holds the
// reference of, and a non-server item that already has a reference to this server, take no
// part: references are matched first (by the index) and the name rule is for items with
// none. An item matches at most one server album and a server album at most one item;
// local items go before downloaded ones, then by id.
std::vector<AlbumLink> linkByName(const std::string& serverKey, const std::map<std::string, IndexItem>& items);

// What Album info says: where the album plays from ("On the device", "Downloaded", "On the
// server"), "Also on the server" for a merged entry, then the artist and the track count.
std::string albumInfoText(const MergedAlbum& album);

} // namespace oap
