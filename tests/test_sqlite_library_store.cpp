#include "core/library_index.h"
#include "library/sqlite_library_store.h"
#include "temp_dir.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <sys/stat.h>

using namespace oap;
using namespace oap::test;

namespace {

namespace fs = std::filesystem;

const std::string kRefA = "music_example_com-0123456789abcdef:album:3a1f9c";
const std::string kRefB = "other_example_org-fedcba9876543210:album:77";
const std::string kServerA = "music_example_com-0123456789abcdef";
const std::string kServerB = "other_example_org-fedcba9876543210";
const std::string kRefTrack = "music_example_com-0123456789abcdef:track:t9";

class SqliteLibraryStoreTest : public testing::Test {
protected:
    void SetUp() override {
        dir = testTempDir();
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = dir / "library.db";
    }
    void TearDown() override { fs::remove_all(dir); }

    fs::path dir, file;
};

IndexTrack track(const std::string& location, const std::string& title, int number) {
    IndexTrack t;
    t.location = location;
    t.title = title;
    t.artist = "Artist of " + title;
    t.number = number;
    t.duration = 215.375;
    t.size = 9'876'543'210;
    t.fileTime = 1700000000;
    return t;
}

IndexItem album(const std::string& id, const std::string& title) {
    IndexItem item;
    item.id = id;
    item.title = title;
    item.artist = "An Artist";
    item.album = title;
    item.genre = "Jazz";
    item.year = 1999;
    item.musicBrainzId = "mbid-" + id;
    item.firstSeen = 1700000100;
    item.lastPlayed = 1700000200;
    item.trackCount = 12;
    item.created = 1700000300;
    item.tracks = {track("/music/" + id + "/1.mp3", "One", 1), track("/music/" + id + "/2.mp3", "Two", 2)};
    return item;
}

std::map<std::string, IndexItem> byId(const std::vector<IndexItem>& items) {
    std::map<std::string, IndexItem> out;
    for (const auto& item : items) out[item.id] = item;
    return out;
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

mode_t modeOf(const fs::path& path) {
    struct stat st {};
    EXPECT_EQ(::stat(path.c_str(), &st), 0) << path;
    return st.st_mode & 07777;
}

// Runs SQL on the file with a connection of its own, the way another program or a test would.
void rawExec(const fs::path& file, const std::string& sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(file.c_str(), &db), SQLITE_OK);
    char* err = nullptr;
    EXPECT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
    sqlite3_close(db);
}

std::string rawScalar(const fs::path& file, const std::string& sql) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(file.c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

} // namespace

TEST_F(SqliteLibraryStoreTest, StartsEmptyAndCreatesTheFile) {
    SqliteLibraryStore store(dir / "deeper" / "library.db");
    EXPECT_TRUE(store.load().empty());
    EXPECT_TRUE(fs::exists(dir / "deeper" / "library.db"));
}

TEST_F(SqliteLibraryStoreTest, RoundTripsItemsTracksAndReferences) {
    IndexItem a = album("local:/books/Война и мир", "Война и мир \"Том 1\"\nвторая строка 🎵");
    a.source = IndexItem::Source::Cached;
    a.kind = IndexItem::Kind::Book;
    a.artist = "Лев Толстой; it's \"quoted\"";
    a.genre = "line\none\r\ntwo\ttab";
    a.seenOn = {kRefA, kRefB};
    a.tracks[0].title = "Глава 1: 'Начало'\n\"конец\"";
    a.tracks[0].location = "/books/Война и мир/глава 1.mp3";
    a.tracks[0].seenOn = {kRefTrack, kRefB};
    a.tracks[1].duration = 0.1; // not exactly representable: must come back bit for bit
    a.tracks[1].number = 0;
    IndexItem b = album("srv:42", "Plain");
    b.source = IndexItem::Source::Server;
    b.kind = IndexItem::Kind::Podcast;
    b.year = 0;
    b.lastPlayed = 0;
    b.tracks.clear(); // an item without tracks and references
    IndexItem c = album("it's \"id\"; DROP TABLE items;--", "Injection");

    {
        SqliteLibraryStore store(file);
        ASSERT_TRUE(store.save({a, b, c}, {}));
    }
    SqliteLibraryStore reopened(file);
    auto loaded = byId(reopened.load());
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[a.id], a);
    EXPECT_EQ(loaded[b.id], b);
    EXPECT_EQ(loaded[c.id], c);
    ASSERT_EQ(loaded[a.id].tracks.size(), 2u);
    EXPECT_EQ(loaded[a.id].tracks[0].seenOn, (std::vector<std::string>{kRefTrack, kRefB})); // order kept
    EXPECT_TRUE(loaded[a.id].tracks[1].seenOn.empty());
}

