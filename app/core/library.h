#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace oap {

// A folder of audio files (one file per chapter) or a single file (chapters inside, e.g. M4B).
struct Book {
    std::string title;
    std::filesystem::path path;
    std::vector<std::filesystem::path> files;
};

std::vector<Book> scanLibrary(const std::filesystem::path& root);

} // namespace oap
