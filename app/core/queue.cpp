#include "core/queue.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <utility>

namespace oap {

namespace {

std::atomic<std::uint64_t> lastRevision{0};

template <class T>
void shuffleWith(std::vector<T>& v, const QueueRandom& random) {
    for (std::size_t i = v.size(); i > 1; --i)
        std::swap(v[i - 1], v[random(i) % i]); // Fisher-Yates
}

} // namespace

void Queue::touch() { revision_ = ++lastRevision; }

void Queue::replace(std::vector<QueueEntry> entries, std::size_t start) {
    entries_ = std::move(entries);
    order_.resize(entries_.size());
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    pos_ = entries_.empty() ? 0 : std::min(start, entries_.size() - 1);
    shuffle_ = QueueShuffle::Off;
    repeat_ = QueueRepeat::Off;
    edited_ = false;
    touch();
}

bool Queue::restore(std::vector<QueueEntry> entries, std::vector<std::size_t> order, std::size_t position,
                    QueueShuffle shuffle, QueueRepeat repeat, bool edited) {
    std::vector<bool> seen(entries.size(), false);
    bool valid = order.size() == entries.size();
    for (std::size_t i = 0; valid && i < order.size(); ++i) {
        valid = order[i] < entries.size() && !seen[order[i]];
        if (valid) seen[order[i]] = true;
    }
    // With shuffle off the play order is the original one (move() relies on it).
    if (valid && shuffle == QueueShuffle::Off && entries.size() > 0)
        for (std::size_t i = 0; valid && i < order.size(); ++i) valid = order[i] == i;
    entries_ = std::move(entries);
    if (valid) {
        order_ = std::move(order);
        shuffle_ = shuffle;
    } else {
        order_.resize(entries_.size());
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        shuffle_ = QueueShuffle::Off;
    }
    if (entries_.empty()) shuffle_ = QueueShuffle::Off;
    // `position` is in the play order: without that order it does not say which entry was current.
    pos_ = valid && position < entries_.size() ? position : 0;
    repeat_ = repeat;
    edited_ = edited;
    touch();
    return valid;
}

Queue Queue::around(std::size_t limit) const {
    if (entries_.size() <= limit) return *this;
    // The window [first, first + limit) of the play order: the current entry in the middle, unless an end is near.
    const std::size_t first = std::min(pos_ > limit / 2 ? pos_ - limit / 2 : 0, entries_.size() - limit);
    std::vector<bool> kept(entries_.size(), false);
    for (std::size_t p = first; p < first + limit; ++p) kept[order_[p]] = true;
    std::vector<std::size_t> newIndex(entries_.size(), 0); // original index -> index among the kept
    std::vector<QueueEntry> entries;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (kept[i]) {
            newIndex[i] = entries.size();
            entries.push_back(entries_[i]);
        }
    std::vector<std::size_t> order;
    for (std::size_t p = first; p < first + limit; ++p) order.push_back(newIndex[order_[p]]);
    Queue cut;
    cut.restore(std::move(entries), std::move(order), pos_ - first, shuffle_, repeat_, edited_);
    return cut;
}

void Queue::insertAt(std::size_t originalIndex, std::size_t orderPos, const std::vector<QueueEntry>& entries) {
    if (entries.empty()) return;
    const std::size_t n = entries.size();
    for (auto& i : order_)
        if (i >= originalIndex) i += n;
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(originalIndex), entries.begin(), entries.end());
    std::vector<std::size_t> added(n);
    std::iota(added.begin(), added.end(), originalIndex);
    order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(orderPos), added.begin(), added.end());
    edited_ = true;
    touch();
}

void Queue::playNext(const std::vector<QueueEntry>& entries) {
    if (empty()) return insertAt(0, 0, entries);
    insertAt(order_[pos_] + 1, pos_ + 1, entries);
}

void Queue::append(const std::vector<QueueEntry>& entries) { insertAt(entries_.size(), order_.size(), entries); }

