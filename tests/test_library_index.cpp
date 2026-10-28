#include "core/library_index.h"
#include "fakes.h"

#include <gtest/gtest.h>

using namespace oap;
using namespace oap::test;

namespace {

const std::string kKeyA = "music_example_com-0123456789abcdef";
const std::string kKeyB = "other_example_org-fedcba9876543210";

IndexTrack track(const std::string& location, const std::string& title, int number = 1) {
    IndexTrack t;
    t.location = location;
    t.title = title;
    t.number = number;
    t.duration = 200.5;
    t.size = 4096;
    t.fileTime = 1700000000;
    return t;
}

IndexItem album(const std::string& id, const std::string& title, IndexItem::Source source = IndexItem::Source::Local) {
    IndexItem item;
    item.id = id;
    item.source = source;
    item.title = title;
    item.artist = "An Artist";
    item.album = title;
    item.genre = "Jazz";
    item.year = 1999;
    item.musicBrainzId = "mbid-" + id;
    item.tracks = {track("/music/" + id + "/1.mp3", "One", 1), track("/music/" + id + "/2.mp3", "Two", 2)};
    return item;
}

constexpr int64_t kNow = 1800000000;

// An item as the store holds it after a scan with a real clock (firstSeen is set).
IndexItem stored(const std::string& id, const std::string& title, IndexItem::Source source = IndexItem::Source::Local) {
    IndexItem item = album(id, title, source);
    item.firstSeen = kNow;
    return item;
}

} // namespace

TEST(LibraryIndex, UnchangedIndexSavesNothing) {
    FakeLibraryStore store;
    store.items["a"] = stored("a", "Alpha");
    LibraryIndex index(store);
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.saves, 0);

    index.upsert(album("a", "Alpha"), kNow); // the same item again
    index.replaceSource(IndexItem::Source::Local, {album("a", "Alpha")}, kNow);
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.saves, 0);
}

TEST(LibraryIndex, LoadsWhatTheStoreHolds) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    LibraryIndex index(store);
    ASSERT_NE(index.find("a"), nullptr);
    EXPECT_EQ(index.find("a")->title, "Alpha");
    EXPECT_EQ(index.find("a")->tracks.size(), 2u);
    EXPECT_EQ(index.find("missing"), nullptr);
}

TEST(LibraryIndex, OneChangedItemSavesOnlyThatItem) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    store.items["b"] = album("b", "Beta");
    store.items["c"] = album("c", "Gamma");
    LibraryIndex index(store);

    IndexItem changed = album("b", "Beta");
    changed.year = 2001;
    index.upsert(changed, kNow);
    EXPECT_TRUE(index.save());

    EXPECT_EQ(store.saves, 1);
    EXPECT_EQ(store.savedIds, std::vector<std::string>{"b"});
    EXPECT_TRUE(store.removedIds.empty());
    EXPECT_EQ(store.items["b"].year, 2001);

    EXPECT_TRUE(index.save()); // nothing left to write
    EXPECT_EQ(store.saves, 1);
}

TEST(LibraryIndex, NewItemGetsFirstSeenAndOldKeepsIt) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    EXPECT_EQ(index.find("a")->firstSeen, kNow);

    IndexItem again = album("a", "Alpha (remaster)");
    index.upsert(again, kNow + 500);
    EXPECT_EQ(index.find("a")->firstSeen, kNow);
    EXPECT_EQ(index.find("a")->title, "Alpha (remaster)");
}

TEST(LibraryIndex, FirstSeenOfAnUnsetClockIsFilledInByALaterUpsert) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), 40); // 1970 plus uptime: no RTC, no NTP yet
    EXPECT_EQ(index.find("a")->firstSeen, 0);

    index.upsert(album("a", "Alpha"), kNow); // the same item, the clock is set now
    EXPECT_EQ(index.find("a")->firstSeen, kNow);

    index.upsert(album("a", "Alpha"), kNow + 500);
    EXPECT_EQ(index.find("a")->firstSeen, kNow); // and from then on it stays
}

