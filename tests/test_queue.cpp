#include "core/queue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace oap;

namespace {

QueueEntry local(const std::string& title, const std::string& album = "") {
    QueueEntry e;
    e.path = "/music/" + title + ".mp3";
    e.title = title;
    e.album = album;
    return e;
}

QueueEntry remote(const std::string& id, const std::string& title) {
    QueueEntry e;
    e.kind = QueueEntry::Kind::Remote;
    e.serverUrl = "https://music.example";
    e.user = "ann";
    e.trackId = id;
    e.title = title;
    e.duration = 200;
    return e;
}

std::vector<QueueEntry> tracks(std::initializer_list<const char*> titles) {
    std::vector<QueueEntry> v;
    for (auto t : titles) v.push_back(local(t));
    return v;
}

// The titles in the play order.
std::string order(const Queue& q) {
    std::string s;
    for (std::size_t i = 0; i < q.size(); ++i) s += q.at(i).title;
    return s;
}

std::string currentTitle(const Queue& q) { return q.current() ? q.current()->title : "-"; }

QueueRandom seeded(unsigned seed) {
    auto gen = std::make_shared<std::mt19937>(seed);
    return [gen](std::size_t bound) { return static_cast<std::size_t>((*gen)() % bound); };
}

// Always picks the first slot: a known permutation.
const QueueRandom kZero = [](std::size_t) { return std::size_t{0}; };

Queue abcde() {
    Queue q;
    q.replace(tracks({"a", "b", "c", "d", "e"}));
    return q;
}

QueueEntry inAlbum(const std::string& title, const std::string& album, const std::string& albumId = "") {
    QueueEntry e = local(title, album);
    e.albumId = albumId;
    return e;
}

// The titles reached by advance() from the current entry, `n` times.
std::string advanced(Queue& q, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        q.advance();
        s += currentTitle(q);
    }
    return s;
}

} // namespace

TEST(Queue, StartsEmpty) {
    Queue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.current(), nullptr);
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
    EXPECT_EQ(q.repeat(), QueueRepeat::Off);
    EXPECT_FALSE(q.edited());
    EXPECT_EQ(q.next(), Queue::Step::End);
    EXPECT_EQ(q.advance(), Queue::Step::End);
}

TEST(Queue, ReplaceStartsAtTheGivenEntryAndClampsIt) {
    Queue q;
    q.replace(tracks({"a", "b", "c"}), 1);
    EXPECT_EQ(currentTitle(q), "b");
    EXPECT_EQ(q.position(), 1u);
    q.replace(tracks({"a", "b", "c"}), 99);
    EXPECT_EQ(currentTitle(q), "c");
}

TEST(Queue, EntriesKeepRemoteIdentityAndLocalPath) {
    Queue q;
    q.replace({local("a"), remote("42", "r")}, 1);
    EXPECT_EQ(q.current()->kind, QueueEntry::Kind::Remote);
    EXPECT_EQ(q.current()->trackId, "42");
    EXPECT_EQ(q.current()->serverUrl, "https://music.example");
    EXPECT_EQ(q.at(0).path, "/music/a.mp3");
}

TEST(Queue, ReplaceResetsShuffleRepeatAndEditedFlag) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, seeded(1));
    q.setRepeat(QueueRepeat::All);
    q.append(tracks({"f"}), kZero);
    ASSERT_TRUE(q.edited());
    q.replace(tracks({"x", "y"}));
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
    EXPECT_EQ(q.repeat(), QueueRepeat::Off);
    EXPECT_FALSE(q.edited());
    EXPECT_EQ(order(q), "xy");
    EXPECT_EQ(currentTitle(q), "x");
}

