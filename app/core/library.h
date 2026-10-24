#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace oap {

// A folder of audio files (one file per chapter) or a single file (chapters inside, e.g. M4B).
// A remote album or playlist is a Book too: `files` holds stream URLs, which carry
// the server login, so its part titles come from `titles`, never from the URL.
struct Book {
    std::string title;
    std::filesystem::path path; // empty for a remote list
    std::vector<std::filesystem::path> files;
    std::vector<std::string> titles; // one per file from the server; empty: titles come from the file names
    bool remote = false;             // files are stream URLs
};

std::vector<Book> scanLibrary(const std::filesystem::path& root);

} // namespace oap
