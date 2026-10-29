#include "core/track_cache.h"
#include "fakes.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>
#include <tuple>

using oap::CachedList;
using oap::CachedTrack;
using oap::RemoteChapter;
using oap::RemoteSource;
using oap::TrackCache;
using oap::test::diskFiles;
using oap::test::testTempDir;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
}

RemoteSource album(const std::string& id) { return {RemoteSource::Kind::Album, id}; }
RemoteSource playlist(const std::string& id) { return {RemoteSource::Kind::Playlist, id}; }

CachedList listOf(RemoteSource source, std::vector<CachedTrack> tracks, std::string name = "Weather",
                  std::string artist = "Band") {
    return {std::move(source), std::move(name), std::move(artist), std::move(tracks)};
}

CachedTrack track(std::string id, std::string file) {
    CachedTrack t;
    t.id = std::move(id);
    t.file = std::move(file);
    t.title = "Title " + t.id;
    t.duration = 181.5;
    t.changed = "2025-03-01T10:00:00Z";
    return t;
}
} // namespace

class TrackCacheTest : public testing::Test {
protected:
    void SetUp() override {
        root = testTempDir();
        fs::remove_all(root);
    }
    void TearDown() override { fs::remove_all(root); }
    TrackCache cache() { return TrackCache(diskFiles(), root, "https://music.example.com/", "anna"); }
    fs::path root;
};

TEST_F(TrackCacheTest, ManifestRoundTrips) {
    CachedTrack a = track("s1", "01.flac");
    a.title = "Tab\there, new\nline and back\\slash";
    a.pinned = true;
    a.lastPlayed = 1700000000;
    CachedTrack b = track("s2", "02.mp3");
    b.duration = 0;
    b.changed = "";
    ASSERT_TRUE(cache().save(listOf(album("al1"), {a, b})));

    auto loaded = cache().load(album("al1")).tracks;
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].id, "s1");
    EXPECT_EQ(loaded[0].file, "01.flac");
    EXPECT_EQ(loaded[0].title, a.title);
    EXPECT_DOUBLE_EQ(loaded[0].duration, 181.5);
    EXPECT_EQ(loaded[0].changed, "2025-03-01T10:00:00Z");
    EXPECT_TRUE(loaded[0].pinned);
    EXPECT_EQ(loaded[0].lastPlayed, 1700000000);
    EXPECT_EQ(loaded[1].id, "s2");
    EXPECT_EQ(loaded[1].changed, "");
    EXPECT_FALSE(loaded[1].pinned);
    EXPECT_EQ(loaded[1].lastPlayed, 0);
}

TEST_F(TrackCacheTest, CachedBitrateRoundTrips) {
    CachedTrack original = track("original", "original.flac");
    CachedTrack low = track("low", "low.mp3");
    low.bitrate = 128;
    CachedTrack high = track("high", "high.mp3");
    high.bitrate = 320;
    ASSERT_TRUE(cache().save(listOf(album("al1"), {original, low, high})));
    const auto loaded = cache().load(album("al1")).tracks;
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].bitrate, 0);
    EXPECT_EQ(loaded[1].bitrate, 128);
    EXPECT_EQ(loaded[2].bitrate, 320);
}

TEST_F(TrackCacheTest, LegacyManifestDefaultsToOriginalAndAddsBitrateOnSave) {
    TrackCache c = cache();
    const std::string head = "oap-cache 2\nlist\talbum\tal1\tWeather\tBand\n";
    const std::string row = "s1\t01.flac\tStorm\t181.500\t\t1\t1700000000";
    writeFile(c.manifestFile(album("al1")), head + row + "\n");
    writeFile(c.listFolder(album("al1")) / "01.flac", "audio");
    CachedList loaded = c.load(album("al1"));
    ASSERT_EQ(loaded.tracks.size(), 1u);
    EXPECT_EQ(loaded.tracks[0].bitrate, 0);
    EXPECT_EQ(diskFiles().read(c.manifestFile(album("al1"))), head + row + "\n") << "load never migrates on disk";
    ASSERT_TRUE(c.save(loaded));
    EXPECT_EQ(diskFiles().read(c.manifestFile(album("al1"))), head + row + "\t0\n");
    const auto saved = c.load(album("al1"));
    ASSERT_EQ(saved.tracks.size(), 1u);
    EXPECT_EQ(saved.tracks[0].bitrate, 0);
    EXPECT_TRUE(c.find("s1")) << "the legacy file still plays";
}

TEST_F(TrackCacheTest, InvalidBitrateSpoilsTheManifest) {
    TrackCache c = cache();
    const std::string row = "oap-cache 2\nlist\talbum\tal1\tWeather\tBand\ns1\t01.mp3\tStorm\t1.000\t\t0\t0\t";
    for (const std::string& bitrate : {"", "-1", "128kbps", "99999999999999999999999999", "2147483648", "128\textra"}) {
        writeFile(c.manifestFile(album("al1")), row + bitrate + "\n");
        EXPECT_TRUE(c.load(album("al1")).source.id.empty()) << bitrate;
    }
}

