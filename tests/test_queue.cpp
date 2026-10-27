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
    q.append(tracks({"f"}));
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
    q.append(tracks({"x", "y"}));
    EXPECT_EQ(order(q), "abcdexy");
    EXPECT_EQ(currentTitle(q), "d");
    EXPECT_TRUE(q.edited());
}

TEST(Queue, AppendWhenShuffledGoesToTheEndOfBothOrders) {
    Queue q = abcde();
    q.setShuffle(QueueShuffle::Tracks, kZero);
    q.append(tracks({"x"}));
    EXPECT_EQ(order(q), "acdebx");
    q.setShuffle(QueueShuffle::Off, kZero);
    EXPECT_EQ(order(q), "abcdex");
}

TEST(Queue, AddingNothingIsNotAnEdit) {
    Queue q = abcde();
    q.append({});
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
