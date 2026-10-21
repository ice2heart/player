#include "library.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace fs = std::filesystem;

namespace oap {

namespace {
bool isAudio(const fs::path& p) {
    static const std::set<std::string> kExtensions = {".mp3", ".m4a", ".m4b", ".aac", ".ogg",
                                                      ".opus", ".flac", ".wav", ".aiff"};
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return kExtensions.count(ext) > 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

std::vector<Book> scanLibrary(const fs::path& root) {
    std::vector<Book> books;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.path().filename().string().starts_with(".")) continue;
        if (entry.is_directory()) {
            Book book{entry.path().filename().string(), entry.path(), {}};
            for (const auto& f : fs::recursive_directory_iterator(entry.path(), ec))
                if (f.is_regular_file() && isAudio(f.path())) book.files.push_back(f.path());
            std::sort(book.files.begin(), book.files.end());
            if (!book.files.empty()) books.push_back(std::move(book));
        } else if (entry.is_regular_file() && isAudio(entry.path())) {
            books.push_back({entry.path().stem().string(), entry.path(), {entry.path()}});
        }
    }
    std::sort(books.begin(), books.end(),
              [](const Book& a, const Book& b) { return lower(a.title) < lower(b.title); });
    return books;
}

} // namespace oap
