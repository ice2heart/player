#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oap {

// The album or playlist on a server that a remote Book was made from, so that
// it can be loaded again after a restart (state.tsv `last_stream.*`).
struct RemoteSource {
    enum class Kind { Album, Playlist };
    Kind kind = Kind::Album;
    std::string id; // the server's album or playlist id
};

// A folder of audio files (one file per chapter) or a single file (chapters inside, e.g. M4B).
// A remote album or playlist is a Book too: `files` holds the server's track
// ids, and App asks the server for each stream URL (which carries the login)
// when it opens the track. Its part titles come from `titles`.
struct Book {
    std::string title;
    std::filesystem::path path; // empty for a remote list
    std::vector<std::filesystem::path> files;
    std::vector<std::string> titles; // one per file from the server; empty: titles come from the file names
    std::vector<double> durations;   // one per file from the server (seconds, 0 if unknown); empty for local books
    bool remote = false;             // files are track ids on the server
    std::optional<RemoteSource> source; // remote: where the list came from; none: it is not resumed
};

// The books under `root`. A folder `skip` (the cache of downloaded tracks),
// wherever it lies below `root`, is left out.
std::vector<Book> scanLibrary(const std::filesystem::path& root, const std::filesystem::path& skip = {});

} // namespace oap
