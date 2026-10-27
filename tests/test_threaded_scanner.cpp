// ThreadedScanner on a temp folder with a FakeTagReader: what it lists, what it reads
// (only new and changed files), what it never enters (the cache folder), and that a
// cancelled scan never calls back. FakeLibraryScanner is checked at the end.

#include "core/library_index.h"
#include "fakes.h"
#include "frontend/threaded_scanner.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace oap;
using oap::test::FakeLibraryScanner;
using oap::test::FakeTagReader;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& contents = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
}

TrackTags album(const std::string& artist, const std::string& albumName, int track, const std::string& title = "") {
    TrackTags t;
    t.albumArtist = artist;
    t.artist = artist;
    t.album = albumName;
    t.track = track;
    t.title = title;
    t.duration = 60;
    return t;
}

// Runs a scan and waits for its callback; none if it did not come in time.
std::optional<std::vector<IndexItem>> runScan(ThreadedScanner& scanner, ScanRequest request) {
    std::optional<std::vector<IndexItem>> result;
    scanner.scan(std::move(request), [&](std::vector<IndexItem> items) { result = std::move(items); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        scanner.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}

const IndexItem* byId(const std::vector<IndexItem>& items, const std::string& id) {
    for (const auto& item : items)
        if (item.id == id) return &item;
    return nullptr;
}

std::vector<std::string> locations(const IndexItem& item) {
    std::vector<std::string> out;
    for (const auto& t : item.tracks) out.push_back(t.location);
    return out;
}

// A reader that throws, as a failing filesystem call or an allocation could.
class ThrowingTagReader : public FakeTagReader {
public:
    std::optional<TrackTags> read(const fs::path& file) override {
        if (file.filename() == "bad.mp3") {
            ++thrown;
            throw std::runtime_error("boom");
        }
        return FakeTagReader::read(file);
    }
    std::atomic<int> thrown{0};
};

// A reader whose reads wait until the test lets them go, to hold a scan in the middle.
class GatedTagReader : public FakeTagReader {
public:
    std::optional<TrackTags> read(const fs::path& file) override {
        {
            std::unique_lock lock(m_);
            ++waiting_;
            cv_.notify_all();
            cv_.wait(lock, [this] { return open_; });
            --waiting_;
        }
        return FakeTagReader::read(file);
    }
    void waitUntilReading() {
        std::unique_lock lock(m_);
        ASSERT_TRUE(cv_.wait_for(lock, std::chrono::seconds(5), [this] { return waiting_ > 0; }));
    }
    void release() {
        std::lock_guard lock(m_);
        open_ = true;
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int waiting_ = 0;
    bool open_ = false;
};

} // namespace

class ThreadedScannerTest : public testing::Test {
protected:
    void SetUp() override {
        root = test::testTempDir();
        fs::remove_all(root);
        library = root / "library";
        music = root / "music";
        cache = music / "cache";
        fs::create_directories(library);
        fs::create_directories(music);
    }
    void TearDown() override { fs::remove_all(root); }

    ScanRequest request(std::vector<IndexItem> previous = {}) const { return {library, music, cache, std::move(previous)}; }

    fs::path root, library, music, cache;
    FakeTagReader reader;
};

TEST_F(ThreadedScannerTest, BooksStayFoldersAndMusicIsGroupedByTags) {
    writeFile(library / "Book A" / "02.mp3");
    writeFile(library / "Book A" / "01.mp3");
    writeFile(library / "Loose.m4b");
    writeFile(music / "disc1" / "01.mp3");
    writeFile(music / "disc2" / "01.mp3");
    writeFile(music / "other" / "a.flac");
    reader.tags[library / "Book A" / "01.mp3"] = album("Narrator", "Not music", 1, "Chapter one");
    reader.tags[music / "disc1" / "01.mp3"] = album("Band", "Record", 1, "First");
    reader.tags[music / "disc2" / "01.mp3"] = album("Band", "Record", 2, "Second");
    reader.tags[music / "other" / "a.flac"] = album("Solo", "Single", 1);
    reader.tags[music / "other" / "a.flac"].albumArtist.clear(); // the artist stands in for it
    ThreadedScanner scanner(reader);

    auto items = runScan(scanner, request());
    ASSERT_TRUE(items);
    EXPECT_EQ(items->size(), 4u) << "two books and two albums";

    const IndexItem* book = byId(*items, "local:book:" + (library / "Book A").string());
    ASSERT_TRUE(book);
    EXPECT_EQ(book->kind, IndexItem::Kind::Book);
    EXPECT_EQ(book->source, IndexItem::Source::Local);
    EXPECT_EQ(book->title, "Book A") << "a book is titled by its folder, whatever the tags say";
    EXPECT_EQ(locations(*book), (std::vector<std::string>{(library / "Book A" / "01.mp3").string(),
                                                           (library / "Book A" / "02.mp3").string()}));
    EXPECT_EQ(book->tracks[0].title, "Chapter one");
    EXPECT_EQ(book->tracks[1].title, "02") << "no tags: the file name";
    ASSERT_TRUE(byId(*items, "local:book:" + (library / "Loose.m4b").string()));

    const IndexItem* record = byId(*items, std::string("local:album:Band") + '\x1f' + "Record");
    ASSERT_TRUE(record) << "two folders tagged as one album are one item";
    EXPECT_EQ(record->kind, IndexItem::Kind::Music);
    EXPECT_EQ(record->title, "Record");
    EXPECT_EQ(record->artist, "Band");
    EXPECT_EQ(record->album, "Record");
    EXPECT_EQ(locations(*record), (std::vector<std::string>{(music / "disc1" / "01.mp3").string(),
                                                             (music / "disc2" / "01.mp3").string()}));
    EXPECT_EQ(record->tracks[1].number, 2);
    EXPECT_EQ(record->tracks[1].duration, 60);
    EXPECT_EQ(record->tracks[0].size, 1);
    EXPECT_EQ(record->firstSeen, 0);
    const IndexItem* single = byId(*items, std::string("local:album:Solo") + '\x1f' + "Single");
    ASSERT_TRUE(single) << "without an album artist the artist groups the album";
    EXPECT_EQ(single->artist, "Solo");
}

TEST_F(ThreadedScannerTest, IdsAreTheSameInEveryScan) {
    writeFile(library / "Book" / "a.mp3");
    writeFile(music / "disc1" / "01.mp3");
    writeFile(music / "plain" / "x.mp3");
    reader.tags[music / "disc1" / "01.mp3"] = album("Band", "Record", 1);
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    auto second = runScan(scanner, request()); // no previous: everything is read again
    ASSERT_TRUE(first && second);
    EXPECT_EQ(*first, *second);
    EXPECT_TRUE(byId(*first, "local:folder:" + (music / "plain").string()));
}

TEST_F(ThreadedScannerTest, UnchangedFilesAreNotReadAgain) {
    writeFile(library / "Book" / "a.mp3");
    writeFile(music / "disc1" / "01.mp3");
    writeFile(music / "disc1" / "02.mp3");
    reader.tags[music / "disc1" / "01.mp3"] = album("Band", "Record", 1);
    reader.tags[music / "disc1" / "02.mp3"] = album("Band", "Record", 2);
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);
    ASSERT_EQ(reader.totalReads(), 3);

    auto second = runScan(scanner, request(*first));
    ASSERT_TRUE(second);
    EXPECT_EQ(reader.totalReads(), 3) << "no file was read again, also the one that has no tags";
    EXPECT_EQ(*second, *first) << "the kept entries give the same items";
}

TEST_F(ThreadedScannerTest, ChangedFileIsReadAgainAndTheOthersAreNot) {
    writeFile(music / "d" / "01.mp3", "aaa");
    writeFile(music / "d" / "02.mp3", "bbb");
    writeFile(music / "d" / "03.mp3", "ccc");
    reader.tags[music / "d" / "01.mp3"] = album("Band", "Record", 1);
    reader.tags[music / "d" / "02.mp3"] = album("Band", "Record", 2);
    reader.tags[music / "d" / "03.mp3"] = album("Band", "Record", 3);
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);

    writeFile(music / "d" / "01.mp3", "aaaa");                  // another size
    fs::last_write_time(music / "d" / "02.mp3",                 // same size, newer
                        fs::last_write_time(music / "d" / "02.mp3") + std::chrono::seconds(10));
    reader.tags[music / "d" / "01.mp3"].title = "Renamed"; // set while no scan runs
    auto second = runScan(scanner, request(*first));
    ASSERT_TRUE(second);
    EXPECT_EQ(reader.reads(music / "d" / "01.mp3"), 2) << "a changed size";
    EXPECT_EQ(reader.reads(music / "d" / "02.mp3"), 2) << "a changed file time";
    EXPECT_EQ(reader.reads(music / "d" / "03.mp3"), 1) << "not changed";
    ASSERT_EQ(second->size(), 1u) << "kept and re-read tracks stay in one album";
    EXPECT_EQ(second->front().tracks.size(), 3u);
    EXPECT_EQ(second->front().tracks[0].title, "Renamed");
    EXPECT_EQ(second->front().tracks[0].size, 4);
}

