#include "core/album_merge.h"
#include "core/library_index.h"

#include <gtest/gtest.h>

using namespace oap;

namespace {

const std::string kKey = "music_example_com-0123456789abcdef";
const std::string kOther = "other_example_org-fedcba9876543210";

std::string ref(const std::string& id, const std::string& key = kKey) { return LibraryIndex::seenRef(key, SeenKind::Album, id); }

IndexItem local(const std::string& id, const std::string& artist, const std::string& album, int tracks) {
    IndexItem item;
    item.id = "local:album:" + id;
    item.source = IndexItem::Source::Local;
    item.title = album;
    item.album = album;
    item.artist = artist;
    for (int i = 0; i < tracks; ++i) item.tracks.push_back({"/music/" + id + "/" + std::to_string(i) + ".mp3", "T", artist});
    return item;
}

IndexItem served(const std::string& id, const std::string& artist, const std::string& album, int count,
                 const std::string& key = kKey) {
    IndexItem item;
    item.id = ref(id, key);
    item.source = IndexItem::Source::Server;
    item.title = album;
    item.album = album;
    item.artist = artist;
    item.trackCount = count;
    item.seenOn = {item.id};
    return item;
}

IndexItem downloaded(const std::string& id, const std::string& artist, const std::string& album, int tracks) {
    IndexItem item = local(id, artist, album, tracks);
    item.id = "cached:" + ref(id);
    item.source = IndexItem::Source::Cached;
    item.seenOn = {ref(id)};
    return item;
}

std::map<std::string, IndexItem> indexOf(std::vector<IndexItem> items) {
    std::map<std::string, IndexItem> out;
    for (auto& item : items) out[item.id] = std::move(item);
    return out;
}

} // namespace

TEST(SameAlbumByName, EqualArtistAlbumAndTrackCountMatchIgnoringCaseAndSpaces) {
    EXPECT_TRUE(sameAlbumByName(local("a", "The Band", "Road Trip", 2), served("1", "the  band", "ROAD TRIP", 2)));
    EXPECT_TRUE(sameAlbumByName(local("a", "Группа", "Дорога", 3), served("1", "ГРУППА", "дорога", 3)));
    EXPECT_TRUE(sameAlbumByName(local("a", "Café", "Été", 3), served("1", "CAFÉ", "été", 3)));
    EXPECT_TRUE(sameAlbumByName(local("a", "Zespół", "Łódź", 3), served("1", "ZESPÓŁ", "ŁÓDŹ", 3)));
    EXPECT_TRUE(sameAlbumByName(local("a", "Ελληνικά", "Άλμπουμ", 3), served("1", "ΕΛΛΗΝΙΚΆ", "άλμπουμ", 3)));
}

TEST(SameAlbumByName, AnotherTrackCountArtistOrAlbumDoesNotMatch) {
    IndexItem a = local("a", "Band", "Road Trip", 2);
    EXPECT_FALSE(sameAlbumByName(a, served("1", "Band", "Road Trip", 3)));
    EXPECT_FALSE(sameAlbumByName(a, served("1", "Other", "Road Trip", 2)));
    EXPECT_FALSE(sameAlbumByName(a, served("1", "Band", "Road Trips", 2)));
}

TEST(SameAlbumByName, UnknownTrackCountAndEmptyNamesNeverMatchByName) {
    EXPECT_FALSE(sameAlbumByName(local("a", "Band", "Road Trip", 2), served("1", "Band", "Road Trip", 0)));
    EXPECT_FALSE(sameAlbumByName(local("a", "Band", "", 2), served("1", "Band", "", 2)));
}

TEST(SameAlbumByName, AMatchingMusicBrainzIdMatchesDifferentSpellings) {
    IndexItem a = local("a", "Band", "Road Trip", 2);
    IndexItem b = served("1", "The Band", "Roadtrip (Deluxe)", 17);
    EXPECT_FALSE(sameAlbumByName(a, b));
    a.musicBrainzId = b.musicBrainzId = "9b3a";
    EXPECT_TRUE(sameAlbumByName(a, b));
    b.musicBrainzId = "other";
    EXPECT_FALSE(sameAlbumByName(a, b));
}

TEST(SameAlbumByName, ADownloadWithNoNameIsNotMatchedByThePlaceholderTitle) {
    IndexItem d = downloaded("1", "Band", "", 2);
    d.title = "Album";
    EXPECT_FALSE(sameAlbumByName(d, served("2", "Band", "Album", 2)));
    IndexItem folder = local("f", "Band", "", 2); // untagged folder: named by the folder
    folder.title = "Mix";
    EXPECT_TRUE(sameAlbumByName(folder, served("3", "Band", "Mix", 2)));
}