TEST_F(TrackCacheTest, AServerBooksChaptersRoundTripInOrderBesideTheTracks) {
    CachedList list = listOf(album("b1"), {track("f1", "f1.mp3"), track("f2", "f2.mp3")}, "Dune", "Herbert");
    list.chapters = {{"Intro", 0}, {"Tab\there\nand back\\slash", 100.5}, {"", 230.25}};
    ASSERT_TRUE(cache().save(list));
    CachedList loaded = cache().load(album("b1"));
    ASSERT_EQ(loaded.chapters.size(), 3u);
    EXPECT_EQ(loaded.chapters[0].title, "Intro");
    EXPECT_DOUBLE_EQ(loaded.chapters[0].start, 0);
    EXPECT_EQ(loaded.chapters[1].title, list.chapters[1].title);
    EXPECT_DOUBLE_EQ(loaded.chapters[1].start, 100.5);
    EXPECT_EQ(loaded.chapters[2].title, "");
    EXPECT_DOUBLE_EQ(loaded.chapters[2].start, 230.25);
    ASSERT_EQ(loaded.tracks.size(), 2u) << "the tracks after the chapters are read";
    EXPECT_EQ(loaded.tracks[1].id, "f2");
    CachedList plain = listOf(album("al1"), {track("s1", "01.flac")});
    ASSERT_TRUE(cache().save(plain));
    EXPECT_TRUE(cache().load(album("al1")).chapters.empty()) << "an album has none";
}

TEST_F(TrackCacheTest, ATrackWhoseIdIsChapterIsATrackNotAChapterLine) {
    TrackCache c = cache();
    CachedList list = listOf(album("b1"), {track("chapter", "chapter.mp3")}, "Dune", "Herbert");
    list.chapters = {{"Intro", 0}};
    writeFile(c.listFolder(album("b1")) / "chapter.mp3", "audio");
    ASSERT_TRUE(c.save(list));
    CachedList loaded = c.load(album("b1"));
    ASSERT_EQ(loaded.tracks.size(), 1u) << "the manifest is not read as damaged";
    EXPECT_EQ(loaded.tracks[0].id, "chapter");
    EXPECT_EQ(loaded.chapters.size(), 1u);
}

TEST_F(TrackCacheTest, ChaptersSurviveWhatRewritesAManifest) {
    TrackCache c = cache();
    CachedTrack t = track("f1", "f1.mp3");
    t.pinned = true;
    CachedTrack unpinned = track("f2", "f2.mp3");
    CachedList list = listOf(album("b1"), {t, unpinned}, "Dune", "Herbert");
    list.chapters = {{"Intro", 0}};
    writeFile(c.listFolder(album("b1")) / "f1.mp3", "audio");
    writeFile(c.listFolder(album("b1")) / "f2.mp3", "audio");
    ASSERT_TRUE(c.save(list));
    EXPECT_EQ(c.trim(0, {}, {}), 1u) << "the unpinned file goes";
    ASSERT_EQ(c.load(album("b1")).tracks.size(), 1u);
    ASSERT_EQ(c.load(album("b1")).chapters.size(), 1u) << "the manifest written by trim() keeps them";
    EXPECT_EQ(c.lists(true).front().chapters.front().title, "Intro");
}

TEST_F(TrackCacheTest, ABadChapterLineSpoilsTheManifest) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("b1")) / "f1.mp3", "audio");
    const std::string head = "oap-cache 2\nlist\talbum\tb1\tDune\tHerbert\n";
    const std::string row = "f1\tf1.mp3\tT\t1.000\t\t1\t0\n";
    for (const std::string& bad : {
             head + "chapter\tx\tIntro\n" + row,          // the start is not a number
             head + "chapter\t1.000\n" + row,              // too few columns
             head + "chapter\t1.000\tA\textra\n" + row,   // too many
             head + "chapter\t1.000\tA\\x\n" + row,       // unknown escape
             head + row + "chapter\t1.000\tAfter\n",      // chapters come before the tracks
         }) {
        writeFile(c.manifestFile(album("b1")), bad);
        EXPECT_TRUE(c.load(album("b1")).source.id.empty()) << bad;
    }
    writeFile(c.manifestFile(album("b1")), head + "chapter\t1.000\tA\n" + row);
    CachedList ok = c.load(album("b1"));
    ASSERT_EQ(ok.chapters.size(), 1u);
    EXPECT_EQ(ok.tracks.size(), 1u);
}

TEST_F(TrackCacheTest, SaveReplacesTheManifestAndLeavesNoTemporaryFile) {
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac"), track("s2", "02.flac")})));
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s3", "03.flac")})));
    auto loaded = cache().load(album("al1")).tracks;
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "s3");
    int files = 0;
    for (const auto& e : fs::directory_iterator(cache().listFolder(album("al1")))) {
        (void)e;
        ++files;
    }
    EXPECT_EQ(files, 1);
}