TEST(Queue, NextAndPreviousWalkThePlayOrder) {
    Queue q = abcde();
    EXPECT_EQ(q.next(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "b");
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.previous(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "b");
}

TEST(Queue, RepeatOffEndsAtTheLastEntryAndGoesBackToTheFirst) {
    Queue q = abcde();
    q.setCurrent(4);
    EXPECT_EQ(q.advance(), Queue::Step::End);
    EXPECT_EQ(currentTitle(q), "a");
    EXPECT_EQ(q.previous(), Queue::Step::End); // nothing before the first
    EXPECT_EQ(currentTitle(q), "a");
}

TEST(Queue, RepeatAllWrapsBothWays) {
    Queue q = abcde();
    q.setRepeat(QueueRepeat::All);
    q.setCurrent(4);
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a");
    EXPECT_EQ(q.previous(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "e");
    q.setCurrent(4);
    EXPECT_EQ(q.next(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a");
}

TEST(Queue, RepeatAllWithOneEntryStaysOnIt) {
    Queue q;
    q.replace(tracks({"a"}));
    q.setRepeat(QueueRepeat::All);
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a");
}

TEST(Queue, RepeatTrackStaysWhenATrackEndsButNextAndPreviousMove) {
    Queue q = abcde();
    q.setRepeat(QueueRepeat::Track);
    q.setCurrent(2);
    EXPECT_EQ(q.advance(), Queue::Step::Repeated);
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.next(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_EQ(q.advance(), Queue::Step::Repeated); // the track moved to is the one that repeats
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_EQ(q.previous(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "c");
}

TEST(Queue, RepeatTrackAtTheLastEntryStillRepeatsIt) {
    Queue q = abcde();
    q.setRepeat(QueueRepeat::Track);
    q.setCurrent(4);
    EXPECT_EQ(q.advance(), Queue::Step::Repeated);
    EXPECT_EQ(currentTitle(q), "e");
}

TEST(Queue, UpcomingIsWhereAdvanceWouldGoAndMovesNothing) {
    Queue q = abcde();
    q.setCurrent(1);
    EXPECT_EQ(q.upcoming(), std::optional<std::size_t>(2));
    EXPECT_EQ(currentTitle(q), "b") << "looking ahead does not move";
    q.setCurrent(4);
    EXPECT_EQ(q.upcoming(), std::nullopt) << "the end of the queue";
    q.setRepeat(QueueRepeat::All);
    EXPECT_EQ(q.upcoming(), std::optional<std::size_t>(0));
    q.setRepeat(QueueRepeat::Track);
    EXPECT_EQ(q.upcoming(), std::optional<std::size_t>(4)) << "the same entry again";
    EXPECT_EQ(Queue{}.upcoming(), std::nullopt);
}

TEST(Queue, SetCurrentOutOfRangeChangesNothing) {
    Queue q = abcde();
    q.setCurrent(1);
    EXPECT_FALSE(q.setCurrent(5));
    EXPECT_EQ(currentTitle(q), "b");
}

TEST(Queue, ShuffleTracksKeepsThePlayedPartAndTheCurrentEntryAndUsesEveryEntryOnce) {
    Queue q = abcde();
    q.setCurrent(2);
    q.setShuffle(QueueShuffle::Tracks, seeded(7));
    EXPECT_EQ(q.shuffle(), QueueShuffle::Tracks);
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.position(), 2u);
    const std::string o = order(q);
    EXPECT_EQ(o.substr(0, 3), "abc"); // what came before stays
    std::string sorted = o;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, "abcde");
    EXPECT_FALSE(q.edited());
}

TEST(Queue, ShuffleTracksFollowsTheGivenGenerator) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero);
    // Current "a" first; the groups b c d e shuffled by Fisher-Yates with 0 each time.
    EXPECT_EQ(order(q), "acdeb");
}

TEST(Queue, ShuffleAlbumsKeepsEachAlbumsOrderAndKeepsAlbumsTogether) {
    Queue q;
    q.replace({local("a1", "A"), local("a2", "A"), local("a3", "A"), local("b1", "B"), local("b2", "B"),
               local("c1", "C"), local("c2", "C")});
    for (unsigned seed = 0; seed < 20; ++seed) {
        q.setShuffle(QueueShuffle::Albums, seeded(seed));
        EXPECT_EQ(currentTitle(q), "a1");
        const std::string o = order(q);
        EXPECT_EQ(o.substr(0, 6), "a1a2a3") << o; // the current album plays first, in order
        bool bThenC = o.find("b1b2") != std::string::npos && o.find("c1c2") != std::string::npos;
        EXPECT_TRUE(bThenC) << o;
    }
    // Both orders of the other albums occur.
    bool bFirst = false, cFirst = false;
    for (unsigned seed = 0; seed < 40; ++seed) {
        q.setShuffle(QueueShuffle::Albums, seeded(seed));
        const std::string o = order(q);
        (o.find("b1") < o.find("c1") ? bFirst : cFirst) = true;
    }
    EXPECT_TRUE(bFirst);
    EXPECT_TRUE(cFirst);
}

TEST(Queue, ShuffleAlbumsFromTheMiddleOfAnAlbumPlaysTheRestOfItFirst) {
    Queue q;
    q.replace({local("a1", "A"), local("a2", "A"), local("a3", "A"), local("b1", "B"), local("b2", "B"),
               local("c1", "C"), local("c2", "C")},
              1);
    q.setShuffle(QueueShuffle::Albums, kZero);
    // a1 a2 stay, a3 goes on; then the groups [b1 b2] and [c1 c2] shuffled: with 0 that is c, b.
    EXPECT_EQ(order(q), "a1a2a3c1c2b1b2");
    EXPECT_EQ(currentTitle(q), "a2");
}

TEST(Queue, ShuffleAlbumsTreatsEntriesWithoutAnAlbumAsSeparate) {
    Queue q;
    q.replace({local("x"), local("y"), local("z")});
    q.setShuffle(QueueShuffle::Albums, kZero);
    EXPECT_EQ(order(q), "xzy");
}

TEST(Queue, ShuffleOffGivesBackTheOriginalOrderWithTheSameCurrentEntry) {
    Queue q = abcde();
    q.setCurrent(1);
    q.setShuffle(QueueShuffle::Tracks, seeded(3));
    q.next();
    q.next();
    const std::string now = currentTitle(q);
    q.setShuffle(QueueShuffle::Off, seeded(3));
    EXPECT_EQ(order(q), "abcde");
    EXPECT_EQ(currentTitle(q), now);
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
}

TEST(Queue, ShufflingAgainKeepsWhatWasPlayedAndShufflesTheRest) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    q.next();
    q.next(); // d
    q.setShuffle(QueueShuffle::Tracks, kZero);
    EXPECT_EQ(order(q), "acdbe");
    EXPECT_EQ(currentTitle(q), "d");
}

TEST(Queue, ShuffleOnAnEmptyQueueDoesNothing) {
    Queue q;
    q.setShuffle(QueueShuffle::Tracks, seeded(1));
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off); // nothing to shuffle
    EXPECT_TRUE(q.empty());
}

