#include "library.h"
#include "text.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>

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

// Dot files and folders: macOS "._" resource forks, .Trashes, .Spotlight-V100 and the like.
bool isHidden(const fs::path& p) { return p.filename().string().starts_with("."); }

// Folds case (foldCase) and drops Latin-1 accents ("Ä" sorts as "a").
char32_t fold(char32_t c) {
    c = foldCase(c);
    if (c == 0xDF) return 's'; // ß
    if (c >= 0xE0 && c <= 0xFF && c != 0xF7) { // à..ÿ except ÷
        static constexpr char kBase[] = "aaaaaaaceeeeiiiidnooooo_ouuuuyty";
        return static_cast<unsigned char>(kBase[c - 0xE0]);
    }
    return c;
}

// Reads the folded character at s[i] and skips the combining accents (U+0300..U+036F) after it,
// so a name decomposed by macOS ("A" + U+0308) sorts like the composed one.
char32_t nextKey(std::string_view s, size_t& i) {
    char32_t c = fold(nextCodePoint(s, i));
    while (i < s.size()) {
        size_t k = i;
        char32_t mark = nextCodePoint(s, k);
        if (mark < 0x300 || mark > 0x36F) break;
        i = k;
    }
    return c;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Reads the run of digits at s[i] and returns it without leading zeros.
std::string_view digitRun(std::string_view s, size_t& i) {
    size_t start = i;
    while (i < s.size() && isDigit(s[i])) ++i;
    auto run = s.substr(start, i - start);
    auto nonZero = run.find_first_not_of('0');
    return nonZero == std::string_view::npos ? std::string_view{} : run.substr(nonZero);
}

} // namespace

std::string nameGroup(std::string_view name) {
    size_t i = 0;
    if (name.empty() || (static_cast<unsigned char>(name[0]) < 0x80 && !std::isalnum(static_cast<unsigned char>(name[0])))) return "#";
    char32_t c = nextKey(name, i);
    if (c >= '0' && c <= '9') return "#";
    if (c >= 'a' && c <= 'z') return std::string(1, char(c - 'a' + 'A'));
    std::string out;
    appendUtf8(out, c);
    return out;
}

// Natural, case-insensitive order: runs of digits compare as numbers ("3" before "10") and
// letters compare folded. Names that fold equal fall back to their bytes, so the order is total.
bool naturalLess(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            auto x = digitRun(a, i), y = digitRun(b, j);
            if (x.size() != y.size()) return x.size() < y.size();
            if (x != y) return x < y;
            continue;
        }
        char32_t x = nextKey(a, i), y = nextKey(b, j);
        if (x != y) return x < y;
    }
    if ((i < a.size()) != (j < b.size())) return j < b.size();
    return a < b;
}

namespace {
// The cache folder is not a book.
bool isSkipped(const fs::path& dir, const fs::path& skip) {
    std::error_code ec;
    return fs::equivalent(dir, skip, ec); // false (with `ec` set) when `skip` is empty or missing
}

// Adds the audio files under dir, skipping dot entries and anything that cannot be read.
// Never throws: a folder that fails to list keeps what was read before the error.
void collectAudio(const fs::path& dir, const fs::path& skip, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (isHidden(entry.path())) continue;
        std::error_code entryEc;
        if (entry.is_directory(entryEc) && !entry.is_symlink(entryEc)) {
            if (!isSkipped(entry.path(), skip)) collectAudio(entry.path(), skip, out);
        } else if (entry.is_regular_file(entryEc) && isAudio(entry.path()))
            out.push_back(entry.path());
    }
}
} // namespace

bool pathLess(const fs::path& a, const fs::path& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](const fs::path& x, const fs::path& y) {
                                            return naturalLess(x.string(), y.string());
                                        });
}

std::vector<Book> scanLibrary(const fs::path& root, const fs::path& skip) {
    std::vector<Book> books;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (isHidden(entry.path())) continue;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            if (isSkipped(entry.path(), skip)) continue;
            Book book;
            book.title = entry.path().filename().string();
            book.path = entry.path();
            collectAudio(entry.path(), skip, book.files);
            std::sort(book.files.begin(), book.files.end(), pathLess);
            if (!book.files.empty()) books.push_back(std::move(book));
        } else if (entry.is_regular_file(entryEc) && isAudio(entry.path())) {
            Book book;
            book.title = entry.path().stem().string();
            book.path = entry.path();
            book.files = {entry.path()};
            books.push_back(std::move(book));
        }
    }
    std::sort(books.begin(), books.end(),
              [](const Book& a, const Book& b) { return naturalLess(a.title, b.title); });
    return books;
}

} // namespace oap