TEST_F(TrackCacheTest, ManifestsGoThroughTheTextFilesInterfaceAndNeverTouchTheDisk) {
    class MemoryFiles : public oap::TextFiles {
    public:
        std::optional<std::string> read(const fs::path& file) override {
            auto it = files.find(file);
            return it == files.end() ? std::nullopt : std::optional<std::string>(it->second);
        }
        bool writeAtomically(const fs::path& file, const std::string& contents) override {
            files[file] = contents;
            return true;
        }
        std::map<fs::path, std::string> files;
    } memory;
    TrackCache c(memory, root, "https://music.example.com/", "anna");
    ASSERT_TRUE(c.save(listOf(album("al1"), {track("s1", "01.flac")})));
    EXPECT_EQ(memory.files.size(), 1u);
    EXPECT_TRUE(memory.files.count(c.manifestFile(album("al1"))));
    EXPECT_FALSE(fs::exists(root)) << "core wrote no file itself";
    CachedList loaded = c.load(album("al1"));
    ASSERT_EQ(loaded.tracks.size(), 1u);
    EXPECT_EQ(loaded.tracks[0].id, "s1");
}

TEST_F(TrackCacheTest, SavingOverAManifestThatCannotBeReadDeletesTheFilesItNoLongerLists) {
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac"), track("s2", "02.flac")})));
    fs::path folder = cache().listFolder(album("al1"));
    for (const char* name : {"01.flac", "02.flac", "03.flac", "03.flac.part", "notes.txt"}) writeFile(folder / name, "x");
    writeFile(cache().listFolder(album("al2")) / "other.flac", "x");
    writeFile(cache().manifestFile(album("al1")), "not a manifest\n");
    ASSERT_TRUE(cache().load(album("al1")).tracks.empty());
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s3", "03.flac")})));
    EXPECT_TRUE(fs::exists(folder / "03.flac")) << "listed";
    EXPECT_TRUE(fs::exists(folder / "03.flac.part")) << "a .part may belong to a running transfer";
    EXPECT_FALSE(fs::exists(folder / "01.flac"));
    EXPECT_FALSE(fs::exists(folder / "02.flac"));
    EXPECT_FALSE(fs::exists(folder / "notes.txt"));
    EXPECT_TRUE(fs::exists(cache().listFolder(album("al2")) / "other.flac")) << "another list is not touched";
    EXPECT_EQ(cache().load(album("al1")).tracks.size(), 1u);
}

TEST_F(TrackCacheTest, AManifestThatCannotBeReadIsNotReplacedAndNothingIsDeleted) {
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac")})));
    fs::path folder = cache().listFolder(album("al1"));
    writeFile(folder / "01.flac", "x");
    class Unreadable : public oap::TextFiles {
    public:
        std::optional<std::string> read(const fs::path&) override { return std::nullopt; } // an I/O error
        bool writeAtomically(const fs::path& file, const std::string& contents) override {
            return oap::test::diskFiles().writeAtomically(file, contents);
        }
    } broken;
    TrackCache c(broken, root, "https://music.example.com/", "anna");
    EXPECT_FALSE(c.save(listOf(album("al1"), {track("s2", "02.flac")}))) << "what could not be read is not overwritten";
    EXPECT_TRUE(fs::exists(folder / "01.flac"));
    EXPECT_EQ(cache().load(album("al1")).tracks.at(0).id, "s1") << "the manifest is as it was";
}

TEST_F(TrackCacheTest, SavingOverAReadableOrMissingManifestKeepsUnlistedFiles) {
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac")})));
    fs::path folder = cache().listFolder(album("al1"));
    writeFile(folder / "02.flac", "x");
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac")})));
    EXPECT_TRUE(fs::exists(folder / "02.flac")) << "the manifest was fine: not this function's business";
    ASSERT_TRUE(cache().remove(album("al1")));
    writeFile(folder / "02.flac", "x");
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac")})));
    EXPECT_TRUE(fs::exists(folder / "02.flac")) << "there was no manifest to lose";
}

TEST_F(TrackCacheTest, FailedSaveKeepsTheOldManifest) {
    ASSERT_TRUE(cache().save(listOf(album("al1"), {track("s1", "01.flac")})));
    // A folder where the temporary file goes makes the write fail.
    fs::path tmp = cache().manifestFile(album("al1"));
    tmp += ".tmp";
    fs::create_directories(tmp);
    EXPECT_FALSE(cache().save(listOf(album("al1"), {track("s2", "02.flac")})));
    auto loaded = cache().load(album("al1")).tracks;
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "s1");
}