TEST_F(ThreadedScannerTest, KeptTracksKeepTheirAlbumData) {
    writeFile(music / "d" / "01.mp3");
    TrackTags t = album("Band", "Record", 1);
    t.genre = "Rock";
    t.year = 1999;
    t.musicBrainzAlbumId = "mbid";
    reader.tags[music / "d" / "01.mp3"] = t;
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);
    auto second = runScan(scanner, request(*first));
    ASSERT_TRUE(second);
    ASSERT_EQ(second->size(), 1u);
    EXPECT_EQ(second->front().genre, "Rock");
    EXPECT_EQ(second->front().year, 1999);
    EXPECT_EQ(second->front().musicBrainzId, "mbid");
    EXPECT_EQ(second->front().album, "Record");
}

TEST_F(ThreadedScannerTest, AnAlbumIsInTrackNumberOrderAndFolderOrder) {
    writeFile(music / "CD10" / "a.mp3");
    writeFile(music / "CD2" / "z.mp3");
    writeFile(music / "CD2" / "a.mp3");
    writeFile(music / "CD2" / "m.mp3");
    reader.tags[music / "CD10" / "a.mp3"] = album("Band", "Record", 1);
    reader.tags[music / "CD2" / "z.mp3"] = album("Band", "Record", 1);
    reader.tags[music / "CD2" / "a.mp3"] = album("Band", "Record", 2);
    // m.mp3 has an album tag but no number: after the numbered ones of its folder.
    reader.tags[music / "CD2" / "m.mp3"] = album("Band", "Record", 0);
    ThreadedScanner scanner(reader);
    auto items = runScan(scanner, request());
    ASSERT_TRUE(items);
    ASSERT_EQ(items->size(), 1u);
    EXPECT_EQ(locations(items->front()),
              (std::vector<std::string>{(music / "CD2" / "z.mp3").string(), (music / "CD2" / "a.mp3").string(),
                                        (music / "CD2" / "m.mp3").string(), (music / "CD10" / "a.mp3").string()}));
}