bool Queue::remove(std::size_t position, bool* removedCurrent) {
    if (position >= order_.size()) return false;
    const std::size_t index = order_[position];
    const bool wasCurrent = position == pos_;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(position));
    for (auto& i : order_)
        if (i > index) --i;
    if (position < pos_) --pos_;
    if (pos_ >= order_.size()) pos_ = 0; // the last was current: the next is the first
    if (order_.empty()) shuffle_ = QueueShuffle::Off; // nothing to be shuffled
    edited_ = true;
    touch();
    if (removedCurrent) *removedCurrent = wasCurrent;
    return true;
}

bool Queue::move(std::size_t from, std::size_t to) {
    if (from >= order_.size() || to >= order_.size()) return false;
    const std::size_t currentIndex = order_[pos_];
    const std::size_t moved = order_[from];
    order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(from));
    order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(to), moved);
    pos_ = static_cast<std::size_t>(std::find(order_.begin(), order_.end(), currentIndex) - order_.begin());
    if (shuffle_ == QueueShuffle::Off) {
        // The original order follows the play order: move the one entry, then order_ is 0, 1, 2 ... again.
        QueueEntry entry = std::move(entries_[from]);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from));
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(to), std::move(entry));
        std::iota(order_.begin(), order_.end(), std::size_t{0});
    }
    edited_ = true;
    touch();
    return true;
}

Queue::Step Queue::advance() {
    if (repeat_ == QueueRepeat::Track && !empty()) return Step::Repeated;
    return next();
}

Queue::Step Queue::next() {
    touch();
    if (pos_ + 1 < order_.size()) {
        ++pos_;
        return Step::Moved;
    }
    if (repeat_ == QueueRepeat::All && !empty()) {
        pos_ = 0;
        return Step::Moved;
    }
    pos_ = 0;
    return Step::End;
}

Queue::Step Queue::previous() {
    touch();
    if (pos_ > 0) {
        --pos_;
        return Step::Moved;
    }
    if (repeat_ == QueueRepeat::All && !empty()) {
        pos_ = order_.size() - 1;
        return Step::Moved;
    }
    return Step::End;
}

std::optional<std::size_t> Queue::upcoming() const {
    if (empty()) return std::nullopt;
    if (repeat_ == QueueRepeat::Track) return pos_;
    if (pos_ + 1 < order_.size()) return pos_ + 1;
    if (repeat_ == QueueRepeat::All) return 0;
    return std::nullopt;
}

bool Queue::setCurrent(std::size_t position) {
    if (position >= order_.size()) return false;
    pos_ = position;
    touch();
    return true;
}

void Queue::setShuffle(QueueShuffle mode, const QueueRandom& random) {
    if (empty()) return; // nothing to shuffle; a new queue starts with shuffle off
    touch();
    shuffle_ = mode;
    if (mode == QueueShuffle::Off) {
        const std::size_t current = order_[pos_];
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        pos_ = current;
        return;
    }
    // What was played and the current entry stay; what follows is shuffled. The tail is cut into groups
    // that stay together: one per entry, or (albums) one per run of entries with the same album name.
    // The run that goes on with the current entry's album stays first.
    const std::vector<std::size_t> tail(order_.begin() + static_cast<std::ptrdiff_t>(pos_) + 1, order_.end());
    order_.resize(pos_ + 1);
    std::vector<std::vector<std::size_t>> groups;
    bool inFirst = mode == QueueShuffle::Albums;
    for (std::size_t k = 0; k < tail.size(); ++k) {
        const std::string& album = entries_[tail[k]].album;
        const std::string& before = entries_[k == 0 ? order_[pos_] : tail[k - 1]].album;
        const bool sameAlbum = mode == QueueShuffle::Albums && !album.empty() && album == before;
        if (inFirst && sameAlbum) {
            order_.push_back(tail[k]); // the rest of the current album plays on
        } else if (sameAlbum) {
            groups.back().push_back(tail[k]);
        } else {
            inFirst = false;
            groups.push_back({tail[k]});
        }
    }
    shuffleWith(groups, random);
    for (const auto& g : groups) order_.insert(order_.end(), g.begin(), g.end());
}

} // namespace oap