TEST(LibraryIndex, UpsertKeepsLastPlayedUnlessLater) {
    FakeLibraryStore store;
    IndexItem a = album("a", "Alpha");
    a.lastPlayed = 1750000000;
    store.items["a"] = a;
    LibraryIndex index(store);

    index.upsert(album("a", "Alpha"), kNow); // a scan knows no play time
    EXPECT_EQ(index.find("a")->lastPlayed, 1750000000);

    IndexItem later = album("a", "Alpha");
    later.lastPlayed = 1760000000;
    index.upsert(later, kNow);
    EXPECT_EQ(index.find("a")->lastPlayed, 1760000000);
}

TEST(LibraryIndex, RemovedItemIsRemovedFromTheStore) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    store.items["b"] = album("b", "Beta");
    LibraryIndex index(store);

    EXPECT_TRUE(index.remove("a"));
    EXPECT_FALSE(index.remove("a"));
    EXPECT_EQ(index.find("a"), nullptr);
    EXPECT_TRUE(index.save());

    EXPECT_EQ(store.removedIds, std::vector<std::string>{"a"});
    EXPECT_EQ(store.items.count("a"), 0u);
    EXPECT_EQ(store.items.count("b"), 1u);
    EXPECT_TRUE(store.savedIds.empty());
}

TEST(LibraryIndex, ItemAddedAndRemovedBeforeSavingIsNotWritten) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    index.remove("a");
    index.save();
    EXPECT_TRUE(store.savedIds.empty());
    EXPECT_TRUE(store.items.empty());
}

TEST(LibraryIndex, RemovedThenAddedAgainIsWrittenNotDeleted) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    LibraryIndex index(store);
    index.remove("a");
    index.upsert(album("a", "Alpha 2"), kNow);
    index.save();
    EXPECT_EQ(store.items.count("a"), 1u);
    EXPECT_EQ(store.items["a"].title, "Alpha 2");
}

TEST(LibraryIndex, FailedSaveIsTriedAgain) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    store.failSaves = true;
    EXPECT_FALSE(index.save());
    store.failSaves = false;
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.items.count("a"), 1u);
}

TEST(LibraryIndex, FindsATrackByItsPath) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    store.items["b"] = album("b", "Beta");
    LibraryIndex index(store);

    auto hit = index.findByLocation("/music/b/2.mp3");
    ASSERT_NE(hit.item, nullptr);
    ASSERT_NE(hit.track, nullptr);
    EXPECT_EQ(hit.item->id, "b");
    EXPECT_EQ(hit.track->title, "Two");
    EXPECT_EQ(index.findByLocation("/music/b/3.mp3").item, nullptr);

    index.remove("b"); // lookups follow changes
    EXPECT_EQ(index.findByLocation("/music/b/2.mp3").item, nullptr);
    EXPECT_NE(index.findByLocation("/music/a/1.mp3").item, nullptr);
}