TEST(SameAlbumByName, TwoEmptyMusicBrainzIdsAreNotAMatch) {
    EXPECT_FALSE(sameAlbumByName(local("a", "Band", "One", 2), served("1", "Band", "Two", 2)));
}

TEST(MergeAlbums, ItemsSharingAReferenceAreOneEntryWithTheLocalOnePreferred) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    l.seenOn = {ref("1")};
    auto items = indexOf({served("1", "Band", "Road Trip", 2), l, served("2", "Band", "Other", 5)});
    auto merged = mergeAlbums(items); // points into `items`
    ASSERT_EQ(merged.size(), 2u);
    const MergedAlbum* both = merged[0].items.size() == 2 ? &merged[0] : &merged[1];
    EXPECT_EQ(both->primary->id, l.id);
    EXPECT_TRUE(both->onServer);
    EXPECT_EQ(both->items.size(), 2u);
    const MergedAlbum* only = merged[0].items.size() == 2 ? &merged[1] : &merged[0];
    EXPECT_EQ(only->primary->source, IndexItem::Source::Server);
    EXPECT_EQ(only->items.size(), 1u);
}

TEST(MergeAlbums, ADownloadIsPreferredToTheServerAndALocalCopyToBoth) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    l.seenOn = {ref("1")};
    auto items = indexOf({served("1", "Band", "Road Trip", 2), downloaded("1", "Band", "Road Trip", 2)});
    auto two = mergeAlbums(items);
    ASSERT_EQ(two.size(), 1u);
    EXPECT_EQ(two[0].primary->source, IndexItem::Source::Cached);
    EXPECT_TRUE(two[0].onServer);
    items[l.id] = l;
    auto three = mergeAlbums(items);
    ASSERT_EQ(three.size(), 1u);
    EXPECT_EQ(three[0].primary->source, IndexItem::Source::Local);
    EXPECT_EQ(three[0].items.size(), 3u);
}

TEST(MergeAlbums, ADownloadWhoseServerAlbumIsGoneIsNotOnTheServer) {
    auto items = indexOf({downloaded("1", "Band", "Road Trip", 2)});
    auto merged = mergeAlbums(items);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_FALSE(merged[0].onServer);
}

TEST(MergeAlbums, AlbumsWithTheSameNameAndNoReferenceStayApart) {
    auto merged = mergeAlbums(indexOf({local("a", "Band", "Road Trip", 2), served("1", "Band", "Road Trip", 2)}));
    EXPECT_EQ(merged.size(), 2u) << "merging by name is done when the index is built, and is stored as a reference";
}

TEST(MergeAlbums, BooksAreLeftOut) {
    IndexItem book = local("b", "Author", "Book", 1);
    book.kind = IndexItem::Kind::Book;
    EXPECT_TRUE(mergeAlbums(indexOf({book})).empty());
}

TEST(MergeAlbums, ReferencesToAnotherServerMergeToo) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    l.seenOn = {ref("1", kOther)};
    auto merged = mergeAlbums(indexOf({l, served("1", "Band", "Road Trip", 2, kOther), served("1", "Band", "Road Trip", 2)}));
    EXPECT_EQ(merged.size(), 2u);
}

TEST(LinkByName, ALocalAlbumGetsTheReferenceOfTheServerAlbumItIs) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    auto links = linkByName(kKey, indexOf({l, served("1", "band", "road trip", 2), served("2", "Band", "Other", 2)}));
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].itemId, l.id);
    EXPECT_EQ(links[0].reference, ref("1"));
}

TEST(LinkByName, ReferencesAreMatchedFirstSoAnItemWithOneIsNotRenamedByName) {
    IndexItem held = local("a", "Band", "Old name", 2);
    held.seenOn = {ref("1")};
    IndexItem twin = local("b", "Band", "Same As The Server", 2);
    // Server album 1 was renamed to what `twin` is called; `held` already has its reference.
    auto links = linkByName(kKey, indexOf({held, twin, served("1", "Band", "Same As The Server", 2)}));
    EXPECT_TRUE(links.empty()) << "the server album belongs to the item that holds its reference";
}

TEST(LinkByName, AnItemThatHasAReferenceToThisServerIsNotNameMatchedAgain) {
    IndexItem held = local("a", "Band", "Road Trip", 2);
    held.seenOn = {ref("stale")};
    auto links = linkByName(kKey, indexOf({held, served("1", "Band", "Road Trip", 2)}));
    EXPECT_TRUE(links.empty());
}