TEST_F(TrackCacheTest, FindsACompleteFileInAnyList) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("al1")) / "01.flac", "audio");
    writeFile(c.listFolder(playlist("pl2")) / "x.mp3", "audio");
    ASSERT_TRUE(c.save(listOf(album("al1"), {track("s1", "01.flac")})));
    ASSERT_TRUE(c.save(listOf(playlist("pl2"), {track("s9", "x.mp3")})));
    EXPECT_EQ(c.find("s1"), c.listFolder(album("al1")) / "01.flac");
    EXPECT_EQ(c.find("s9"), c.listFolder(playlist("pl2")) / "x.mp3");
    EXPECT_FALSE(c.find("s2"));
    EXPECT_FALSE(c.find(""));
}

TEST_F(TrackCacheTest, PartMissingAndEmptyFilesAreNeverReturned) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("al1")) / "01.flac.part", "half");
    writeFile(c.listFolder(album("al1")) / "03.flac", "");
    fs::create_directories(c.listFolder(album("al1")) / "04.flac");
    ASSERT_TRUE(c.save(listOf(album("al1"), {track("s1", "01.flac.part"), track("s2", "02.flac"), track("s3", "03.flac"),
                                                 track("s4", "04.flac")})));
    EXPECT_FALSE(c.find("s1")); // a .part file
    EXPECT_FALSE(c.find("s2")); // in the manifest, no file
    EXPECT_FALSE(c.find("s3")); // empty file
    EXPECT_FALSE(c.find("s4")); // a folder
}

TEST_F(TrackCacheTest, IndexHoldsTheCompleteFilesOfAllListsAndOnlyThose) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("al1")) / "01.flac", "audio");
    writeFile(c.listFolder(album("al1")) / "02.flac.part", "half");
    writeFile(c.listFolder(album("al1")) / "03.flac", "");
    writeFile(c.listFolder(playlist("pl2")) / "x.mp3", "audio");
    ASSERT_TRUE(c.save(listOf(album("al1"), {track("s1", "01.flac"), track("s2", "02.flac.part"), track("s3", "03.flac"),
                                                 track("s4", "04.flac")})));
    ASSERT_TRUE(c.save(listOf(playlist("pl2"), {track("s9", "x.mp3")})));
    EXPECT_EQ(c.index(), (std::map<std::string, fs::path>{{"s1", c.listFolder(album("al1")) / "01.flac"},
                                                           {"s9", c.listFolder(playlist("pl2")) / "x.mp3"}}));
    EXPECT_TRUE(TrackCache(diskFiles(), root, "", "anna").index().empty());
}

TEST_F(TrackCacheTest, FileOnDiskWithoutManifestEntryIsNotFound) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("al1")) / "01.flac", "audio");
    EXPECT_FALSE(c.find("s1"));
}

TEST_F(TrackCacheTest, CorruptManifestIsIgnored) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("al1")) / "01.flac", "audio");
    const std::string head = "oap-cache 2\nlist\talbum\tal1\tWeather\tBand\n";
    const std::string row = "s1\t01.flac\tT\t1.000\t\t0\t0\n";
    for (const std::string& bad : {
             std::string("not a manifest at all\n"),
             std::string(""),
             std::string("oap-cache 1\n") + row,                                                   // format 1 has no list line
             std::string("oap-cache 3\nlist\talbum\tal1\tWeather\tBand\n") + row,                  // another version
             std::string("oap-cache 2\n") + row,                                                   // no list line
             std::string("oap-cache 2\nlist\tbook\tal1\tWeather\tBand\n") + row,                   // unknown kind
             std::string("oap-cache 2\nlist\talbum\t\tWeather\tBand\n") + row,                     // empty id
             std::string("oap-cache 2\nlist\talbum\tal1\tWeather\n") + row,                        // list line too short
             std::string("oap-cache 2\nlist\talbum\tal1\tWeather\\x\tBand\n") + row,              // unknown escape
             head + "s1\t01.flac\tT\t1.000\t\t0\t0\textra\n",                                    // too many columns
             head + "s1\t01.flac\n",                                                              // too few columns
             head + "s1\t01.flac\tT\tx\t\t0\t0\n",                                                // duration is not a number
             head + "s1\t01.flac\tT\t1\t\t7\t0\n",                                                // pinned is not 0 or 1
             head + "s1\t01.flac\tT\t1\t\t0\tlater\n",                                            // last played is not a number
             head + "s1\t../../etc/passwd\tT\t1\t\t0\t0\n",                                       // leaves the folder
             head + "s1\t01.flac\tT\\x\t1\t\t0\t0\n",                                             // unknown escape
             head + row + "garbage line\n",                                                        // one bad line spoils the file
             "oap-cache 2\nlist\talbum\tother\tWeather\tBand\n" + row,                          // the manifest of another list
         }) {
        writeFile(c.manifestFile(album("al1")), bad);
        EXPECT_TRUE(c.load(album("al1")).tracks.empty()) << bad;
        EXPECT_TRUE(c.load(album("al1")).source.id.empty()) << bad;
        EXPECT_FALSE(c.find("s1")) << bad;
        EXPECT_TRUE(c.lists().empty()) << bad;
    }
    writeFile(c.manifestFile(album("al1")), head + row);
    EXPECT_EQ(c.load(album("al1")).tracks.size(), 1u);
    EXPECT_TRUE(c.find("s1"));
    EXPECT_EQ(c.lists().size(), 1u);
}