TEST_F(SqliteLibraryStoreTest, SavingOneItemLeavesTheOthersUnchanged) {
    IndexItem a = album("a", "Alpha");
    a.seenOn = {kRefA};
    IndexItem b = album("b", "Beta");
    b.tracks[0].seenOn = {kRefTrack};
    IndexItem c = album("c", "Gamma");
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({a, b, c}, {}));

    IndexItem b2 = album("b", "Beta, new edition");
    b2.tracks.pop_back(); // shorter, and without the old reference
    b2.year = 2024;
    ASSERT_TRUE(store.save({b2}, {}));

    auto loaded = byId(store.load());
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded["a"], a);
    EXPECT_EQ(loaded["b"], b2);
    EXPECT_EQ(loaded["b"].tracks.size(), 1u);
    EXPECT_TRUE(loaded["b"].tracks[0].seenOn.empty());
    EXPECT_EQ(loaded["c"], c);
}

TEST_F(SqliteLibraryStoreTest, RemovingAnItemRemovesItsTracksAndReferences) {
    IndexItem a = album("a", "Alpha");
    a.seenOn = {kRefA};
    a.tracks[0].seenOn = {kRefTrack};
    IndexItem b = album("b", "Beta");
    b.seenOn = {kRefB};
    b.tracks[1].seenOn = {kRefTrack};
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({a, b}, {}));
    ASSERT_EQ(rawScalar(file, "SELECT count(*) FROM tracks"), "4");
    ASSERT_EQ(rawScalar(file, "SELECT count(*) FROM refs"), "4");

    ASSERT_TRUE(store.save({}, {"a", "never-stored"}));

    auto loaded = byId(store.load());
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded["b"], b);
    EXPECT_EQ(rawScalar(file, "SELECT count(*) FROM items"), "1");
    EXPECT_EQ(rawScalar(file, "SELECT count(*) FROM tracks"), "2");
    EXPECT_EQ(rawScalar(file, "SELECT count(*) FROM refs"), "2");
    EXPECT_EQ(rawScalar(file, "SELECT count(*) FROM tracks WHERE item_id = 'a'"), "0");
    EXPECT_EQ(rawScalar(file, "SELECT count(*) FROM refs WHERE item_id = 'a'"), "0");
}

TEST_F(SqliteLibraryStoreTest, ChangesAndRemovalsInOneSaveBothApply) {
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({album("a", "Alpha"), album("b", "Beta")}, {}));
    IndexItem c = album("c", "Gamma");
    ASSERT_TRUE(store.save({c}, {"a"}));
    auto loaded = byId(store.load());
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded.count("a"), 0u);
    EXPECT_EQ(loaded["c"], c);
}