TEST(Queue, PlayNextPutsEntriesRightAfterTheCurrentOne) {
    Queue q = abcde();
    q.setCurrent(1);
    q.playNext(tracks({"x", "y"}));
    EXPECT_EQ(order(q), "abxycde");
    EXPECT_EQ(currentTitle(q), "b");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, PlayNextWhenShuffledPlaysNextAndTurnsUpAfterTheCurrentEntryInTheOriginalOrder) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    q.next();                                   // c
    q.playNext(tracks({"x"}));
    EXPECT_EQ(order(q), "acxdeb");
    EXPECT_EQ(currentTitle(q), "c");
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abcxde"); // x follows c in the original order
    EXPECT_EQ(currentTitle(q), "c");
}

TEST(Queue, PlayNextOnAnEmptyQueueMakesTheFirstCurrent) {
    Queue q;
    q.playNext(tracks({"x", "y"}));
    EXPECT_EQ(order(q), "xy");
    EXPECT_EQ(currentTitle(q), "x");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, AppendPutsEntriesAtTheEndAndKeepsTheCurrentOne) {
    Queue q = abcde();
    q.setCurrent(3);
    q.append(tracks({"x", "y"}), kZero);
    EXPECT_EQ(order(q), "abcdexy");
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, AppendWithShuffleOffAsksForNoRandomNumber) {
    Queue q = abcde();
    int calls = 0;
    q.append(tracks({"x", "y"}), [&](std::size_t) { return ++calls, std::size_t{0}; });
    EXPECT_EQ(order(q), "abcdexy");
    EXPECT_EQ(calls, 0);
}

// The numbers it is given, in turn; the bounds it was asked for are kept.
struct Script {
    std::vector<std::size_t> values;
    std::vector<std::size_t> bounds{};
    QueueRandom random() {
        return [this](std::size_t bound) {
            bounds.push_back(bound);
            return values[(bounds.size() - 1) % values.size()];
        };
    }
};

TEST(Queue, AppendWithShuffleTracksMixesEachNewEntryIntoWhatHasNotPlayed) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero);
    ASSERT_EQ(order(q), "acdeb");
    Script script{{2, 3}};
    q.append(tracks({"x", "y"}), script.random());
    EXPECT_EQ(order(q), "acdxyeb") << "not both at the end";
    EXPECT_EQ(script.bounds, (std::vector<std::size_t>{5, 6})) << "a place before, between or after the 4, then 5 unplayed";
    EXPECT_EQ(currentTitle(q), "a");
    EXPECT_TRUE(q.edited());
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abcdexy") << "the original order has them at the end, in the order they were added";
}