TEST_F(ThreadedScannerTest, ARetaggedFileWithFewerTagsKeepsTheAlbumData) {
    writeFile(music / "d" / "01.mp3", "a");
    writeFile(music / "d" / "02.mp3", "b");
    TrackTags full = album("Band", "Record", 1);
    full.genre = "Rock";
    full.year = 1999;
    full.musicBrainzAlbumId = "mbid";
    reader.tags[music / "d" / "01.mp3"] = full;
    reader.tags[music / "d" / "02.mp3"] = full;
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);
    writeFile(music / "d" / "02.mp3", "bb");
    reader.tags[music / "d" / "02.mp3"] = album("Band", "Record", 2); // no genre, year or id
    auto second = runScan(scanner, request(*first));
    ASSERT_TRUE(second);
    ASSERT_EQ(second->size(), 1u);
    EXPECT_EQ(second->front().genre, "Rock");
    EXPECT_EQ(second->front().year, 1999);
    EXPECT_EQ(second->front().musicBrainzId, "mbid");
}

TEST_F(ThreadedScannerTest, RemovedFilesAndFoldersLeaveTheIndex) {
    writeFile(library / "Gone" / "a.mp3");
    writeFile(library / "Stays" / "a.mp3");
    writeFile(music / "d" / "01.mp3");
    writeFile(music / "d" / "02.mp3");
    reader.tags[music / "d" / "01.mp3"] = album("Band", "Record", 1);
    reader.tags[music / "d" / "02.mp3"] = album("Band", "Record", 2);
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);
    fs::remove_all(library / "Gone");
    fs::remove(music / "d" / "02.mp3");

    auto second = runScan(scanner, request(*first));
    ASSERT_TRUE(second);
    EXPECT_FALSE(byId(*second, "local:book:" + (library / "Gone").string()));
    EXPECT_TRUE(byId(*second, "local:book:" + (library / "Stays").string()));
    const IndexItem* record = byId(*second, std::string("local:album:Band") + '\x1f' + "Record");
    ASSERT_TRUE(record);
    EXPECT_EQ(locations(*record), std::vector<std::string>{(music / "d" / "01.mp3").string()});

    // And once the result goes to a LibraryIndex, the removed item is gone from the store.
    test::FakeLibraryStore store;
    LibraryIndex index(store);
    index.replaceSource(IndexItem::Source::Local, *first, 100);
    index.replaceSource(IndexItem::Source::Local, *second, 200);
    EXPECT_FALSE(index.find("local:book:" + (library / "Gone").string()));
}