TEST(LibraryIndex, ItemWithTwoServersIsFoundByEachAndRoundTrips) {
    FakeLibraryStore store;
    IndexItem item = album("a", "Alpha");
    LibraryIndex index(store);
    index.upsert(item, kNow);
    std::string refA = LibraryIndex::seenRef(kKeyA, SeenKind::Album, "3a1f9c");
    std::string refB = LibraryIndex::seenRef(kKeyB, SeenKind::Album, "77");
    EXPECT_EQ(refA, kKeyA + ":album:3a1f9c");
    EXPECT_TRUE(index.addSeenOn("a", refA));
    EXPECT_TRUE(index.addSeenOn("a", refB));
    EXPECT_FALSE(index.addSeenOn("nope", refB));

    IndexItem withTrackRef = *index.find("a");
    withTrackRef.tracks[0].seenOn.push_back(LibraryIndex::seenRef(kKeyA, SeenKind::Track, "t1"));
    withTrackRef.tracks[0].seenOn.push_back(LibraryIndex::seenRef(kKeyB, SeenKind::Track, "u9"));
    index.upsert(withTrackRef, kNow);

    ASSERT_NE(index.findBySeen(refA), nullptr);
    EXPECT_EQ(index.findBySeen(refA)->id, "a");
    ASSERT_NE(index.findBySeen(refB), nullptr);
    EXPECT_EQ(index.findBySeen(refB)->id, "a");
    EXPECT_EQ(index.findBySeen(kKeyA + ":album:other"), nullptr);
    auto track = index.trackBySeen(kKeyB + ":track:u9");
    ASSERT_NE(track.track, nullptr);
    EXPECT_EQ(track.track->location, "/music/a/1.mp3");
    EXPECT_EQ(index.trackBySeen(kKeyA + ":track:t1").track, track.track);

    ASSERT_TRUE(index.save());
    LibraryIndex reloaded(store);
    ASSERT_NE(reloaded.find("a"), nullptr);
    EXPECT_EQ(reloaded.find("a")->seenOn, (std::vector<std::string>{refA, refB}));
    EXPECT_EQ(reloaded.findBySeen(refB)->id, "a");
    EXPECT_EQ(reloaded.trackBySeen(kKeyA + ":track:t1").track->location, "/music/a/1.mp3");
    EXPECT_EQ(*reloaded.find("a"), *index.find("a"));
}

TEST(LibraryIndex, ReferenceIsFoundForItsOwnServerKeyOnly) {
    std::string refA = LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1");
    std::string refB = LibraryIndex::seenRef(kKeyB, SeenKind::Playlist, "2");
    std::vector<std::string> refs{refA, refB};
    EXPECT_EQ(LibraryIndex::refOnServer(refs, kKeyA), refA);
    EXPECT_EQ(LibraryIndex::refOnServer(refs, kKeyB), refB);
    EXPECT_EQ(LibraryIndex::refOnServer(refs, "music_example_com-0123"), std::nullopt); // a prefix of a key
    EXPECT_EQ(LibraryIndex::refOnServer(refs, kKeyA + "x"), std::nullopt);
    EXPECT_EQ(LibraryIndex::refOnServer(refs, ""), std::nullopt);
    EXPECT_EQ(LibraryIndex::refOnServer({}, kKeyA), std::nullopt);
}

TEST(LibraryIndex, AddingTheSameReferenceTwiceKeepsOne) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    index.save();
    std::string ref = LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1");
    EXPECT_TRUE(index.addSeenOn("a", ref));
    EXPECT_TRUE(index.addSeenOn("a", ref));
    EXPECT_EQ(index.find("a")->seenOn, std::vector<std::string>{ref});

    IndexItem again = *index.find("a");
    again.seenOn = {ref, ref};
    again.tracks[0].seenOn = {ref};
    index.upsert(again, kNow);
    IndexItem merged = album("a", "Alpha");
    merged.seenOn = {ref};
    merged.tracks[0].seenOn = {ref};
    index.upsert(merged, kNow); // merging with what is there adds no second copy
    EXPECT_EQ(index.find("a")->seenOn, std::vector<std::string>{ref});
    EXPECT_EQ(index.find("a")->tracks[0].seenOn, std::vector<std::string>{ref});
}

TEST(LibraryIndex, AddingAKnownReferenceSavesNothing) {
    FakeLibraryStore store;
    IndexItem item = album("a", "Alpha");
    item.seenOn = {kKeyA + ":album:1"};
    store.items["a"] = item;
    LibraryIndex index(store);
    index.addSeenOn("a", kKeyA + ":album:1");
    index.save();
    EXPECT_EQ(store.saves, 0);
}