TEST(LinkByName, ALocalAlbumIsStillLinkedToAServerAlbumThatOnlyADownloadHolds) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    auto links = linkByName(kKey, indexOf({l, downloaded("1", "Band", "Road Trip", 2), served("1", "Band", "Road Trip", 2)}));
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].itemId, l.id);
    EXPECT_EQ(links[0].reference, ref("1"));
}

TEST(LinkByName, AReferenceToAnotherServerDoesNotStopTheNameRule) {
    IndexItem l = local("a", "Band", "Road Trip", 2);
    l.seenOn = {ref("9", kOther)};
    auto links = linkByName(kKey, indexOf({l, served("1", "Band", "Road Trip", 2)}));
    ASSERT_EQ(links.size(), 1u);
}

TEST(LinkByName, OneItemMatchesOneServerAlbumAndTheLocalOneComesFirst) {
    IndexItem l = local("z", "Band", "Road Trip", 2);
    IndexItem d = downloaded("9", "Band", "Road Trip", 2);
    d.seenOn.clear();
    auto links = linkByName(kKey, indexOf({d, l, served("1", "Band", "Road Trip", 2), served("2", "Band", "Road Trip", 2)}));
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0].itemId, l.id) << "the first server album goes to the local item";
    EXPECT_EQ(links[1].itemId, d.id);
    EXPECT_NE(links[0].reference, links[1].reference);
}

TEST(LinkByName, ServerAlbumsOfAnotherKeyAreNotLinked) {
    auto links = linkByName(kKey, indexOf({local("a", "Band", "Road Trip", 2), served("1", "Band", "Road Trip", 2, kOther)}));
    EXPECT_TRUE(links.empty());
}

TEST(AlbumInfoText, NamesTheSourceTheArtistAndTheTrackCount) {
    MergedAlbum onDevice;
    IndexItem l = local("a", "Band", "Road Trip", 2);
    onDevice.primary = &l;
    onDevice.items = {&l};
    EXPECT_EQ(albumInfoText(onDevice), "On the device. Band. 2 tracks.");

    IndexItem d = downloaded("1", "Band", "Weather", 1);
    MergedAlbum dl;
    dl.primary = &d;
    dl.items = {&d};
    EXPECT_EQ(albumInfoText(dl), "Downloaded. Band. 1 track.");

    IndexItem s = served("1", "Band", "Weather", 12);
    MergedAlbum remote;
    remote.primary = &s;
    remote.items = {&s};
    remote.onServer = true;
    EXPECT_EQ(albumInfoText(remote), "On the server. Band. 12 tracks.");

    onDevice.onServer = true;
    EXPECT_EQ(albumInfoText(onDevice), "On the device. Also on the server. Band. 2 tracks.");
    dl.onServer = true;
    EXPECT_EQ(albumInfoText(dl), "Downloaded. Also on the server. Band. 1 track.");
}

TEST(AlbumInfoText, LeavesOutWhatIsNotKnown) {
    IndexItem l = local("a", "", "Road Trip", 0);
    MergedAlbum m;
    m.primary = &l;
    m.items = {&l};
    EXPECT_EQ(albumInfoText(m), "On the device.");
}

TEST(ArtistNames, TheSameArtistDiffersOnlyInCaseAndSpacing) {
    EXPECT_EQ(artistKey("Pink  Floyd "), artistKey("pink floyd"));
    EXPECT_EQ(artistKey("БИ-2"), artistKey("би-2"));
    EXPECT_NE(artistKey("Pink Floyd"), artistKey("PinkFloyd"));
    EXPECT_NE(artistKey("The Band"), artistKey("Band"));
    EXPECT_EQ(artistKey("  "), "");
}

TEST(ArtistNames, ArtistsSortWithoutALeadingThe) {
    EXPECT_TRUE(artistLess("ABBA", "The Beatles"));
    EXPECT_TRUE(artistLess("The Beatles", "Cher"));
    EXPECT_FALSE(artistLess("the Beatles", "Band")) << "Beatles after Band, not under T";
    EXPECT_TRUE(artistLess("Theodor", "Thin Lizzy")) << "only the word The is ignored";
    EXPECT_TRUE(artistLess("Zed", "Агата Кристи")) << "Latin before Cyrillic";
    EXPECT_TRUE(artistLess("Агата Кристи", "би-2")) << "Cyrillic ignores case";
    EXPECT_FALSE(artistLess("Band", "Band"));
    EXPECT_NE(artistLess("The Band", "Band"), artistLess("Band", "The Band")) << "equal sort names still have an order";
}