TEST(Queue, AppendWithShuffleTracksCanGoRightAfterTheCurrentEntryOrToTheEndButNeverBeforeIt) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero);
    q.setCurrent(2); // a c [d] e b
    ASSERT_EQ(currentTitle(q), "d");
    Script first{{0}};
    q.append(tracks({"x"}), first.random());
    EXPECT_EQ(order(q), "acdxeb");
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_EQ(q.position(), 2u);
    EXPECT_EQ(first.bounds, (std::vector<std::size_t>{3})) << "2 unplayed entries, 3 places";
    Script last{{3}};
    q.append(tracks({"y"}), last.random());
    EXPECT_EQ(order(q), "acdxeby") << "the last place is the end";
    EXPECT_EQ(last.bounds, (std::vector<std::size_t>{4}));
    EXPECT_EQ(currentTitle(q), "d");
}

TEST(Queue, AppendWithShuffleTracksToTheLastEntryPutsThemAfterIt) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero);
    q.setCurrent(4);
    Script script{{0}};
    q.append(tracks({"x"}), script.random());
    EXPECT_EQ(order(q), "acdebx");
    EXPECT_EQ(script.bounds, (std::vector<std::size_t>{1}));
}

namespace {
// A1 A2 B1 B2 C1, current A1, shuffled by albums with kZero: A1 A2 C1 B1 B2.
Queue albumsShuffled() {
    Queue q;
    q.replace({local("A1", "A"), local("A2", "A"), local("B1", "B"), local("B2", "B"), local("C1", "C")});
    q.setShuffle(QueueShuffle::Albums, kZero);
    return q;
}
} // namespace

TEST(Queue, AppendWithShuffleAlbumsKeepsEachAddedAlbumWholeAndBetweenTwoRuns) {
    Queue q = albumsShuffled();
    ASSERT_EQ(order(q), "A1A2C1B1B2");
    Script script{{1, 3}};
    q.append({local("D1", "D"), local("D2", "D"), local("E1", "E")}, script.random());
    EXPECT_EQ(order(q), "A1A2C1D1D2B1B2E1");
    EXPECT_EQ(script.bounds, (std::vector<std::size_t>{3, 4})) << "2 runs, then 3, to place the runs between";
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "A1A2B1B2C1D1D2E1");
}

TEST(Queue, AppendWithShuffleAlbumsNeverSplitsTheRestOfTheCurrentAlbumOrAnotherRun) {
    Queue q = albumsShuffled();
    Script script{{0}};
    q.append({local("D1", "D"), local("D2", "D")}, script.random());
    EXPECT_EQ(order(q), "A1A2D1D2C1B1B2") << "the first place is after the rest of the current album";
    EXPECT_EQ(currentTitle(q), "A1");
    // Every place for a run of one: none is inside B1 B2.
    for (std::size_t slot = 0; slot < 3; ++slot) {
        Queue other = albumsShuffled();
        Script one{{slot}};
        other.append({local("D1", "D")}, one.random());
        const std::string o = order(other);
        EXPECT_NE(o.find("B1B2"), std::string::npos) << o;
        EXPECT_NE(o.find("A1A2"), std::string::npos) << o;
    }
}

TEST(Queue, PlayNextWithShuffleOnPutsTheEntriesRightAfterTheCurrentOneInOrder) {
    for (QueueShuffle mode : {QueueShuffle::Tracks, QueueShuffle::Albums}) {
        Queue q = abcde();
        q.setShuffle(mode, kZero);
        q.playNext(tracks({"x", "y"}));
        EXPECT_EQ(order(q).substr(0, 3), "axy") << static_cast<int>(mode);
        EXPECT_EQ(currentTitle(q), "a");
        q.setShuffle(QueueShuffle::Off, kZero);
        EXPECT_EQ(order(q), "axybcde") << "the original order has them after the current entry";
    }
}

TEST(Queue, AddingNothingIsNotAnEdit) {
    Queue q = abcde();
    q.append({}, kZero);
    q.playNext({});
    EXPECT_FALSE(q.edited());
}

TEST(Queue, RemoveBeforeTheCurrentEntryKeepsItCurrent) {
    Queue q = abcde();
    q.setCurrent(3);
    bool wasCurrent = true;
    EXPECT_TRUE(q.remove(1, &wasCurrent));
    EXPECT_FALSE(wasCurrent);
    EXPECT_EQ(order(q), "acde");
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, RemoveAfterTheCurrentEntryKeepsItCurrent) {
    Queue q = abcde();
    q.setCurrent(1);
    EXPECT_TRUE(q.remove(3));
    EXPECT_EQ(order(q), "abce");
    EXPECT_EQ(currentTitle(q), "b");
}