TEST(LibraryIndex, UpsertKeepsReferencesAndTrackReferencesAlreadyThere) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem item = album("a", "Alpha");
    item.seenOn = {kKeyA + ":album:1"};
    item.tracks[1].seenOn = {kKeyA + ":track:t2"};
    index.upsert(item, kNow);

    index.upsert(album("a", "Alpha"), kNow); // a rescan knows no references
    EXPECT_EQ(index.find("a")->seenOn, std::vector<std::string>{kKeyA + ":album:1"});
    EXPECT_EQ(index.find("a")->tracks[1].seenOn, std::vector<std::string>{kKeyA + ":track:t2"});
    EXPECT_TRUE(index.findBySeen(kKeyA + ":album:1") != nullptr);
}

TEST(LibraryIndex, ReplaceSourceRemovesGoneItemsOfThatSourceOnly) {
    FakeLibraryStore store;
    store.items["a"] = stored("a", "Alpha");
    store.items["b"] = stored("b", "Beta");
    store.items["s"] = stored("s", "Served", IndexItem::Source::Server);
    LibraryIndex index(store);

    IndexItem c = album("c", "Gamma");
    index.replaceSource(IndexItem::Source::Local, {album("a", "Alpha"), c}, kNow);
    EXPECT_EQ(index.find("b"), nullptr);
    EXPECT_NE(index.find("c"), nullptr);
    EXPECT_NE(index.find("s"), nullptr);
    index.save();
    EXPECT_EQ(store.savedIds, std::vector<std::string>{"c"});
    EXPECT_EQ(store.removedIds, std::vector<std::string>{"b"});
}

TEST(LibraryIndex, ReplaceServerReplacesOneKeysItems) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    auto served = [](const std::string& key, const std::string& id) {
        IndexItem item = album(key + id, "Served " + id, IndexItem::Source::Server);
        item.seenOn = {LibraryIndex::seenRef(key, SeenKind::Album, id)};
        return item;
    };
    index.replaceServer(kKeyA, {served(kKeyA, "1"), served(kKeyA, "2")}, kNow);
    index.replaceServer(kKeyB, {served(kKeyB, "1")}, kNow);
    IndexItem local = album("loc", "Local");
    local.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1"), LibraryIndex::seenRef(kKeyB, SeenKind::Album, "9")};
    local.tracks[0].seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Track, "t")};
    index.upsert(local, kNow);
    ASSERT_TRUE(index.save());
    store.savedIds.clear();
    store.saves = 0;

    // Server A now has album 2 (unchanged) and 3; album 1 is gone.
    index.replaceServer(kKeyA, {served(kKeyA, "2"), served(kKeyA, "3")}, kNow + 10);

    EXPECT_EQ(index.find(kKeyA + "1"), nullptr);
    EXPECT_NE(index.find(kKeyA + "2"), nullptr);
    EXPECT_NE(index.find(kKeyA + "3"), nullptr);
    EXPECT_NE(index.find(kKeyB + "1"), nullptr); // another server is not touched
    ASSERT_NE(index.find("loc"), nullptr); // a local item stays, without references to this key
    EXPECT_EQ(index.find("loc")->seenOn, std::vector<std::string>{LibraryIndex::seenRef(kKeyB, SeenKind::Album, "9")});
    EXPECT_TRUE(index.find("loc")->tracks[0].seenOn.empty());
    EXPECT_EQ(index.findBySeen(LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")), nullptr);

    ASSERT_TRUE(index.save());
    // Album 2 came back the same and is not written again.
    EXPECT_EQ(store.savedIds, (std::vector<std::string>{"loc", kKeyA + "3"}));
    EXPECT_EQ(store.removedIds.back(), kKeyA + "1");
}

TEST(LibraryIndex, ReplaceServerKeepsItemsThatAreStillOnAnotherServerOrAreNotServerItems) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem both = album("both", "On two", IndexItem::Source::Server);
    both.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1"), LibraryIndex::seenRef(kKeyB, SeenKind::Album, "1")};
    IndexItem downloaded = album("dl", "Downloaded", IndexItem::Source::Cached);
    downloaded.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "2")};
    IndexItem local = album("loc", "Local");
    local.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "3")};
    index.upsert(both, kNow);
    index.upsert(downloaded, kNow);
    index.upsert(local, kNow);

    index.replaceServer(kKeyA, {}, kNow);

    ASSERT_NE(index.find("both"), nullptr);
    EXPECT_EQ(index.find("both")->seenOn, std::vector<std::string>{LibraryIndex::seenRef(kKeyB, SeenKind::Album, "1")});
    EXPECT_NE(index.find("dl"), nullptr);
    EXPECT_TRUE(index.find("dl")->seenOn.empty());
    EXPECT_NE(index.find("loc"), nullptr);
    EXPECT_TRUE(index.find("loc")->seenOn.empty());
}