TEST_F(TrackCacheTest, NoManifestIsEmpty) {
    EXPECT_TRUE(cache().load(album("nothing")).tracks.empty());
    EXPECT_FALSE(cache().find("s1"));
}

TEST_F(TrackCacheTest, ListKeepsItsKindNameAndArtist) {
    CachedList in = listOf(playlist("p7"), {track("s1", "01.flac")}, "Tab\there\nnew line", "A\\B");
    ASSERT_TRUE(cache().save(in));
    CachedList out = cache().load(playlist("p7"));
    EXPECT_EQ(out.source.kind, RemoteSource::Kind::Playlist);
    EXPECT_EQ(out.source.id, "p7");
    EXPECT_EQ(out.name, in.name);
    EXPECT_EQ(out.artist, in.artist);
    ASSERT_EQ(out.tracks.size(), 1u);
}

TEST_F(TrackCacheTest, AnAlbumAndAPlaylistWithTheSameIdAreApart) {
    TrackCache c = cache();
    EXPECT_NE(c.listFolder(album("7")), c.listFolder(playlist("7")));
    EXPECT_EQ(c.listFolder(album("7")).parent_path(), c.serverFolder());
    writeFile(c.listFolder(album("7")) / "a.flac", "audio");
    writeFile(c.listFolder(playlist("7")) / "p.flac", "audio");
    ASSERT_TRUE(c.save(listOf(album("7"), {track("sa", "a.flac")}, "Album seven")));
    ASSERT_TRUE(c.save(listOf(playlist("7"), {track("sp", "p.flac")}, "Playlist seven")));
    EXPECT_EQ(c.load(album("7")).tracks[0].id, "sa");
    EXPECT_EQ(c.load(playlist("7")).tracks[0].id, "sp");
    EXPECT_EQ(c.lists().size(), 2u);
}

TEST_F(TrackCacheTest, ListsAreSortedByNameAndHoldOnlyCompleteTracks) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("b1")) / "x.flac", "audio");
    writeFile(c.listFolder(album("b2")) / "y.flac", "audio");
    writeFile(c.listFolder(playlist("b3")) / "z.flac", "audio");
    writeFile(c.listFolder(album("b2")) / "y2.flac.part", "half");
    writeFile(c.listFolder(album("b4")) / "w.flac.part", "half");
    ASSERT_TRUE(c.save(listOf(album("b1"), {track("s1", "x.flac")}, "weather", "Band")));
    ASSERT_TRUE(c.save(listOf(album("b2"), {track("s2", "y.flac"), track("s2b", "y2.flac.part"), track("s2c", "gone.flac")},
                              "Apples", "Cider")));
    ASSERT_TRUE(c.save(listOf(playlist("b3"), {track("s3", "z.flac")}, "Zebra", "")));
    ASSERT_TRUE(c.save(listOf(album("b4"), {track("s4", "w.flac.part")}, "Only a part", ""))); // nothing whole
    fs::create_directories(c.serverFolder() / "no-manifest");
    auto lists = c.lists();
    ASSERT_EQ(lists.size(), 3u);
    EXPECT_EQ(lists[0].name, "Apples"); // letter case does not matter
    EXPECT_EQ(lists[1].name, "weather");
    EXPECT_EQ(lists[2].name, "Zebra");
    EXPECT_EQ(lists[0].artist, "Cider");
    EXPECT_EQ(lists[2].source.kind, RemoteSource::Kind::Playlist);
    ASSERT_EQ(lists[0].tracks.size(), 1u);
    EXPECT_EQ(lists[0].tracks[0].id, "s2");
}

TEST_F(TrackCacheTest, AManifestCopiedToAnotherFolderIsNotListed) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("a1")) / "x.flac", "audio");
    ASSERT_TRUE(c.save(listOf(album("a1"), {track("s1", "x.flac")})));
    fs::copy(c.listFolder(album("a1")), c.serverFolder() / "copy", fs::copy_options::recursive);
    ASSERT_EQ(c.lists().size(), 1u);
}

TEST_F(TrackCacheTest, RemoveDeletesOnlyThatListsFolder) {
    TrackCache c = cache();
    writeFile(c.listFolder(album("a1")) / "x.flac", "audio");
    writeFile(c.listFolder(playlist("a1")) / "y.flac", "audio");
    ASSERT_TRUE(c.save(listOf(album("a1"), {track("s1", "x.flac")})));
    ASSERT_TRUE(c.save(listOf(playlist("a1"), {track("s2", "y.flac")})));
    TrackCache other(diskFiles(), root, "https://other.example", "anna");
    writeFile(other.listFolder(album("a1")) / "x.flac", "audio");
    EXPECT_TRUE(c.remove(album("a1")));
    EXPECT_FALSE(fs::exists(c.listFolder(album("a1"))));
    EXPECT_TRUE(fs::exists(c.listFolder(playlist("a1")) / "y.flac"));
    EXPECT_TRUE(fs::exists(other.listFolder(album("a1")) / "x.flac"));
    EXPECT_TRUE(fs::exists(c.serverFolder()));
    EXPECT_TRUE(c.remove(album("a1"))) << "already gone is fine";
}