TEST(Queue, RemoveOfTheCurrentEntryMakesTheNextOneCurrent) {
    Queue q = abcde();
    q.setCurrent(2);
    bool wasCurrent = false;
    EXPECT_TRUE(q.remove(2, &wasCurrent));
    EXPECT_TRUE(wasCurrent);
    EXPECT_EQ(order(q), "abde");
    EXPECT_EQ(currentTitle(q), "d");
}

TEST(Queue, RemoveOfTheLastCurrentEntryWrapsToTheFirst) {
    Queue q = abcde();
    q.setCurrent(4);
    EXPECT_TRUE(q.remove(4));
    EXPECT_EQ(order(q), "abcd");
    EXPECT_EQ(currentTitle(q), "a");
}

TEST(Queue, RemoveOfTheOnlyEntryEmptiesTheQueue) {
    Queue q;
    q.replace(tracks({"a"}));
    EXPECT_TRUE(q.remove(0));
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.current(), nullptr);
}

TEST(Queue, RemoveOutOfRangeDoesNothing) {
    Queue q = abcde();
    EXPECT_FALSE(q.remove(5));
    EXPECT_EQ(q.size(), 5u);
    EXPECT_FALSE(q.edited());
}

TEST(Queue, RemoveWhenShuffledRemovesFromBothOrders) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    EXPECT_TRUE(q.remove(1));                   // c
    EXPECT_EQ(order(q), "adeb");
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abde");
}

TEST(Queue, RemovingTheLastEntryOfAShuffledQueueTurnsShuffleOff) {
    Queue q;
    q.replace(tracks({"a"}));
    q.setShuffle(QueueShuffle::Tracks, kZero);
    ASSERT_EQ(q.shuffle(), QueueShuffle::Tracks);
    q.remove(0);
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
}

TEST(Queue, RemoveOfTheCurrentEntryWhenShuffledMakesTheNextInPlayOrderCurrent) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    q.next();                                   // c
    bool wasCurrent = false;
    EXPECT_TRUE(q.remove(1, &wasCurrent));
    EXPECT_TRUE(wasCurrent);
    EXPECT_EQ(order(q), "adeb");
    EXPECT_EQ(currentTitle(q), "d");
}

TEST(Queue, MoveOfTheCurrentEntryWhenShuffledKeepsItCurrent) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    q.setCurrent(1);                            // c
    EXPECT_TRUE(q.move(1, 4));
    EXPECT_EQ(order(q), "adebc");
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.position(), 4u);
}

TEST(Queue, MoveOfAnotherEntryKeepsTheCurrentOneCurrent) {
    Queue q = abcde();
    q.setCurrent(2);
    EXPECT_TRUE(q.move(0, 4)); // a to the end
    EXPECT_EQ(order(q), "bcdea");
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.position(), 1u);
    EXPECT_TRUE(q.move(4, 0)); // and back
    EXPECT_EQ(order(q), "abcde");
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, MoveOfTheCurrentEntryKeepsItCurrent) {
    Queue q = abcde();
    q.setCurrent(1);
    EXPECT_TRUE(q.move(1, 3));
    EXPECT_EQ(order(q), "acdbe");
    EXPECT_EQ(currentTitle(q), "b");
    EXPECT_EQ(q.position(), 3u);
}

TEST(Queue, MoveOverTheCurrentEntryShiftsItsPosition) {
    Queue q = abcde();
    q.setCurrent(2);
    EXPECT_TRUE(q.move(4, 1)); // e jumps in front of c
    EXPECT_EQ(order(q), "aebcd");
    EXPECT_EQ(currentTitle(q), "c");
    EXPECT_EQ(q.position(), 3u);
}

TEST(Queue, MoveOutOfRangeDoesNothing) {
    Queue q = abcde();
    EXPECT_FALSE(q.move(0, 5));
    EXPECT_FALSE(q.move(5, 0));
    EXPECT_EQ(order(q), "abcde");
    EXPECT_FALSE(q.edited());
}

TEST(Queue, MoveWhenShuffledChangesOnlyThePlayOrder) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero); // a c d e b
    EXPECT_TRUE(q.move(3, 1));
    EXPECT_EQ(order(q), "aecdb");
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abcde");
}