TEST(LibraryIndex, ReplaceServerDropsTrackReferencesToo) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem local = album("loc", "Local");
    local.tracks[0].seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Track, "t")};
    index.upsert(local, kNow);
    index.save();
    store.saves = 0;

    index.replaceServer(kKeyA, {}, kNow);
    EXPECT_TRUE(index.find("loc")->tracks[0].seenOn.empty());
    EXPECT_EQ(index.trackBySeen(kKeyA + ":track:t").track, nullptr);
    index.save();
    EXPECT_EQ(store.saves, 1);
    EXPECT_TRUE(store.items["loc"].tracks[0].seenOn.empty());
}

TEST(LibraryIndex, ReplaceServerWithTheSameItemsSavesNothing) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem served = album("s", "Served", IndexItem::Source::Server);
    served.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")};
    index.replaceServer(kKeyA, {served}, kNow);
    index.save();
    store.saves = 0;

    index.replaceServer(kKeyA, {served}, kNow + 60);
    index.save();
    EXPECT_EQ(store.saves, 0);
}

TEST(LibraryIndex, TracksWithoutLocationAreNotFoundByLocation) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem item = album("a", "Alpha");
    item.tracks[0].location.clear();
    index.upsert(item, kNow);
    EXPECT_EQ(index.findByLocation("").item, nullptr);
}

TEST(LibraryIndex, ReplaceServerKeepsAnUnsavedChangeOfAnItemThatComesBackTheSame) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem served = album("s", "Served", IndexItem::Source::Server);
    served.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")};
    index.upsert(served, kNow); // not saved yet

    index.replaceServer(kKeyA, {served}, kNow + 60);
    index.save();
    EXPECT_EQ(store.items.count("s"), 1u);
}

TEST(LibraryIndex, ReplaceServerKeepsTheReferenceOfALocalOrDownloadedItemThatTheServerStillHas) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    IndexItem local = album("loc", "Local");
    local.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")};
    IndexItem downloaded = album("dl", "Downloaded", IndexItem::Source::Cached);
    downloaded.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "2"), LibraryIndex::seenRef(kKeyA, SeenKind::Album, "gone")};
    index.upsert(local, kNow);
    index.upsert(downloaded, kNow);
    index.save();
    store.saves = 0;

    // The server renamed album 1 and dropped "gone".
    IndexItem renamed = album(kKeyA + ":album:1", "Renamed", IndexItem::Source::Server);
    renamed.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")};
    IndexItem two = album(kKeyA + ":album:2", "Two", IndexItem::Source::Server);
    two.seenOn = {LibraryIndex::seenRef(kKeyA, SeenKind::Album, "2")};
    index.replaceServer(kKeyA, {renamed, two}, kNow + 5);

    EXPECT_EQ(index.find("loc")->seenOn, std::vector<std::string>{LibraryIndex::seenRef(kKeyA, SeenKind::Album, "1")});
    EXPECT_EQ(index.find("dl")->seenOn, std::vector<std::string>{LibraryIndex::seenRef(kKeyA, SeenKind::Album, "2")});
    index.save();
    EXPECT_EQ(store.items["loc"].seenOn.size(), 1u);
    EXPECT_EQ(store.items["dl"].seenOn.size(), 1u) << "only the reference the server dropped is gone";
}