TEST_F(SqliteLibraryStoreTest, AFailedSaveStoresNothingAndTheStoreKeepsWorking) {
    IndexItem a = album("a", "Alpha");
    IndexItem c = album("c", "Gamma");
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({a, c}, {}));

    // A track called "boom" makes the database refuse the insert halfway through the save.
    rawExec(file, "CREATE TRIGGER refuse BEFORE INSERT ON tracks WHEN NEW.title = 'boom' "
                  "BEGIN SELECT RAISE(ABORT, 'refused'); END;");
    IndexItem a2 = album("a", "Alpha changed");
    IndexItem bad = album("b", "Bad");
    bad.tracks[1].title = "boom";
    EXPECT_FALSE(store.save({a2, bad}, {"c"}));

    auto loaded = byId(store.load());
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded["a"], a);
    EXPECT_EQ(loaded["c"], c);

    bad.tracks[1].title = "fine";
    EXPECT_TRUE(store.save({a2, bad}, {"c"}));
    loaded = byId(store.load());
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded["a"], a2);
    EXPECT_EQ(loaded["b"], bad);
}

TEST_F(SqliteLibraryStoreTest, PlaylistsRoundTripAndAreReplacedAsAWhole) {
    const std::vector<IndexPlaylist> first = {{kServerA, "p1", "Road Trip", 12, 1772359200},
                                              {kServerA, "p2", "Chill \xD0\xBC\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0", 0, 0},
                                              {kServerB, "p1", "Same id, other server", 3, 5}};
    {
        SqliteLibraryStore store(file);
        EXPECT_TRUE(store.loadPlaylists().empty());
        ASSERT_TRUE(store.savePlaylists(first));
        EXPECT_EQ(store.loadPlaylists(), first);
    }
    SqliteLibraryStore reopened(file);
    EXPECT_EQ(reopened.loadPlaylists(), first) << "kept in the file";

    const std::vector<IndexPlaylist> second = {{kServerA, "p2", "Chill", 4, 99}};
    ASSERT_TRUE(reopened.savePlaylists(second));
    EXPECT_EQ(reopened.loadPlaylists(), second) << "the old list is replaced, not added to";
    ASSERT_TRUE(reopened.savePlaylists({}));
    EXPECT_TRUE(reopened.loadPlaylists().empty());
}

TEST_F(SqliteLibraryStoreTest, PlaylistsAndItemsAreStoredApart) {
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({album("a", "Alpha")}, {}));
    ASSERT_TRUE(store.savePlaylists({{kServerA, "p1", "Road Trip", 12, 1}}));
    ASSERT_TRUE(store.save({album("b", "Beta")}, {"a"}));
    EXPECT_EQ(store.loadPlaylists().size(), 1u) << "saving items leaves the playlists";
    ASSERT_TRUE(store.savePlaylists({}));
    EXPECT_EQ(byId(store.load()).size(), 1u) << "saving playlists leaves the items";
}

TEST_F(SqliteLibraryStoreTest, AFailedPlaylistSaveKeepsTheOldPlaylists) {
    SqliteLibraryStore store(file);
    const std::vector<IndexPlaylist> old = {{kServerA, "p1", "Road Trip", 12, 1}};
    ASSERT_TRUE(store.savePlaylists(old));
    // The same playlist twice breaks the key halfway through the write.
    EXPECT_FALSE(store.savePlaylists({{kServerA, "p2", "New", 1, 1}, {kServerA, "p2", "New again", 2, 2}}));
    EXPECT_EQ(store.loadPlaylists(), old);
    EXPECT_TRUE(store.savePlaylists({{kServerA, "p2", "New", 1, 1}})) << "and the store keeps working";
}

TEST_F(SqliteLibraryStoreTest, AVersion2DatabaseOpensWithItsItemsAndGainsThePlaylistsTable) {
    {
        // A file as the version before this one wrote it: no playlists table, version 2.
        SqliteLibraryStore store(file);
        ASSERT_TRUE(store.save({album("a", "Alpha")}, {}));
    }
    rawExec(file, "DROP TABLE playlists; UPDATE schema_version SET version = 2;");
    ASSERT_EQ(rawScalar(file, "SELECT count(*) FROM sqlite_master WHERE name = 'playlists'"), "0");

    SqliteLibraryStore store(file);
    EXPECT_EQ(byId(store.load()).size(), 1u) << "the items are kept";
    EXPECT_FALSE(fs::exists(SqliteLibraryStore::asidePath(file))) << "it is not moved aside";
    EXPECT_TRUE(store.loadPlaylists().empty());
    const std::vector<IndexPlaylist> playlists = {{kServerA, "p1", "Road Trip", 12, 1}};
    ASSERT_TRUE(store.savePlaylists(playlists));
    EXPECT_EQ(rawScalar(file, "SELECT version FROM schema_version"), "3");
    SqliteLibraryStore reopened(file);
    EXPECT_EQ(reopened.loadPlaylists(), playlists);
    EXPECT_EQ(byId(reopened.load()).size(), 1u);
}

