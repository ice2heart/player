#pragma once
// The music queue (docs/queue.md, "The queue"): entries, their original order and play order, the
// current entry, shuffle, repeat and whether the user edited it by hand. A plain class: no App, no I/O.
// The random generator is passed in, so tests choose the shuffle.
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oap {

// One track in the queue: a file on this device or a track of a music server.
struct QueueEntry {
    enum class Kind { Local, Remote };
    Kind kind = Kind::Local;
    std::string path{};      // Local: the file
    std::string serverUrl{}; // Remote: the server key is serverUrl + user. Never a password or a stream URL.
    std::string user{};
    std::string trackId{};   // Remote
    std::string title{}, album{}, artist{};
    double duration = 0;     // seconds, Remote (0 if unknown)
    bool operator==(const QueueEntry&) const = default;
};

enum class QueueShuffle { Off, Tracks, Albums };
enum class QueueRepeat { Off, All, Track };

// Returns a number in [0, bound), bound > 0. Tests pass one that makes the shuffle known.
using QueueRandom = std::function<std::size_t(std::size_t bound)>;

// current(), at() and originalEntries() point into the queue: any change to it invalidates them, so copy
// an entry you keep.
class Queue {
public:
    // What a move to another entry did.
    enum class Step {
        Moved,    // another entry (or, with one entry and repeat all, the same one) is current
        Repeated, // advance() with repeat track: the current entry plays again
        End,      // next(): past the last entry with repeat off; the first entry is current again.
                  // previous(): nothing before the first entry; the current entry did not change.
    };

    // Replaces everything with `entries`, current at `start` (clamped), shuffle and repeat off, not edited.
    void replace(std::vector<QueueEntry> entries, std::size_t start = 0);

    // Puts `entries` after the current entry (play order and original order) / at the end. Both count as
    // edits. On an empty queue the first one becomes current.
    void playNext(const std::vector<QueueEntry>& entries);
    void append(const std::vector<QueueEntry>& entries);

    // Position is in the play order. Out of range: nothing happens and false is returned. Both are edits.
    // remove() of the current entry makes the next one current (the first, if it was the last);
    // `removedCurrent` tells it, so the caller can stop playing it.
    bool remove(std::size_t position, bool* removedCurrent = nullptr);
    // Moves the entry at `from` to `to`; the current entry stays current wherever it ends up.
    bool move(std::size_t from, std::size_t to);

    // The track ended: repeat track plays it again, else as next().
    Step advance();
    // The user moves on / back. Repeat track does not matter; repeat all wraps around.
    Step next();
    Step previous();
    // The position advance() would move to, without moving: the same one with repeat track, the first with
    // repeat all past the last entry, none at the end of the queue (and when empty).
    std::optional<std::size_t> upcoming() const;
    // Makes the entry at `position` current (not an edit). False if out of range.
    bool setCurrent(std::size_t position);

    // Shuffle keeps the current entry current, keeps what came before it in the play order and shuffles
    // what follows; albums keep each album's order (an album is a run of entries with the same album
    // name, and the rest of the current album plays first). Off gives back the original order with the
    // same current entry. On an empty queue it does nothing.
    void setShuffle(QueueShuffle mode, const QueueRandom& random);
    void setRepeat(QueueRepeat mode) { repeat_ = mode; }
    QueueShuffle shuffle() const { return shuffle_; }
    QueueRepeat repeat() const { return repeat_; }

    // True once the user added, moved or removed something since replace().
    bool edited() const { return edited_; }

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    // Position of the current entry in the play order; 0 when empty.
    std::size_t position() const { return pos_; }
    // The current entry, null when empty.
    const QueueEntry* current() const { return empty() ? nullptr : &entries_[order_[pos_]]; }
    // The entry at a position of the play order; position < size().
    const QueueEntry& at(std::size_t position) const { return entries_[order_[position]]; }
    // The entries in the original order, and for each position of the play order the index into them.
    const std::vector<QueueEntry>& originalEntries() const { return entries_; }
    const std::vector<std::size_t>& playOrder() const { return order_; }

private:
    void insertAt(std::size_t originalIndex, std::size_t orderPos, const std::vector<QueueEntry>& entries);

    std::vector<QueueEntry> entries_; // original order
    std::vector<std::size_t> order_;  // play order: indexes into entries_
    std::size_t pos_ = 0;             // position in order_
    QueueShuffle shuffle_ = QueueShuffle::Off;
    QueueRepeat repeat_ = QueueRepeat::Off;
    bool edited_ = false;
};

} // namespace oap
