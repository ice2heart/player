#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace oap {

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
};

std::vector<Book> scanLibrary(const std::filesystem::path& root);

} // namespace oap