TEST_F(SqliteLibraryStoreTest, DamagedFileOpensEmptyAndIsMovedAside) {
    const std::string garbage = "this is not a database, it is long enough to be read as a header: 0123456789abcdef";
    write(file, garbage);

    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_EQ(slurp(SqliteLibraryStore::asidePath(file)), garbage);

    IndexItem a = album("a", "Alpha");
    ASSERT_TRUE(store.save({a}, {}));
    SqliteLibraryStore reopened(file);
    EXPECT_EQ(byId(reopened.load())["a"], a);
}

TEST_F(SqliteLibraryStoreTest, AFileCutShortOpensEmptyAndIsMovedAside) {
    {
        SqliteLibraryStore store(file);
        std::vector<IndexItem> many;
        for (int i = 0; i < 200; ++i) many.push_back(album("item" + std::to_string(i), "Album " + std::to_string(i)));
        ASSERT_TRUE(store.save(many, {}));
    }
    std::string whole = slurp(file);
    ASSERT_GT(whole.size(), 8192u);
    write(file, whole.substr(0, whole.size() / 2)); // the file system lost the end of it

    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_TRUE(fs::exists(SqliteLibraryStore::asidePath(file)));
}

TEST_F(SqliteLibraryStoreTest, AFileWithAScrambledPageOpensEmptyAndIsMovedAside) {
    {
        SqliteLibraryStore store(file);
        std::vector<IndexItem> many;
        for (int i = 0; i < 200; ++i) many.push_back(album("item" + std::to_string(i), "Album " + std::to_string(i)));
        ASSERT_TRUE(store.save(many, {}));
    }
    std::string whole = slurp(file);
    constexpr size_t kPage = 4096; // the default page size; the damage is a page in the middle, the header stays
    ASSERT_GT(whole.size(), 6 * kPage);
    for (size_t i = 0; i < kPage; ++i) whole[3 * kPage + i] = char(0xA5);
    write(file, whole);

    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_TRUE(fs::exists(SqliteLibraryStore::asidePath(file)));
}

TEST_F(SqliteLibraryStoreTest, AVersion1DatabaseWithoutTheNewColumnsOpensEmptyAndIsMovedAside) {
    rawExec(file, "CREATE TABLE schema_version (version INTEGER NOT NULL); INSERT INTO schema_version VALUES (1);"
                  "CREATE TABLE items (id TEXT PRIMARY KEY);");
    {
        SqliteLibraryStore store(file);
        EXPECT_TRUE(store.load().empty());
        EXPECT_TRUE(store.save({album("a", "Alpha")}, {})); // the new database works
    }
    EXPECT_EQ(rawScalar(SqliteLibraryStore::asidePath(file), "SELECT version FROM schema_version"), "1");
}

TEST_F(SqliteLibraryStoreTest, ANewerSchemaVersionOpensEmptyAndIsMovedAside) {
    rawExec(file, "CREATE TABLE schema_version (version INTEGER NOT NULL); INSERT INTO schema_version VALUES (99);"
                  "CREATE TABLE items (id TEXT, colour TEXT); INSERT INTO items VALUES ('x', 'from the future');");

    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_EQ(rawScalar(SqliteLibraryStore::asidePath(file), "SELECT version FROM schema_version"), "99");
    EXPECT_EQ(rawScalar(SqliteLibraryStore::asidePath(file), "SELECT colour FROM items"), "from the future");

    IndexItem a = album("a", "Alpha");
    ASSERT_TRUE(store.save({a}, {}));
    SqliteLibraryStore reopened(file);
    EXPECT_EQ(byId(reopened.load()).size(), 1u);
}