TEST_F(TrackCacheTest, RemoveNeverLeavesTheServerFolder) {
    TrackCache c = cache();
    fs::create_directories(root / "keep");
    writeFile(root / "keep" / "f", "x");
    for (const RemoteSource& s : {album(""), album(".."), album("../keep"), album("/"), playlist("../../")}) {
        fs::create_directories(c.serverFolder()); // a deleted folder would hide a ".." that climbs out of it
        c.remove(s);
        EXPECT_TRUE(fs::exists(root / "keep" / "f")) << s.id;
    }
    ASSERT_TRUE(fs::exists(root / "keep" / "f"));
    EXPECT_FALSE(TrackCache(diskFiles(), root, "", "anna").remove(album("a1")));
}

TEST_F(TrackCacheTest, AnEmptyServerUrlStoresNothing) {
    TrackCache c(diskFiles(), root, "", "anna");
    EXPECT_FALSE(c.enabled());
    EXPECT_FALSE(c.save(listOf(album("al1"), {track("s1", "01.flac")})));
    EXPECT_TRUE(c.load(album("al1")).tracks.empty());
    EXPECT_FALSE(c.find("s1"));
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(TrackCacheTest, AnEmptyRootStoresNothing) {
    TrackCache c(diskFiles(), "", "https://music.example.com", "anna");
    EXPECT_FALSE(c.enabled());
    EXPECT_FALSE(c.save(listOf(album("al1"), {track("s1", "01.flac")})));
}

TEST_F(TrackCacheTest, ServerKeyIsASafeNameWithoutCredentials) {
    std::string key = TrackCache::serverKey("https://anna:hunter2@Music.Example.com:4533/sub/", "anna");
    EXPECT_EQ(key.find("hunter2"), std::string::npos);
    EXPECT_EQ(key.find('/'), std::string::npos);
    EXPECT_EQ(key.find(':'), std::string::npos);
    EXPECT_EQ(key.find('@'), std::string::npos);
    EXPECT_NE(key.find("Music_Example_com_4533"), std::string::npos);
    // The same server gives the same key; a trailing slash or a login in the URL does not matter.
    EXPECT_EQ(TrackCache::serverKey("https://music.example.com/", "anna"),
              TrackCache::serverKey("https://music.example.com", "anna"));
    EXPECT_EQ(TrackCache::serverKey("https://anna:hunter2@music.example.com", "anna"),
              TrackCache::serverKey("https://bob:other@music.example.com", "anna"));
    // Another user or another server gets another folder.
    EXPECT_NE(TrackCache::serverKey("https://music.example.com", "anna"),
              TrackCache::serverKey("https://music.example.com", "bob"));
    EXPECT_NE(TrackCache::serverKey("https://music.example.com", "anna"),
              TrackCache::serverKey("https://music.example.org", "anna"));
    EXPECT_EQ(TrackCache::serverKey("", "anna"), "");
}

TEST_F(TrackCacheTest, ServerKeyOfAHugeHostStaysShort) {
    EXPECT_LT(TrackCache::serverKey("https://" + std::string(500, 'a') + ".com", "u").size(), 100u);
}

TEST_F(TrackCacheTest, SafeNameNeverLeavesTheFolder) {
    EXPECT_EQ(TrackCache::safeName("al-1_x.2"), "al-1_x.2");
    EXPECT_EQ(TrackCache::safeName("a/b"), "a%2Fb");
    EXPECT_EQ(TrackCache::safeName("a%b"), "a%25b"); // so "a/b" and "a%2Fb" differ
    EXPECT_EQ(TrackCache::safeName(".."), "%2E%2E");
    EXPECT_EQ(TrackCache::safeName("."), "%2E");
    EXPECT_EQ(TrackCache::safeName(""), "%00");
    EXPECT_EQ(TrackCache::safeName("Сон"), "%D0%A1%D0%BE%D0%BD");
    TrackCache c = cache();
    EXPECT_EQ(c.listFolder(album("../../x")).parent_path(), c.serverFolder());
}

// ------------------------------------------------------- automatic cache: trim()

namespace {
constexpr int64_t kNow = 1'800'000'000; // a real wall time (after kEarliestPlayTime)

// A list of `n`-byte files with these (id, pinned, lastPlayed); the files exist.
CachedList seedFiles(const TrackCache& c, RemoteSource source, size_t bytes,
                     std::vector<std::tuple<std::string, bool, int64_t>> tracks) {
    CachedList list = listOf(source, {});
    for (const auto& [id, pinned, played] : tracks) {
        CachedTrack t = track(id, id + ".mp3");
        t.pinned = pinned;
        t.lastPlayed = played;
        list.tracks.push_back(t);
        writeFile(c.listFolder(source) / t.file, std::string(bytes, 'x'));
    }
    EXPECT_TRUE(c.save(list));
    return list;
}
std::vector<std::string> ids(const CachedList& list) {
    std::vector<std::string> out;
    for (const CachedTrack& t : list.tracks) out.push_back(t.id);
    return out;
}
} // namespace

TEST_F(TrackCacheTest, TrimDeletesTheLeastRecentlyPlayedUnpinnedFirstUntilUnderTheLimit) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"new", false, 300}, {"old", false, 100}, {"mid", false, 200}});
    EXPECT_EQ(c.trim(20, {}, {}), 1u);
    EXPECT_EQ(ids(c.load(album("a"))), (std::vector<std::string>{"new", "mid"}));
    EXPECT_FALSE(fs::exists(c.listFolder(album("a")) / "old.mp3"));
    EXPECT_EQ(c.trim(10, {}, {}), 1u);
    EXPECT_EQ(ids(c.load(album("a"))), (std::vector<std::string>{"new"}));
    EXPECT_EQ(c.trim(10, {}, {}), 0u) << "at the limit nothing goes";
}