TEST(Queue, MoveWithShuffleOffChangesTheOriginalOrderToo) {
    Queue q = abcde();
    q.move(4, 0);
    q.setShuffle(QueueShuffle::Tracks, kZero);
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "eabcd");
}

TEST(Queue, SettingRepeatIsNotAnEdit) {
    Queue q = abcde();
    q.setRepeat(QueueRepeat::All);
    q.setShuffle(QueueShuffle::Tracks, seeded(1));
    q.setCurrent(2);
    q.next();
    EXPECT_FALSE(q.edited());
}

TEST(Queue, RepeatTrackOnAnEmptyQueueRepeatsNothing) {
    Queue q;
    q.setRepeat(QueueRepeat::Track);
    EXPECT_EQ(q.advance(), Queue::Step::End);
}

TEST(Queue, RepeatTrackDoesNotWrapWhenTheUserMovesPastTheLastEntry) {
    Queue q = abcde();
    q.setRepeat(QueueRepeat::Track);
    q.setCurrent(4);
    EXPECT_EQ(q.next(), Queue::Step::End);
    EXPECT_EQ(currentTitle(q), "a");
    EXPECT_EQ(q.previous(), Queue::Step::End);
}

TEST(Queue, ShuffleAlbumsDoesNotJoinTheNextAlbumToTheCurrentOne) {
    Queue q;
    q.replace({local("a1", "A"), local("b1", "B"), local("b2", "B"), local("c1", "C")});
    q.setShuffle(QueueShuffle::Albums, kZero);
    EXPECT_EQ(order(q), "a1c1b1b2"); // [b1 b2] and [c1] shuffled, not b1 b2 kept in front
}

TEST(Queue, ShuffleSurvivesAGeneratorThatReturnsTooMuch) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, [](std::size_t) { return ~std::size_t{0}; });
    std::string sorted = order(q);
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, "abcde");
    EXPECT_EQ(currentTitle(q), "a");
}

// ---------------------------------------------------------------- restore and around (queue.tsv)

TEST(QueueRestore, RebuildsAShuffledQueue) {
    Queue q;
    q.restore(tracks({"a", "b", "c", "d"}), {2, 0, 3, 1}, 2, QueueShuffle::Tracks, QueueRepeat::All, true);
    EXPECT_EQ(order(q), "cadb");
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_EQ(q.position(), 2u);
    EXPECT_EQ(q.shuffle(), QueueShuffle::Tracks);
    EXPECT_EQ(q.repeat(), QueueRepeat::All);
    EXPECT_TRUE(q.edited());
    // It behaves like any queue: turning shuffle off gives back the original order, same current entry.
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abcd");
    EXPECT_EQ(currentTitle(q), "d");
}

TEST(QueueRestore, ADuplicateMissingOrOutOfRangeOrderFallsBackToTheOriginalOrder) {
    const std::vector<std::vector<std::size_t>> bad = {{0, 0, 1}, {0, 1}, {0, 1, 2, 3}, {0, 1, 7}, {2, 1, static_cast<std::size_t>(-1)}, {}};
    for (const auto& o : bad) {
        Queue q;
        q.restore(tracks({"a", "b", "c"}), o, 1, QueueShuffle::Albums, QueueRepeat::Off, false);
        EXPECT_EQ(order(q), "abc");
        EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
        EXPECT_EQ(currentTitle(q), "a") << "the position was in an order that is lost";
    }
}

TEST(QueueRestore, AnotherOrderWithShuffleOffFallsBackToTheOriginalOrderAndMoveMovesTheRightEntry) {
    Queue q;
    EXPECT_FALSE(q.restore(tracks({"a", "b", "c"}), {2, 0, 1}, 1, QueueShuffle::Off, QueueRepeat::Off, false));
    EXPECT_EQ(order(q), "abc");
    EXPECT_EQ(currentTitle(q), "a");
    ASSERT_TRUE(q.move(0, 2));
    EXPECT_EQ(order(q), "bca");
    Queue ok;
    EXPECT_TRUE(ok.restore(tracks({"a", "b", "c"}), {0, 1, 2}, 1, QueueShuffle::Off, QueueRepeat::Off, false));
    EXPECT_EQ(currentTitle(ok), "b");
}

