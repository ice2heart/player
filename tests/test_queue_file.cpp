#include "core/queue_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace oap;

namespace {

QueueEntry local(const std::string& title, const std::string& album = "") {
    QueueEntry e;
    e.path = "/music/" + title + ".mp3";
    e.title = title;
    e.album = album;
    e.artist = "Band";
    return e;
}

QueueEntry remote(const std::string& id, const std::string& title) {
    QueueEntry e;
    e.kind = QueueEntry::Kind::Remote;
    e.serverUrl = "https://music.example";
    e.user = "ann";
    e.trackId = id;
    e.albumId = "al" + id;
    e.title = title;
    e.album = "Weather";
    e.artist = "Band";
    e.duration = 215.5;
    return e;
}

std::string titles(const Queue& q) {
    std::string s;
    for (std::size_t i = 0; i < q.size(); ++i) s += q.at(i).title + ",";
    return s;
}

Queue mixed() {
    Queue q;
    q.replace({local("a"), remote("t2", "b"), local("c"), remote("t4", "d")}, 1);
    q.setShuffle(QueueShuffle::Tracks, [](std::size_t) { return std::size_t{0}; });
    q.setRepeat(QueueRepeat::All);
    q.append({local("e")}, [](std::size_t) { return std::size_t{0}; });
    return q;
}

} // namespace

TEST(QueueFile, KeepsQueueSyncMetadataAndEmptyPendingQueue) {
    SavedQueue saved{"Mixed", mixed(), 4};
    saved.changed = 1700000000000LL;
    saved.pendingServers = {"server-a", "server-b"};
    saved.acknowledgedServers = {{"server-a", 1700000001234LL}};
    auto restored = parseQueue(formatQueue(saved));
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->changed, saved.changed);
    EXPECT_EQ(restored->pendingServers, saved.pendingServers);
    EXPECT_EQ(restored->acknowledgedServers, saved.acknowledgedServers);
    saved.queue = Queue{};
    restored = parseQueue(formatQueue(saved));
    ASSERT_TRUE(restored);
    EXPECT_TRUE(restored->queue.empty());
    EXPECT_EQ(restored->pendingServers, saved.pendingServers);
}

TEST(QueueFile, RejectsMalformedQueueSyncTimestamps) {
    const std::string base = formatQueue({"Mixed", mixed(), 0});
    EXPECT_FALSE(parseQueue(base + "changed\t123junk\n"));
    EXPECT_FALSE(parseQueue(base + "ack\tserver\t-1\n"));
    EXPECT_FALSE(parseQueue(base + "ack\tserver\t99999999999999999999\n"));
}

TEST(QueueFile, RoundTripsAMixedShuffledQueue) {
    Queue q = mixed();
    auto back = parseQueue(formatQueue({"Weather", q, 42.5}));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->title, "Weather");
    EXPECT_DOUBLE_EQ(back->position, 42.5);
    EXPECT_EQ(back->queue.originalEntries(), q.originalEntries());
    EXPECT_EQ(back->queue.playOrder(), q.playOrder());
    EXPECT_EQ(back->queue.position(), q.position());
    EXPECT_EQ(back->queue.shuffle(), QueueShuffle::Tracks);
    EXPECT_EQ(back->queue.repeat(), QueueRepeat::All);
    EXPECT_TRUE(back->queue.edited());
}

TEST(QueueFile, KeepsAlbumsShuffleAndRepeatTrack) {
    Queue q;
    q.replace({local("a", "X"), local("b", "X"), local("c", "Y")}, 0);
    q.setShuffle(QueueShuffle::Albums, [](std::size_t) { return std::size_t{0}; });
    q.setRepeat(QueueRepeat::Track);
    auto back = parseQueue(formatQueue({"Q", q, 0}));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->queue.shuffle(), QueueShuffle::Albums);
    EXPECT_EQ(back->queue.repeat(), QueueRepeat::Track);
    EXPECT_FALSE(back->queue.edited());
}

TEST(QueueFile, KeepsRepeatAlbumAndWritesItAsAlbum) {
    Queue q;
    q.replace({local("a", "X"), local("b", "X")}, 0);
    q.setRepeat(QueueRepeat::Album);
    const std::string text = formatQueue({"Q", q, 0});
    EXPECT_NE(text.find("repeat\talbum\n"), std::string::npos);
    auto back = parseQueue(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->queue.repeat(), QueueRepeat::Album);
}