TEST_F(ThreadedScannerTest, TheCacheFolderIsNeverEntered) {
    writeFile(music / "album" / "01.mp3");
    writeFile(cache / "server" / "album%3A1" / "01.mp3");
    writeFile(library / "Book" / "a.mp3");
    ThreadedScanner scanner(reader);
    auto items = runScan(scanner, request());
    ASSERT_TRUE(items);
    EXPECT_EQ(items->size(), 2u);
    EXPECT_EQ(reader.reads(cache / "server" / "album%3A1" / "01.mp3"), 0);
    for (const auto& item : *items)
        for (const auto& t : item.tracks) EXPECT_EQ(t.location.find(cache.string()), std::string::npos) << t.location;
}

TEST_F(ThreadedScannerTest, AFileWithoutReadableTagsIsStillListed) {
    writeFile(library / "Book" / "Part 1.mp3");
    writeFile(music / "Some Folder" / "track one.mp3");
    writeFile(music / "Some Folder" / "track two.mp3");
    // Readable, but without an album: grouped by folder too.
    writeFile(music / "Some Folder" / "track three.mp3");
    TrackTags noAlbum;
    noAlbum.title = "Third";
    reader.tags[music / "Some Folder" / "track three.mp3"] = noAlbum;
    ThreadedScanner scanner(reader);
    auto items = runScan(scanner, request());
    ASSERT_TRUE(items);

    const IndexItem* book = byId(*items, "local:book:" + (library / "Book").string());
    ASSERT_TRUE(book);
    EXPECT_EQ(book->title, "Book");
    ASSERT_EQ(book->tracks.size(), 1u);
    EXPECT_EQ(book->tracks[0].title, "Part 1");

    const IndexItem* folder = byId(*items, "local:folder:" + (music / "Some Folder").string());
    ASSERT_TRUE(folder);
    EXPECT_EQ(folder->kind, IndexItem::Kind::Music);
    EXPECT_EQ(folder->title, "Some Folder");
    ASSERT_EQ(folder->tracks.size(), 3u);
    EXPECT_EQ(folder->tracks[0].title, "track one");
    EXPECT_EQ(folder->tracks[1].title, "Third") << "\"track three\" sorts before \"track two\"";
    EXPECT_EQ(folder->tracks[2].title, "track two");
    EXPECT_EQ(folder->tracks[0].duration, 0);
}