TEST(QueueRevision, EveryChangeGetsANewStampAndCopiesShareIt) {
    Queue q;
    std::uint64_t seen = q.revision();
    auto changed = [&] {
        const bool is = q.revision() != seen;
        seen = q.revision();
        return is;
    };
    q.replace(tracks({"a", "b", "c", "d"}));
    EXPECT_TRUE(changed()) << "replace";
    q.append(tracks({"e"}), kZero);
    EXPECT_TRUE(changed()) << "append";
    q.playNext(tracks({"f"}));
    EXPECT_TRUE(changed()) << "playNext";
    ASSERT_TRUE(q.move(0, 2));
    EXPECT_TRUE(changed()) << "move";
    ASSERT_TRUE(q.remove(1));
    EXPECT_TRUE(changed()) << "remove";
    ASSERT_TRUE(q.setCurrent(2));
    EXPECT_TRUE(changed()) << "setCurrent";
    q.next();
    EXPECT_TRUE(changed()) << "next";
    q.previous();
    EXPECT_TRUE(changed()) << "previous";
    q.setShuffle(QueueShuffle::Tracks, kZero);
    EXPECT_TRUE(changed()) << "setShuffle";
    q.setRepeat(QueueRepeat::All);
    EXPECT_TRUE(changed()) << "setRepeat";
    EXPECT_TRUE(q.restore(tracks({"a", "b"}), {1, 0}, 0, QueueShuffle::Tracks, QueueRepeat::Off, false));
    EXPECT_TRUE(changed()) << "restore";
    Queue copy = q;
    EXPECT_EQ(copy.revision(), q.revision());
    q.append(tracks({"g"}), kZero);
    EXPECT_NE(copy.revision(), q.revision());
    EXPECT_FALSE(q.setCurrent(99));
    EXPECT_TRUE(changed()) << "append";
    EXPECT_FALSE(changed()) << "a refused change leaves the stamp";
}

TEST(QueueRestore, APositionPastTheEndIsTheFirstEntryAndNothingIsNothing) {
    Queue q;
    q.restore(tracks({"a", "b"}), {1, 0}, 5, QueueShuffle::Tracks, QueueRepeat::Off, false);
    EXPECT_EQ(q.position(), 0u);
    EXPECT_EQ(currentTitle(q), "b");
    q.restore({}, {}, 3, QueueShuffle::Tracks, QueueRepeat::All, true);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.shuffle(), QueueShuffle::Off);
    EXPECT_EQ(q.current(), nullptr);
}

namespace {
Queue numbered(std::size_t n, std::size_t start) {
    std::vector<QueueEntry> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(local(std::to_string(i)));
    Queue q;
    q.replace(std::move(v), start);
    return q;
}
}

TEST(QueueAround, ASmallQueueIsUnchanged) {
    Queue q = numbered(10, 4);
    Queue cut = q.around(20);
    EXPECT_EQ(cut.originalEntries(), q.originalEntries());
    EXPECT_EQ(cut.position(), 4u);
}

TEST(QueueAround, TheWindowIsCentredOnTheCurrentEntry) {
    Queue cut = numbered(100, 50).around(20);
    ASSERT_EQ(cut.size(), 20u);
    EXPECT_EQ(cut.at(0).title, "40");
    EXPECT_EQ(cut.at(19).title, "59");
    EXPECT_EQ(currentTitle(cut), "50");
}

TEST(QueueAround, TheWindowStopsAtTheEnds) {
    Queue start = numbered(100, 3).around(20);
    EXPECT_EQ(start.at(0).title, "0");
    EXPECT_EQ(currentTitle(start), "3");
    Queue end = numbered(100, 98).around(20);
    EXPECT_EQ(end.at(0).title, "80");
    EXPECT_EQ(end.at(19).title, "99");
    EXPECT_EQ(currentTitle(end), "98");
}

TEST(QueueAround, AShuffledQueueKeepsItsPlayOrderAndOriginalOrderInTheWindow) {
    Queue q = numbered(10, 5);
    q.setShuffle(QueueShuffle::Tracks, seeded(3));
    const std::string playOrder = order(q);
    Queue cut = q.around(4);
    ASSERT_EQ(cut.size(), 4u);
    // The window is four consecutive entries of the play order, the current one among them.
    EXPECT_NE(playOrder.find(order(cut)), std::string::npos);
    EXPECT_EQ(currentTitle(cut), "5");
    EXPECT_EQ(cut.shuffle(), QueueShuffle::Tracks);
    // The original order is the kept entries in their old relative order.
    std::string previous;
    for (const QueueEntry& e : cut.originalEntries()) {
        EXPECT_LT(std::stoi(previous.empty() ? "-1" : previous), std::stoi(e.title));
        previous = e.title;
    }
}