TEST(QueueFile, AnUnknownRepeatValueReadsAsOff) {
    Queue q;
    q.replace({local("a", "X"), local("b", "X")}, 0);
    q.setRepeat(QueueRepeat::Album);
    std::string text = formatQueue({"Q", q, 0});
    const std::size_t at = text.find("repeat\talbum");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 12, "repeat\tmagic");
    auto back = parseQueue(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->queue.repeat(), QueueRepeat::Off);
}

TEST(QueueFile, TabsNewlinesAndBackslashesInTextSurvive) {
    QueueEntry e = local("Wind\tand\nRain\r\\n end\\");
    e.album = "Al\tbum";
    e.artist = "Ar\nt";
    e.path = "/music/we\tird\\name.mp3";
    Queue q;
    q.replace({e, remote("t1", "Re\tmote\ntitle")});
    auto text = formatQueue({"List\twith\ntab", q, 1});
    // One line per record: nothing the user typed can split a line.
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 10);
    auto back = parseQueue(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->title, "List\twith\ntab");
    EXPECT_EQ(back->queue.originalEntries(), q.originalEntries());
}

TEST(QueueFile, AnEmptyQueueIsNoFile) {
    EXPECT_EQ(formatQueue({"Empty", Queue{}, 0}), "");
    EXPECT_FALSE(parseQueue(""));
}

TEST(QueueFile, TheFileHoldsTheServerKeyAndTheTrackIdButNothingToLogInWith) {
    Queue q;
    q.replace({remote("t1", "Rain")});
    const std::string text = formatQueue({"Weather", q, 0});
    EXPECT_NE(text.find("https://music.example"), std::string::npos);
    EXPECT_NE(text.find("\tt1\t"), std::string::npos);
    for (const char* secret : {"t=", "s=", "u=", "password", "stream"}) EXPECT_EQ(text.find(secret), std::string::npos) << secret;
}

TEST(QueueFile, ALongQueueIsCutAroundTheCurrentEntry) {
    std::vector<QueueEntry> entries;
    for (std::size_t i = 0; i < kMaxSavedEntries + 1000; ++i) entries.push_back(local(std::to_string(i)));
    Queue q;
    q.replace(std::move(entries), 3000);
    auto back = parseQueue(formatQueue({"Big", q, 7}));
    ASSERT_TRUE(back);
    ASSERT_EQ(back->queue.size(), kMaxSavedEntries);
    EXPECT_EQ(back->queue.current()->title, "3000");
    EXPECT_EQ(back->queue.at(0).title, std::to_string(3000 - kMaxSavedEntries / 2));
    EXPECT_EQ(back->queue.at(kMaxSavedEntries - 1).title, std::to_string(3000 - kMaxSavedEntries / 2 + kMaxSavedEntries - 1));
}

TEST(QueueFile, NoCutForAQueueAtTheLimit) {
    std::vector<QueueEntry> entries;
    for (std::size_t i = 0; i < kMaxSavedEntries; ++i) entries.push_back(local(std::to_string(i)));
    Queue q;
    q.replace(std::move(entries), 0);
    auto back = parseQueue(formatQueue({"Big", q, 0}));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->queue.size(), kMaxSavedEntries);
    EXPECT_EQ(back->queue.at(kMaxSavedEntries - 1).title, std::to_string(kMaxSavedEntries - 1));
}

TEST(QueueFile, TextThatIsNotAQueueGivesNone) {
    const std::string good = formatQueue({"Q", mixed(), 0});
    ASSERT_TRUE(parseQueue(good));
    EXPECT_FALSE(parseQueue("hello"));
    EXPECT_FALSE(parseQueue(std::string("\0\xff\xfe garbage \t\t\n", 17)));
    EXPECT_FALSE(parseQueue("oap-queue\t2\nL\t/a.mp3\ta\tb\tc\n")) << "another version";
    EXPECT_FALSE(parseQueue("oap-queue\t1\ntitle\tNothing\n")) << "no entries";
    EXPECT_FALSE(parseQueue("oap-queue\t1\nL\t/a.mp3\ta\n")) << "an entry with fields missing";
    EXPECT_FALSE(parseQueue("oap-queue\t1\nR\thttps://x\tann\tt1\n")) << "a remote entry with fields missing";
    EXPECT_FALSE(parseQueue(good.substr(0, good.find("\nR\t") + 6))) << "cut off in the middle of an entry";
}