TEST_F(ThreadedScannerTest, OnlyLocalPreviousItemsAreKept) {
    writeFile(music / "d" / "01.mp3");
    ThreadedScanner scanner(reader);
    auto first = runScan(scanner, request());
    ASSERT_TRUE(first);
    ASSERT_EQ(first->size(), 1u);
    IndexItem cached = first->front();
    cached.source = IndexItem::Source::Cached;
    auto second = runScan(scanner, request({cached}));
    ASSERT_TRUE(second);
    EXPECT_EQ(reader.reads(music / "d" / "01.mp3"), 2) << "a downloaded copy says nothing about this file";
}

TEST_F(ThreadedScannerTest, ANonRegularFileIsNeverOpened) {
    writeFile(music / "d" / "01.mp3");
    ASSERT_EQ(::mkfifo((music / "d" / "02.mp3").c_str(), 0600), 0); // reading it would block
    ThreadedScanner scanner(reader);
    auto items = runScan(scanner, request());
    ASSERT_TRUE(items);
    EXPECT_EQ(reader.reads(music / "d" / "02.mp3"), 0);
    ASSERT_EQ(items->size(), 1u);
    EXPECT_EQ(items->front().tracks.size(), 1u);
}

TEST_F(ThreadedScannerTest, AfterCancelTheCallbackDoesNotRun) {
    writeFile(library / "Book" / "a.mp3");
    GatedTagReader gated;
    ThreadedScanner scanner(gated);
    bool cancelledRan = false;
    auto id = scanner.scan(request(), [&](std::vector<IndexItem>) { cancelledRan = true; });
    gated.waitUntilReading(); // the scan is running
    scanner.cancel(id);
    gated.release();
    // A scan after it runs after it, so when this one is delivered the cancelled one is over.
    auto next = runScan(scanner, request());
    ASSERT_TRUE(next);
    scanner.poll();
    EXPECT_FALSE(cancelledRan);
}

TEST_F(ThreadedScannerTest, CancelAfterTheScanFinishedBeforePollDropsTheResult) {
    writeFile(library / "Book" / "a.mp3");
    ThreadedScanner scanner(reader);
    bool cancelledRan = false;
    auto id = scanner.scan(request(), [&](std::vector<IndexItem>) { cancelledRan = true; });
    for (int i = 0; i < 5000 && reader.totalReads() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // finished, waiting for poll()
    scanner.cancel(id);
    scanner.poll();
    EXPECT_FALSE(cancelledRan);
}

TEST_F(ThreadedScannerTest, ACancelledQueuedScanNeverStarts) {
    writeFile(library / "Book" / "a.mp3");
    GatedTagReader gated;
    ThreadedScanner scanner(gated);
    bool firstRan = false, secondRan = false;
    scanner.scan(request(), [&](std::vector<IndexItem>) { firstRan = true; });
    gated.waitUntilReading();
    auto second = scanner.scan(request(), [&](std::vector<IndexItem>) { secondRan = true; });
    scanner.cancel(second);
    gated.release();
    for (int i = 0; i < 5000 && !firstRan; ++i) {
        scanner.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(firstRan);
    EXPECT_FALSE(secondRan);
    EXPECT_EQ(gated.totalReads(), 1) << "the cancelled scan read nothing";
}

TEST_F(ThreadedScannerTest, ACancelledRunningScanStopsReading) {
    for (int i = 0; i < 5; ++i) writeFile(music / "d" / (std::to_string(i) + ".mp3"));
    GatedTagReader gated;
    ThreadedScanner scanner(gated);
    auto id = scanner.scan(request(), [](std::vector<IndexItem>) {});
    gated.waitUntilReading();
    scanner.cancel(id);
    gated.release();
    auto next = runScan(scanner, ScanRequest{library, library, cache, {}}); // empty folder: reads nothing
    ASSERT_TRUE(next);
    EXPECT_EQ(gated.totalReads(), 1) << "it stopped after the file it was reading";
}

TEST_F(ThreadedScannerTest, DestroyingTheScannerDuringAScanDoesNotHangOrCallBack) {
    for (int i = 0; i < 3; ++i) writeFile(music / "d" / (std::to_string(i) + ".mp3"));
    GatedTagReader gated;
    bool ran = false;
    std::thread releaser;
    {
        ThreadedScanner scanner(gated);
        scanner.scan(request(), [&](std::vector<IndexItem>) { ran = true; });
        gated.waitUntilReading();
        releaser = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            gated.release();
        });
    }
    releaser.join();
    EXPECT_FALSE(ran);
    EXPECT_EQ(gated.totalReads(), 1);
}

