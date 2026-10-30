#pragma once
// queue.tsv (docs/queue.md, "Keeping it between restarts"): the music queue as text, so a restart has it at once
// and needs no network to list it. A plain format with no I/O: App reads and writes the file.
// Never in it: a stream URL, a token or the password. A remote entry is the server key (url and user) and the
// track id; the stream is made again when it plays.
#include "core/queue.h"

#include <cstddef>
#include <optional>
#include <string>

namespace oap {

// The most entries a saved queue holds; a longer one is cut around the current entry when it is saved.
constexpr std::size_t kMaxSavedEntries = 5000;
// A queue.tsv longer than this is not read (a damaged or foreign file must not stall the start).
constexpr std::size_t kMaxQueueFileBytes = 16u * 1024 * 1024;

struct SavedQueue {
    std::string title; // what the queue is called (the album or playlist it came from)
    Queue queue;
    double position = 0; // seconds into the current entry
    std::int64_t changed = 0; // last local queue or place change, wall milliseconds
    std::vector<std::string> pendingServers;
    std::vector<std::pair<std::string, std::int64_t>> acknowledgedServers;
};

// "" for an empty queue. Cuts the queue to kMaxSavedEntries around the current entry.
std::string formatQueue(const SavedQueue& saved);
// None for text that is not a queue file (empty, another version, a damaged line). A play order or position
// that does not fit the entries is repaired (Queue::restore()), not an error: the first entry is current, at 0 seconds.
std::optional<SavedQueue> parseQueue(const std::string& text);

} // namespace oap
