#pragma once
// scrobbles.tsv (docs/fixes.md, "Scrobbles"): the scrobble outbox as text, next to state.tsv. A plain format
// with no I/O: App reads and writes the file. Never in it: a stream URL, a token or the password; a server is
// its key (TrackCache::serverKey(): host and a hash of url and user), a track its id.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oap {

// The most entries the outbox keeps; the oldest go first.
constexpr std::size_t kMaxScrobbles = 1000;
// A scrobbles.tsv longer than this is not read (a damaged or foreign file must not stall the start).
constexpr std::size_t kMaxScrobbleFileBytes = 1024 * 1024;

// A track heard and not yet taken by its server.
struct ScrobbleEntry {
    std::string server;       // catalogue server key
    std::string trackId;
    std::int64_t timeMs = 0;  // when it was heard, ms since 1970
    bool operator==(const ScrobbleEntry&) const = default;
};

// "" for no entries; else a header line and one line per entry (oldest first): server key, track id, time.
std::string formatScrobbles(const std::vector<ScrobbleEntry>& entries);
// No entries for text that is not a scrobbles file (another version, too long); a damaged line is skipped.
// At most kMaxScrobbles: the newest.
std::vector<ScrobbleEntry> parseScrobbles(const std::string& text);

} // namespace oap