TEST_F(ThreadedScannerTest, ACallbackMayStartAndCancelScans) {
    writeFile(library / "Book" / "a.mp3");
    ThreadedScanner scanner(reader);
    int firstDone = 0, secondDone = 0;
    scanner.scan(request(), [&](std::vector<IndexItem>) {
        ++firstDone;
        scanner.scan(request(), [&](std::vector<IndexItem>) { ++secondDone; });
    });
    for (int i = 0; i < 5000 && secondDone == 0; ++i) {
        scanner.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(firstDone, 1);
    EXPECT_EQ(secondDone, 1);
}

TEST_F(ThreadedScannerTest, AReaderThatThrowsDropsThatScanAndTheNextOneRuns) {
    writeFile(library / "Book" / "bad.mp3");
    ThrowingTagReader throwing;
    ThreadedScanner scanner(throwing);
    bool ran = false;
    scanner.scan(request(), [&](std::vector<IndexItem>) { ran = true; });
    for (int i = 0; i < 5000 && throwing.thrown == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(throwing.thrown, 1);
    fs::remove(library / "Book" / "bad.mp3");
    writeFile(library / "Other" / "ok.mp3");
    auto second = runScan(scanner, request()); // jobs run in order, so the first is over by now
    ASSERT_TRUE(second);
    scanner.poll();
    EXPECT_FALSE(ran) << "a scan that failed delivers nothing, it must not look like an empty library";
    EXPECT_TRUE(byId(*second, "local:book:" + (library / "Other").string()));
}

TEST(FakeLibraryScanner, DeliversOnlyFromPollAfterFinishAndOnce) {
    FakeLibraryScanner scanner;
    std::vector<std::size_t> sizes;
    ScanRequest request;
    request.musicRoot = "/m";
    auto id = scanner.scan(request, [&](std::vector<IndexItem> items) { sizes.push_back(items.size()); });
    EXPECT_EQ(scanner.lastRequest().request.musicRoot, "/m");
    scanner.poll();
    EXPECT_TRUE(sizes.empty()) << "nothing is delivered until the test says so";
    ASSERT_TRUE(scanner.finish(id, {IndexItem{}, IndexItem{}}));
    EXPECT_TRUE(sizes.empty()) << "and then only from poll()";
    scanner.poll();
    EXPECT_EQ(sizes, std::vector<std::size_t>{2});
    scanner.poll();
    EXPECT_EQ(sizes.size(), 1u);
    EXPECT_FALSE(scanner.finish(id, {})) << "a finished scan is not pending";
}

TEST(FakeLibraryScanner, CancelDropsAPendingAndAnAnsweredScan) {
    FakeLibraryScanner scanner;
    int calls = 0;
    auto a = scanner.scan({}, [&](std::vector<IndexItem>) { ++calls; });
    auto b = scanner.scan({}, [&](std::vector<IndexItem>) { ++calls; });
    scanner.cancel(a);
    EXPECT_FALSE(scanner.finish(a, {})) << "a late answer for a cancelled scan";
    ASSERT_TRUE(scanner.finish(b, {}));
    scanner.cancel(b); // answered, not yet delivered
    scanner.poll();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(scanner.pendingCount(), 0u);
    EXPECT_EQ(scanner.cancelled, (std::vector<LibraryScanner::RequestId>{a, b}));
}