TEST_F(SqliteLibraryStoreTest, ADatabaseWithoutOurSchemaIsMovedAside) {
    rawExec(file, "CREATE TABLE something_else (a INTEGER); INSERT INTO something_else VALUES (1);");
    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_EQ(rawScalar(SqliteLibraryStore::asidePath(file), "SELECT a FROM something_else"), "1");
}

TEST_F(SqliteLibraryStoreTest, AnEmptyFileIsAFreshDatabaseAndNothingIsMovedAside) {
    write(file, "");
    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_FALSE(fs::exists(SqliteLibraryStore::asidePath(file)));
    EXPECT_TRUE(store.save({album("a", "Alpha")}, {}));
}

TEST_F(SqliteLibraryStoreTest, AnEarlierAsideFileIsReplaced) {
    write(SqliteLibraryStore::asidePath(file), "older damage");
    write(file, "newer damage, longer than the header of a database file is");
    SqliteLibraryStore store(file);
    EXPECT_EQ(slurp(SqliteLibraryStore::asidePath(file)), "newer damage, longer than the header of a database file is");
}

TEST_F(SqliteLibraryStoreTest, TheFileIsMode0600) {
    {
        SqliteLibraryStore store(file);
        EXPECT_EQ(modeOf(file), 0600);
        ASSERT_TRUE(store.save({album("a", "Alpha")}, {}));
        EXPECT_EQ(modeOf(file), 0600);
    }
    ::chmod(file.c_str(), 0644); // made by hand, or by an older version
    SqliteLibraryStore reopened(file);
    EXPECT_EQ(modeOf(file), 0600);
    EXPECT_EQ(reopened.load().size(), 1u);
}

TEST_F(SqliteLibraryStoreTest, AFreshFileIsMode0600EvenWithAnOpenUmask) {
    mode_t old = ::umask(0);
    SqliteLibraryStore store(file);
    ::umask(old);
    EXPECT_EQ(modeOf(file), 0600);
}

TEST_F(SqliteLibraryStoreTest, UsesARollbackJournalNotWal) {
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({album("a", "Alpha")}, {}));
    EXPECT_EQ(rawScalar(file, "PRAGMA journal_mode"), "delete");
    EXPECT_FALSE(fs::exists(file.string() + "-wal"));
    EXPECT_FALSE(fs::exists(file.string() + "-journal")); // gone after the commit
}

TEST_F(SqliteLibraryStoreTest, LoadIgnoresRowsThatBelongToNothing) {
    IndexItem a = album("a", "Alpha");
    a.seenOn = {kRefA};
    {
        SqliteLibraryStore store(file);
        ASSERT_TRUE(store.save({a}, {}));
    }
    rawExec(file, "INSERT INTO items VALUES ('odd-source', 9, 0, 't', '', '', '', 0, '', 0, 0, 0, 0);"
                  "INSERT INTO items VALUES ('odd-kind', 0, -3, 't', '', '', '', 0, '', 0, 0, 0, 0);"
                  "INSERT INTO tracks VALUES ('ghost', 0, '/x.mp3', 'x', '', 1, 1.0, 1, 1);"
                  "INSERT INTO refs VALUES ('ghost', -1, 0, 'ghost-ref');"
                  "INSERT INTO refs VALUES ('a', 7, 0, 'no-such-track');"
                  "INSERT INTO refs VALUES ('a', -2, 0, 'no-such-place');");
    SqliteLibraryStore store(file);
    auto loaded = store.load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], a);
}

TEST_F(SqliteLibraryStoreTest, ANonFiniteDurationIsStoredAsZeroAndDoesNotFailTheSave) {
    IndexItem a = album("a", "Alpha");
    a.tracks[0].duration = std::nan("");
    a.tracks[1].duration = std::numeric_limits<double>::infinity();
    SqliteLibraryStore store(file);
    ASSERT_TRUE(store.save({a}, {}));
    auto loaded = store.load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].tracks[0].duration, 0.0);
    EXPECT_EQ(loaded[0].tracks[1].duration, 0.0);
}