TEST_F(TrackCacheTest, TrimOrdersByPlayAcrossListsAndNeverPlayedGoesFirst) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"a1", false, 500}});
    seedFiles(c, playlist("p"), 10, {{"p1", false, 0}, {"p2", false, 400}});
    EXPECT_EQ(c.trim(20, {}, {}), 1u);
    EXPECT_FALSE(c.find("p1")) << "never played";
    EXPECT_TRUE(c.find("a1"));
    EXPECT_TRUE(c.find("p2"));
}

TEST_F(TrackCacheTest, TrimNeverDeletesPinnedTracksAndDoesNotCountThem) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 100, {{"pin", true, 1}});
    seedFiles(c, album("b"), 10, {{"u", false, 2}});
    EXPECT_EQ(c.trim(10, {}, {}), 0u) << "the pinned 100 bytes are not counted";
    EXPECT_EQ(c.trim(0, {}, {}), 1u);
    EXPECT_TRUE(c.find("pin"));
    EXPECT_FALSE(c.find("u"));
}

TEST_F(TrackCacheTest, TrimKeepsTheFileInKeepAndTakesTheNextOne) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"open", false, 1}, {"other", false, 2}});
    EXPECT_EQ(c.trim(0, {}, {c.listFolder(album("a")) / "open.mp3"}), 1u);
    EXPECT_TRUE(c.find("open"));
    EXPECT_FALSE(c.find("other"));
}

TEST_F(TrackCacheTest, TrimWritesPlayedTimesAndUsesThemForTheOrder) {
    TrackCache c = cache();
    const int64_t t = kNow;
    seedFiles(c, album("a"), 10, {{"x", false, t + 100}, {"y", false, t + 200}});
    // x played after y: y is now the older one.
    EXPECT_EQ(c.trim(10, {{"x", t + 300}}, {}), 1u);
    EXPECT_TRUE(c.find("x"));
    EXPECT_FALSE(c.find("y"));
    EXPECT_EQ(c.load(album("a")).tracks[0].lastPlayed, t + 300);
}

TEST_F(TrackCacheTest, TheLatestPlayReplacesLastPlayedEvenWhenItIsEarlier) {
    TrackCache c = cache();
    // A wrong clock (ahead) once wrote a far future time for x: a later real play must replace it.
    seedFiles(c, album("a"), 10, {{"x", false, kNow + 5'000'000}, {"y", false, kNow + 1000}});
    EXPECT_EQ(c.trim(10, {{"x", kNow + 200}}, {}), 1u);
    EXPECT_FALSE(c.find("x")) << "its real play is older than y's";
    EXPECT_TRUE(c.find("y"));
}

TEST_F(TrackCacheTest, AUnsetClockIsNotRecordedAsAPlay) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"x", false, kNow}});
    EXPECT_EQ(c.trim(1000, {{"x", 5}}, {}), 0u); // 1970: the Pi had no time yet
    EXPECT_EQ(c.load(album("a")).tracks[0].lastPlayed, kNow);
}

TEST_F(TrackCacheTest, AnAbandonedPartFileIsEvictedFirstButNotOneInKeep) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"x", false, kNow}});
    writeFile(c.listFolder(album("a")) / "old.mp3.part", std::string(10, 'p'));
    writeFile(c.listFolder(album("a")) / "next.mp3.part", std::string(10, 'p'));
    TrackCache::TrimInfo info;
    EXPECT_EQ(c.trim(20, {}, {c.listFolder(album("a")) / "next.mp3.part"}, &info), 0u);
    EXPECT_FALSE(fs::exists(c.listFolder(album("a")) / "old.mp3.part")) << "nobody resumes it";
    EXPECT_TRUE(fs::exists(c.listFolder(album("a")) / "next.mp3.part")) << "a transfer that waits stays";
    EXPECT_TRUE(c.find("x")) << "the part goes before a track";
    EXPECT_EQ(info.remaining, 20u);
}