TEST(QueueFile, MoreEntriesThanAreEverSavedIsNotOurFile) {
    std::string text = "oap-queue\t1\n";
    for (std::size_t i = 0; i <= kMaxSavedEntries; ++i) text += "L\t/a.mp3\ta\tb\tc\n";
    EXPECT_FALSE(parseQueue(text));
}

TEST(QueueFile, AWrongPlayOrderGivesTheOriginalOrder) {
    for (const char* order : {"order\t0\t0\t1", "order\t0\t1", "order\t0\t1\t5", "order\t0\t1\tx", "order\t2\t1\t-1", "order"}) {
        auto back = parseQueue(std::string("oap-queue\t1\nshuffle\ttracks\nposition\t1\nseconds\t30\n") + order +
                               "\nL\t/a.mp3\ta\t\t\nL\t/b.mp3\tb\t\t\nL\t/c.mp3\tc\t\t\n");
        ASSERT_TRUE(back) << order;
        EXPECT_EQ(titles(back->queue), "a,b,c,") << order;
        EXPECT_EQ(back->queue.shuffle(), QueueShuffle::Off) << order;
        EXPECT_EQ(back->queue.position(), 0u) << order << ": which entry was current is not known";
        EXPECT_EQ(back->position, 0.0) << order << ": nor where in it";
    }
}

TEST(QueueFile, AWrongPlayOrderDropsTheSecondsEvenWhenThePositionWasZero) {
    for (const char* order : {"order\t0\t0\t1", "order\t2\t0\t1"}) { // the second one is valid, but not with shuffle off
        auto back = parseQueue(std::string("oap-queue\t1\nshuffle\toff\nposition\t0\nseconds\t30\n") + order +
                               "\nL\t/a.mp3\ta\t\t\nL\t/b.mp3\tb\t\t\nL\t/c.mp3\tc\t\t\n");
        ASSERT_TRUE(back) << order;
        EXPECT_EQ(back->queue.position(), 0u) << order;
        EXPECT_EQ(back->position, 0.0) << order << ": the seconds were of another entry";
    }
}

TEST(QueueFile, APositionPastTheEndGivesTheFirstEntryAndBadNumbersGiveZero) {
    for (const char* position : {"position\t3", "position\t99999999999999999999", "position\t-1", "position\tx"}) {
        auto back = parseQueue(std::string("oap-queue\t1\nseconds\t30\n") + position + "\nL\t/a.mp3\ta\t\t\nL\t/b.mp3\tb\t\t\nL\t/c.mp3\tc\t\t\n");
        ASSERT_TRUE(back) << position;
        EXPECT_EQ(back->queue.position(), 0u) << position;
        EXPECT_EQ(back->position, 0.0) << position << ": the seconds were not of the first entry";
    }
}

TEST(QueueFile, ANegativeOrNonNumericDurationOrPlaceIsZero) {
    auto back = parseQueue("oap-queue\t1\nseconds\t-5\nR\thttps://x\tann\tt1\tal1\tRain\tW\tB\tinf\n");
    ASSERT_TRUE(back);
    EXPECT_EQ(back->position, 0.0);
    EXPECT_EQ(back->queue.at(0).duration, 0.0);
}

TEST(QueueFile, AnAudiobookshelfFileIdRoundTripsAsATrackId) {
    QueueEntry e = remote("6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f/12345", "Part 1");
    e.albumId = "6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f";
    Queue q;
    q.replace({e}, 0);
    auto back = parseQueue(formatQueue({"Book", q, 3}));
    ASSERT_TRUE(back);
    ASSERT_EQ(back->queue.size(), 1u);
    EXPECT_EQ(back->queue.at(0).trackId, "6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f/12345");
    EXPECT_EQ(back->queue.at(0).albumId, "6f1c2d3e-8a4b-4c5d-9e0f-1a2b3c4d5e6f");
}