TEST(LibraryIndex, MarkPlayedIsKeptInMemoryAndSaveDoesNotWriteIt) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    LibraryIndex index(store);
    EXPECT_TRUE(index.markPlayed("a", kNow));
    EXPECT_EQ(index.find("a")->lastPlayed, kNow);
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.saves, 0);
    EXPECT_FALSE(index.markPlayed("nope", kNow));
}

TEST(LibraryIndex, SavePlayTimesWritesOnlyThePlayedItemsOnceAndAnEarlierTimeDoesNotLowerIt) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    store.items["b"] = album("b", "Beta");
    LibraryIndex index(store);
    index.markPlayed("a", kNow);
    index.markPlayed("a", kNow - 10);
    EXPECT_EQ(index.find("a")->lastPlayed, kNow);
    EXPECT_TRUE(index.savePlayTimes());
    EXPECT_EQ(store.savedIds, std::vector<std::string>{"a"});
    EXPECT_EQ(store.items.at("a").lastPlayed, kNow);
    EXPECT_TRUE(index.savePlayTimes());
    EXPECT_EQ(store.saves, 1) << "written once";
}

TEST(LibraryIndex, APlayTimeWrittenWithAnItemThatChangedIsNotWrittenAgain) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    LibraryIndex index(store);
    index.markPlayed("a", kNow);
    index.upsert(album("a", "Renamed"), kNow); // changed for another reason: written whole
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.items.at("a").lastPlayed, kNow);
    EXPECT_TRUE(index.savePlayTimes());
    EXPECT_EQ(store.saves, 1);
}

TEST(LibraryIndex, AFailedPlayTimeSaveIsRetried) {
    FakeLibraryStore store;
    store.items["a"] = album("a", "Alpha");
    LibraryIndex index(store);
    index.markPlayed("a", kNow);
    store.failSaves = true;
    EXPECT_FALSE(index.savePlayTimes());
    store.failSaves = false;
    EXPECT_TRUE(index.savePlayTimes());
    EXPECT_EQ(store.items.at("a").lastPlayed, kNow);
}

TEST(LibraryIndex, ARescanOfTheSameItemKeepsTheMarkedPlayTimeAndWritesNothing) {
    FakeLibraryStore store;
    store.items["a"] = stored("a", "Alpha");
    LibraryIndex index(store);
    index.markPlayed("a", kNow);
    index.replaceSource(IndexItem::Source::Local, {album("a", "Alpha")}, kNow);
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.saves, 0);
    EXPECT_EQ(index.find("a")->lastPlayed, kNow);
}

// ---- Playlists (docs/fixes.md 2.4)

TEST(LibraryIndexPlaylists, AreLoadedFromTheStoreByServerAndId) {
    FakeLibraryStore store;
    store.playlists = {{kKeyA, "p1", "Road Trip", 12, 5}, {kKeyB, "p1", "Same id", 1, 6}};
    LibraryIndex index(store);
    ASSERT_EQ(index.playlists().size(), 2u);
    EXPECT_EQ(index.playlists().at(LibraryIndex::seenRef(kKeyA, SeenKind::Playlist, "p1")).name, "Road Trip");
    EXPECT_EQ(index.playlists().at(LibraryIndex::seenRef(kKeyB, SeenKind::Playlist, "p1")).name, "Same id");
    EXPECT_EQ(index.playlistServers(), (std::set<std::string>{kKeyA, kKeyB}));
}