TEST_F(SqliteLibraryStoreTest, ALockedDatabaseIsNotMovedAside) {
    IndexItem a = album("a", "Alpha");
    {
        SqliteLibraryStore store(file);
        ASSERT_TRUE(store.save({a}, {}));
    }
    sqlite3* other = nullptr;
    ASSERT_EQ(sqlite3_open(file.c_str(), &other), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(other, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
    {
        SqliteLibraryStore busy(file); // another program is writing
        EXPECT_TRUE(busy.load().empty());
        EXPECT_FALSE(busy.save({album("b", "Beta")}, {}));
    }
    sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(other);
    EXPECT_FALSE(fs::exists(SqliteLibraryStore::asidePath(file)));
    SqliteLibraryStore later(file);
    ASSERT_EQ(later.load().size(), 1u);
    EXPECT_EQ(later.load()[0], a);
}

TEST_F(SqliteLibraryStoreTest, AFolderInPlaceOfTheFileIsLeftAlone) {
    fs::create_directories(file);
    write(file / "keep.txt", "mine");
    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.load().empty());
    EXPECT_FALSE(store.save({album("a", "Alpha")}, {}));
    EXPECT_TRUE(fs::is_directory(file));
    EXPECT_EQ(slurp(file / "keep.txt"), "mine");
    EXPECT_FALSE(fs::exists(SqliteLibraryStore::asidePath(file)));
}

TEST_F(SqliteLibraryStoreTest, SavingNothingIsSuccess) {
    SqliteLibraryStore store(file);
    EXPECT_TRUE(store.save({}, {}));
}

TEST_F(SqliteLibraryStoreTest, LibraryIndexRoundTripsOnTheRealStore) {
    const std::string key = "music_example_com-0123456789abcdef";
    std::string ref = LibraryIndex::seenRef(key, SeenKind::Album, "3a1f9c");
    std::string trackRef = LibraryIndex::seenRef(key, SeenKind::Track, "t1");
    {
        SqliteLibraryStore store(file);
        LibraryIndex index(store);
        EXPECT_TRUE(index.items().empty());
        IndexItem a = album("a", "Альбом \"A\"\nline two");
        a.tracks[0].seenOn = {trackRef};
        index.upsert(a, 1800000000);
        index.upsert(album("b", "Beta"), 1800000000);
        index.upsert(album("c", "Gamma"), 1800000000);
        ASSERT_TRUE(index.addSeenOn("a", ref));
        EXPECT_TRUE(index.save());
    }
    {
        SqliteLibraryStore store(file);
        LibraryIndex index(store);
        ASSERT_EQ(index.items().size(), 3u);
        ASSERT_NE(index.find("a"), nullptr);
        EXPECT_EQ(index.find("a")->title, "Альбом \"A\"\nline two");
        EXPECT_EQ(index.findBySeen(ref), index.find("a"));
        EXPECT_EQ(index.trackBySeen(trackRef).item, index.find("a"));
        EXPECT_EQ(index.findByLocation("/music/b/2.mp3").item, index.find("b"));
        EXPECT_EQ(LibraryIndex::refOnServer(index.find("a")->seenOn, key), ref);
        EXPECT_EQ(index.find("b")->firstSeen, 1700000100); // kept as saved, not reset to the new time

        EXPECT_TRUE(index.remove("c"));
        index.upsert(album("b", "Beta, second edition"), 1800000500);
        EXPECT_TRUE(index.save());
    }
    SqliteLibraryStore store(file);
    LibraryIndex index(store);
    EXPECT_EQ(index.items().size(), 2u);
    EXPECT_EQ(index.find("c"), nullptr);
    ASSERT_NE(index.find("b"), nullptr);
    EXPECT_EQ(index.find("b")->title, "Beta, second edition");
}
