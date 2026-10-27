#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
    // remote: an audiobook of the server (docs/streaming.md 4.11), not an album. Its chapters are the
    // tracks, its place is kept under `pos:` + `path` (the library item id, never a URL) and it
    // is not a `last_stream`.
    bool serverBook = false;
    std::optional<RemoteSource> source; // remote: where the list came from; none: it is not resumed
    // Remote, for the automatic cache (one per file; may be shorter, then unknown): the original
    // file's type (e.g. "flac") and size in bytes, and the artist of the list (an album's, may be empty).
    std::vector<std::string> suffixes;
    std::vector<long long> sizes;
    std::string artist;
    // Music that spans albums (Play all): where each album starts in `files`, in order, with its
    // title. Empty: the whole list is one album. The Album step unit moves between these.
    struct Album {
        size_t first = 0; // index in `files` of its first track
        std::string title;
    };
    std::vector<Album> albums;
};

// Natural, case-insensitive order of names: runs of digits compare as numbers ("3" before
// "10"). A total order. The order of the Library and Music lists.
bool naturalLess(std::string_view a, std::string_view b);

// The group a name falls in for letter jumps in a list sorted by `naturalLess`: its first
// letter in capitals ("D"), "#" for a digit or ASCII punctuation (or an empty name), or its first
// character itself for another script (a group of its own, after the Latin ones). Case and
// Latin-1 accents are ignored. A name starting with ASCII punctuation or a space is "#" too,
// because that is where naturalLess sorts it: a group is always a run of the sorted list.
std::string nameGroup(std::string_view name);

// Natural order of paths: folder by folder, digits as numbers, case and accents ignored
// ("cd2/01" before "cd10/01"). The order of the files of a book.
bool pathLess(const std::filesystem::path& a, const std::filesystem::path& b);

// The books under `root`. A folder `skip` (the cache of downloaded tracks),
// wherever it lies below `root`, is left out.
std::vector<Book> scanLibrary(const std::filesystem::path& root, const std::filesystem::path& skip = {});

} // namespace oap