TEST(LibraryIndexPlaylists, ReplacingOneServersPlaylistsLeavesTheOthersAlone) {
    FakeLibraryStore store;
    store.playlists = {{kKeyA, "p1", "Old", 12, 5}, {kKeyA, "p2", "Gone", 1, 5}, {kKeyB, "p1", "Other server", 1, 6}};
    LibraryIndex index(store);
    index.replacePlaylists(kKeyA, {{"", "p1", "New", 3, 9}});
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlists, (std::vector<IndexPlaylist>{{kKeyA, "p1", "New", 3, 9}, {kKeyB, "p1", "Other server", 1, 6}}))
        << "the server is filled in from the key";
    index.replacePlaylists(kKeyA, {});
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlists, (std::vector<IndexPlaylist>{{kKeyB, "p1", "Other server", 1, 6}}));
}

TEST(LibraryIndexPlaylists, AddingOneReplacesThePlaylistWithTheSameId) {
    FakeLibraryStore store;
    store.playlists = {{kKeyA, "p1", "Old", 12, 5}};
    LibraryIndex index(store);
    index.addPlaylist({kKeyA, "p2", "Added", 2, 7});
    index.addPlaylist({kKeyA, "p1", "Renamed", 12, 5});
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlists, (std::vector<IndexPlaylist>{{kKeyA, "p1", "Renamed", 12, 5}, {kKeyA, "p2", "Added", 2, 7}}));
}

TEST(LibraryIndexPlaylists, UnchangedPlaylistsWriteNothing) {
    FakeLibraryStore store;
    store.playlists = {{kKeyA, "p1", "Road Trip", 12, 5}};
    LibraryIndex index(store);
    index.replacePlaylists(kKeyA, {{kKeyA, "p1", "Road Trip", 12, 5}});
    index.addPlaylist({kKeyA, "p1", "Road Trip", 12, 5});
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlistSaves, 0);
}

TEST(LibraryIndexPlaylists, AFailedSaveIsRetriedAndItemsDoNotRewritePlaylists) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.addPlaylist({kKeyA, "p1", "Road Trip", 12, 5});
    store.failSaves = true;
    EXPECT_FALSE(index.save());
    store.failSaves = false;
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlists.size(), 1u);
    EXPECT_EQ(store.playlistSaves, 1);
    index.upsert(album("a", "Alpha"), kNow);
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlistSaves, 1) << "only a change of the playlists writes them";
}

TEST(LibraryIndexPlaylists, ItemsAndPlaylistsAreSavedApartAndTheFailureOfOneIsNotTheOthers) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    index.addPlaylist({kKeyA, "p1", "Road Trip", 12, 5});
    store.failPlaylistSaves = true;
    store.failItemSaves = true;
    store.failPlaylistSaves = false;
    EXPECT_FALSE(index.save());
    EXPECT_EQ(store.playlists.size(), 1u) << "the playlists are tried although the items failed";
    store.failItemSaves = false;
    index.addPlaylist({kKeyA, "p2", "Another", 1, 5});
    store.failPlaylistSaves = true;
    EXPECT_TRUE(index.saveItems()) << "the playlists do not count";
    EXPECT_EQ(store.items.count("a"), 1u);
    EXPECT_FALSE(index.savePlaylists());
    EXPECT_FALSE(index.save());
    EXPECT_EQ(store.playlists.size(), 1u) << "p2 is not written";
    store.failPlaylistSaves = false;
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.playlists.size(), 2u);
}

TEST(LibraryIndexPlaylists, AFailedItemSaveDoesNotLoseThePlaylistsAndTheOtherWayRound) {
    FakeLibraryStore store;
    LibraryIndex index(store);
    index.upsert(album("a", "Alpha"), kNow);
    index.addPlaylist({kKeyA, "p1", "Road Trip", 12, 5});
    store.failSaves = true;
    EXPECT_FALSE(index.save());
    store.failSaves = false;
    EXPECT_TRUE(index.save());
    EXPECT_EQ(store.items.count("a"), 1u);
    EXPECT_EQ(store.playlists.size(), 1u);
}