TEST_F(TrackCacheTest, AFolderOfOnlyAnAbandonedPartIsRemovedWhenItGoes) {
    TrackCache c = cache();
    fs::create_directories(c.listFolder(album("new")));
    writeFile(c.listFolder(album("new")) / "t.mp3.part", "half");
    c.trim(0, {}, {});
    EXPECT_FALSE(fs::exists(c.listFolder(album("new"))));
}

TEST_F(TrackCacheTest, TrimReportsTheEvictedIdsAndWhatIsLeft) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"x", false, kNow + 1}, {"y", false, kNow + 2}});
    TrackCache::TrimInfo info;
    c.trim(10, {}, {}, &info);
    ASSERT_EQ(info.evicted.size(), 1u);
    EXPECT_EQ(info.evicted[0].first, "x");
    EXPECT_EQ(info.evicted[0].second, c.listFolder(album("a")) / "x.mp3");
    EXPECT_EQ(info.remaining, 10u);
}

TEST_F(TrackCacheTest, TrimWritesAPlayedTimeEvenWhenNothingIsDeleted) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"x", false, 100}});
    EXPECT_EQ(c.trim(1000, {{"x", kNow + 777}, {"unknown", kNow}}, {}), 0u);
    EXPECT_EQ(c.load(album("a")).tracks[0].lastPlayed, kNow + 777);
}

TEST_F(TrackCacheTest, TrimRemovesAListFolderLeftEmptyButNotOneWithAPartFile) {
    TrackCache c = cache();
    seedFiles(c, album("gone"), 10, {{"g", false, 1}});
    seedFiles(c, album("resume"), 10, {{"r", false, 2}});
    writeFile(c.listFolder(album("resume")) / "next.mp3.part", "half");
    EXPECT_EQ(c.trim(0, {}, {c.listFolder(album("resume")) / "next.mp3.part"}), 2u);
    EXPECT_FALSE(fs::exists(c.listFolder(album("gone")))) << "an empty list is removed";
    EXPECT_TRUE(fs::exists(c.listFolder(album("resume")) / "next.mp3.part")) << "a transfer to resume stays";
    EXPECT_TRUE(c.load(album("resume")).tracks.empty());
}

TEST_F(TrackCacheTest, TrimLeavesAnotherServersFilesAlone) {
    TrackCache c = cache();
    TrackCache other(diskFiles(), root, "https://other.example", "anna");
    seedFiles(c, album("a"), 10, {{"mine", false, 1}});
    seedFiles(other, album("a"), 10, {{"theirs", false, 1}});
    c.trim(0, {}, {});
    EXPECT_TRUE(other.find("theirs"));
}

TEST_F(TrackCacheTest, ListsPinnedOnlyLeavesOutListsOfAutomaticTracks) {
    TrackCache c = cache();
    seedFiles(c, album("auto"), 1, {{"a", false, 1}});
    seedFiles(c, album("mixed"), 1, {{"m1", false, 1}, {"m2", true, 1}});
    seedFiles(c, album("mine"), 1, {{"p", true, 1}});
    EXPECT_EQ(c.lists().size(), 3u);
    std::vector<std::string> names;
    for (const CachedList& l : c.lists(true)) names.push_back(l.source.id);
    EXPECT_EQ(names, (std::vector<std::string>{"mine", "mixed"}));
}

TEST_F(TrackCacheTest, TrimRewritesNoManifestWhenNothingChanged) {
    TrackCache c = cache();
    seedFiles(c, album("a"), 10, {{"x", false, kNow + 100}});
    auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 365);
    fs::last_write_time(c.manifestFile(album("a")), longAgo);
    EXPECT_EQ(c.trim(1000, {{"x", kNow + 100}, {"unknown", kNow}}, {}), 0u); // the same time again
    EXPECT_EQ(fs::last_write_time(c.manifestFile(album("a"))), longAgo) << "flash is spared";
}

// Audiobookshelf names a file `<item uuid>/<ino>` (see docs/audiobookshelf.md).
TEST_F(TrackCacheTest, AnAudiobookshelfFileIdMakesOneFlatFileNameAndRoundTripsInTheManifest) {
    const std::string id = "6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f/12345";
    const std::string name = TrackCache::safeName(id);
    EXPECT_EQ(name, "6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f%2F12345");
    EXPECT_EQ(name.find('/'), std::string::npos);
    EXPECT_NE(name, TrackCache::safeName("6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f%2F12345")) << "a '%' in an id differs";

    TrackCache c = cache();
    CachedTrack t = track(id, name + ".m4b");
    ASSERT_TRUE(c.save(listOf(album("6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f"), {t})));
    auto loaded = c.load(album("6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f")).tracks;
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, id);
    EXPECT_EQ(loaded[0].file, name + ".m4b");
}