// ---------------------------------------------------------------- Repeat album (fixes.md 2.2)

TEST(Queue, RepeatAlbumWithShuffleOffGoesFromTheAlbumsLastTrackToItsFirst) {
    Queue q;
    q.replace({inAlbum("x", "X"), inAlbum("a1", "A"), inAlbum("a2", "A"), inAlbum("a3", "A"), inAlbum("y", "Y")}, 1);
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a2");
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a3");
    EXPECT_EQ(q.upcoming(), std::optional<std::size_t>(1)) << "the first track of the album, not y";
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a1");
    EXPECT_EQ(q.position(), 1u);
}

TEST(Queue, RepeatAlbumWithShuffleTracksPlaysTheAlbumInPlayOrderAndSkipsTheOthers) {
    Queue q;
    // Original a1 a2 a3 x y; play order a1 x a2 y a3.
    q.restore({inAlbum("a1", "A"), inAlbum("a2", "A"), inAlbum("a3", "A"), inAlbum("x", "X"), inAlbum("y", "Y")},
              {0, 3, 1, 4, 2}, 0, QueueShuffle::Tracks, QueueRepeat::Album, false);
    EXPECT_EQ(advanced(q, 4), "a2a3a1a2");
}

TEST(Queue, RepeatAlbumTellsTwoAlbumsWithTheSameNameApartByAlbumId) {
    Queue q;
    q.replace({inAlbum("p1", "Hits", "id1"), inAlbum("q1", "Hits", "id2"), inAlbum("p2", "Hits", "id1"),
               inAlbum("q2", "Hits", "id2")});
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(advanced(q, 4), "p2p1p2p1");
}

TEST(Queue, RepeatAlbumUsesTheNameWhenAnEntryHasNoAlbumId) {
    Queue q;
    // One side has an id, the other has none: the name decides. Different names are different albums.
    q.replace({inAlbum("p1", "Hits", "id1"), inAlbum("z", "Other"), inAlbum("p2", "Hits"), inAlbum("w", "Other", "id9")});
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(advanced(q, 3), "p2p1p2");
    q.setCurrent(1);
    EXPECT_EQ(advanced(q, 2), "wz") << "the two Other entries, one with an id, one without";
}

TEST(Queue, RepeatAlbumOfAnEntryWithNoAlbumRepeatsItAlone) {
    Queue q;
    q.replace({inAlbum("a", ""), inAlbum("b", ""), inAlbum("c", "C")});
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.advance(), Queue::Step::Repeated);
    EXPECT_EQ(currentTitle(q), "a") << "b has no album either, but it is not a's album";
    EXPECT_EQ(q.upcoming(), std::optional<std::size_t>(0));
    q.setCurrent(2);
    EXPECT_EQ(q.advance(), Queue::Step::Repeated) << "the only track of its album";
    EXPECT_EQ(currentTitle(q), "c");
}

TEST(Queue, RepeatAlbumNextOnTheAlbumsLastTrackGoesOnInPlayOrderAndThatAlbumRepeats) {
    Queue q;
    q.replace({inAlbum("a1", "A"), inAlbum("a2", "A"), inAlbum("b1", "B"), inAlbum("b2", "B")}, 1);
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.next(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "b1");
    EXPECT_EQ(advanced(q, 3), "b2b1b2");
    EXPECT_EQ(q.previous(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "b1");
    EXPECT_EQ(q.previous(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a2") << "Previous leaves the album too";
    EXPECT_EQ(q.advance(), Queue::Step::Moved);
    EXPECT_EQ(currentTitle(q), "a1");
}

TEST(Queue, RepeatAlbumNextAndPreviousDoNotWrapLikeRepeatAll) {
    Queue q;
    q.replace({inAlbum("a", "A"), inAlbum("b", "B")}, 1);
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.next(), Queue::Step::End);
    EXPECT_EQ(q.previous(), Queue::Step::End);
}

TEST(Queue, RepeatAlbumOnAnEmptyQueueRepeatsNothing) {
    Queue q;
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.advance(), Queue::Step::End);
    EXPECT_EQ(q.upcoming(), std::nullopt);
}

TEST(Queue, RepeatAlbumIsKeptWhenTheQueueIsCut) {
    Queue q;
    q.replace({inAlbum("a1", "A"), inAlbum("a2", "A"), inAlbum("b", "B")});
    q.setRepeat(QueueRepeat::Album);
    EXPECT_EQ(q.around(2).repeat(), QueueRepeat::Album);
}
