#include "core/track_cache.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

using oap::CachedTrack;
using oap::TrackCache;
using oap::test::testTempDir;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
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
    TrackCache cache() { return TrackCache(root, "https://music.example.com/", "anna"); }
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
    ASSERT_TRUE(cache().save("al1", {a, b}));

    auto loaded = cache().load("al1");
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

TEST_F(TrackCacheTest, SaveReplacesTheManifestAndLeavesNoTemporaryFile) {
    ASSERT_TRUE(cache().save("al1", {track("s1", "01.flac"), track("s2", "02.flac")}));
    ASSERT_TRUE(cache().save("al1", {track("s3", "03.flac")}));
    auto loaded = cache().load("al1");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "s3");
    int files = 0;
    for (const auto& e : fs::directory_iterator(cache().listFolder("al1"))) {
        (void)e;
        ++files;
    }
    EXPECT_EQ(files, 1);
}

TEST_F(TrackCacheTest, FailedSaveKeepsTheOldManifest) {
    ASSERT_TRUE(cache().save("al1", {track("s1", "01.flac")}));
    // A folder where the temporary file goes makes the write fail.
    fs::path tmp = cache().manifestFile("al1");
    tmp += ".tmp";
    fs::create_directories(tmp);
    EXPECT_FALSE(cache().save("al1", {track("s2", "02.flac")}));
    auto loaded = cache().load("al1");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "s1");
}

TEST_F(TrackCacheTest, FindsACompleteFileInAnyList) {
    TrackCache c = cache();
    writeFile(c.listFolder("al1") / "01.flac", "audio");
    writeFile(c.listFolder("pl2") / "x.mp3", "audio");
    ASSERT_TRUE(c.save("al1", {track("s1", "01.flac")}));
    ASSERT_TRUE(c.save("pl2", {track("s9", "x.mp3")}));
    EXPECT_EQ(c.find("s1"), c.listFolder("al1") / "01.flac");
    EXPECT_EQ(c.find("s9"), c.listFolder("pl2") / "x.mp3");
    EXPECT_FALSE(c.find("s2"));
    EXPECT_FALSE(c.find(""));
}

TEST_F(TrackCacheTest, PartMissingAndEmptyFilesAreNeverReturned) {
    TrackCache c = cache();
    writeFile(c.listFolder("al1") / "01.flac.part", "half");
    writeFile(c.listFolder("al1") / "03.flac", "");
    fs::create_directories(c.listFolder("al1") / "04.flac");
    ASSERT_TRUE(c.save("al1", {track("s1", "01.flac.part"), track("s2", "02.flac"), track("s3", "03.flac"),
                               track("s4", "04.flac")}));
    EXPECT_FALSE(c.find("s1")); // a .part file
    EXPECT_FALSE(c.find("s2")); // in the manifest, no file
    EXPECT_FALSE(c.find("s3")); // empty file
    EXPECT_FALSE(c.find("s4")); // a folder
}

TEST_F(TrackCacheTest, IndexHoldsTheCompleteFilesOfAllListsAndOnlyThose) {
    TrackCache c = cache();
    writeFile(c.listFolder("al1") / "01.flac", "audio");
    writeFile(c.listFolder("al1") / "02.flac.part", "half");
    writeFile(c.listFolder("al1") / "03.flac", "");
    writeFile(c.listFolder("pl2") / "x.mp3", "audio");
    ASSERT_TRUE(c.save("al1", {track("s1", "01.flac"), track("s2", "02.flac.part"), track("s3", "03.flac"),
                               track("s4", "04.flac")}));
    ASSERT_TRUE(c.save("pl2", {track("s9", "x.mp3")}));
    EXPECT_EQ(c.index(), (std::map<std::string, fs::path>{{"s1", c.listFolder("al1") / "01.flac"},
                                                           {"s9", c.listFolder("pl2") / "x.mp3"}}));
    EXPECT_TRUE(TrackCache(root, "", "anna").index().empty());
}

TEST_F(TrackCacheTest, FileOnDiskWithoutManifestEntryIsNotFound) {
    TrackCache c = cache();
    writeFile(c.listFolder("al1") / "01.flac", "audio");
    EXPECT_FALSE(c.find("s1"));
}

TEST_F(TrackCacheTest, CorruptManifestIsIgnored) {
    TrackCache c = cache();
    writeFile(c.listFolder("al1") / "01.flac", "audio");
    const std::string good = "oap-cache 1\ns1\t01.flac\tT\t1.000\t\t0\t0\n";
    for (const std::string& bad : {
             std::string("not a manifest at all\n"),
             std::string(""),
             std::string("oap-cache 2\ns1\t01.flac\tT\t1.000\t\t0\t0\n"), // another version
             std::string("oap-cache 1\ns1\t01.flac\tT\t1\t\t0\t0\textra\n"), // too many columns
             std::string("oap-cache 1\ns1\t01.flac\n"),                       // too few columns
             std::string("oap-cache 1\ns1\t01.flac\tT\tx\t\t0\t0\n"),          // duration is not a number
             std::string("oap-cache 1\ns1\t01.flac\tT\t1\t\t7\t0\n"),          // pinned is not 0 or 1
             std::string("oap-cache 1\ns1\t01.flac\tT\t1\t\t0\tlater\n"),      // last played is not a number
             std::string("oap-cache 1\ns1\t../../etc/passwd\tT\t1\t\t0\t0\n"), // leaves the folder
             std::string("oap-cache 1\ns1\t01.flac\tT\\x\t1\t\t0\t0\n"),       // unknown escape
             good + "garbage line\n",                                          // one bad line spoils the file
         }) {
        writeFile(c.manifestFile("al1"), bad);
        EXPECT_TRUE(c.load("al1").empty()) << bad;
        EXPECT_FALSE(c.find("s1")) << bad;
    }
    writeFile(c.manifestFile("al1"), good);
    EXPECT_EQ(c.load("al1").size(), 1u);
    EXPECT_TRUE(c.find("s1"));
}

TEST_F(TrackCacheTest, NoManifestIsEmpty) {
    EXPECT_TRUE(cache().load("nothing").empty());
    EXPECT_FALSE(cache().find("s1"));
}

TEST_F(TrackCacheTest, AnEmptyServerUrlStoresNothing) {
    TrackCache c(root, "", "anna");
    EXPECT_FALSE(c.enabled());
    EXPECT_FALSE(c.save("al1", {track("s1", "01.flac")}));
    EXPECT_TRUE(c.load("al1").empty());
    EXPECT_FALSE(c.find("s1"));
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(TrackCacheTest, AnEmptyRootStoresNothing) {
    TrackCache c("", "https://music.example.com", "anna");
    EXPECT_FALSE(c.enabled());
    EXPECT_FALSE(c.save("al1", {track("s1", "01.flac")}));
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
    EXPECT_EQ(c.listFolder("../../x").parent_path(), c.serverFolder());
}
