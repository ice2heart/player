#include "core/library.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using oap::scanLibrary;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
} // namespace

class LibraryTest : public testing::Test {
protected:
    void SetUp() override {
        root = oap::test::testTempDir();
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }
    fs::path root;
};

TEST_F(LibraryTest, FolderIsOneBookWithSortedAudioFiles) {
    touch(root / "Book" / "02 - Two.mp3");
    touch(root / "Book" / "01 - One.MP3"); // extension case does not matter
    touch(root / "Book" / "cover.jpg");
    touch(root / "Book" / "cd2" / "03 - Three.ogg");
    auto books = scanLibrary(root);
    ASSERT_EQ(books.size(), 1u);
    EXPECT_EQ(books[0].title, "Book");
    ASSERT_EQ(books[0].files.size(), 3u);
    EXPECT_EQ(books[0].files[0].filename(), "01 - One.MP3");
    EXPECT_EQ(books[0].files[1].filename(), "02 - Two.mp3");
    EXPECT_EQ(books[0].files[2].filename(), "03 - Three.ogg");
}

TEST_F(LibraryTest, TopLevelFileIsABookNamedAfterItsStem) {
    touch(root / "Single.m4b");
    auto books = scanLibrary(root);
    ASSERT_EQ(books.size(), 1u);
    EXPECT_EQ(books[0].title, "Single");
    EXPECT_EQ(books[0].files.size(), 1u);
}

TEST_F(LibraryTest, SkipsHiddenNonAudioAndEmptyFolders) {
    touch(root / ".hidden" / "a.mp3");
    touch(root / "notes.txt");
    touch(root / "Pictures" / "a.png");
    EXPECT_TRUE(scanLibrary(root).empty());
}

TEST_F(LibraryTest, BooksSortedCaseInsensitively) {
    touch(root / "beta.mp3");
    touch(root / "Alpha.mp3");
    touch(root / "Gamma" / "1.mp3");
    auto books = scanLibrary(root);
    ASSERT_EQ(books.size(), 3u);
    EXPECT_EQ(books[0].title, "Alpha");
    EXPECT_EQ(books[1].title, "beta");
    EXPECT_EQ(books[2].title, "Gamma");
}

TEST_F(LibraryTest, MissingRootIsEmpty) {
    EXPECT_TRUE(scanLibrary(root / "does-not-exist").empty());
}

TEST_F(LibraryTest, HiddenFilesInsideFoldersAreSkipped) {
    touch(root / "Book" / "01 - Sunrise.mp3");
    touch(root / "Book" / "._01 - Sunrise.mp3"); // macOS resource fork next to the real file
    touch(root / "Book" / ".Trashes" / "02 - Old.mp3");
    auto books = scanLibrary(root);
    ASSERT_EQ(books.size(), 1u);
    ASSERT_EQ(books[0].files.size(), 1u);
    EXPECT_EQ(books[0].files[0].filename(), "01 - Sunrise.mp3");
}

TEST_F(LibraryTest, UnreadableFoldersAreSkipped) {
    if (geteuid() == 0) GTEST_SKIP() << "root can read any folder";
    touch(root / "Book" / "01 - One.mp3");
    touch(root / "Book" / "locked" / "02 - Two.mp3");
    touch(root / "Locked" / "01 - One.mp3");
    fs::permissions(root / "Book" / "locked", fs::perms::none);
    fs::permissions(root / "Locked", fs::perms::none);
    std::vector<oap::Book> books;
    EXPECT_NO_THROW(books = scanLibrary(root));
    fs::permissions(root / "Book" / "locked", fs::perms::owner_all); // so TearDown can delete it
    fs::permissions(root / "Locked", fs::perms::owner_all);
    ASSERT_EQ(books.size(), 1u);
    EXPECT_EQ(books[0].title, "Book");
    ASSERT_EQ(books[0].files.size(), 1u);
    EXPECT_EQ(books[0].files[0].filename(), "01 - One.mp3");
}

TEST_F(LibraryTest, NumbersSortByValue) {
    touch(root / "Book" / "10 - Ten.mp3");
    touch(root / "Book" / "3 - Three.mp3");
    touch(root / "Book" / "1 - One.mp3");
    touch(root / "Book" / "Disc 10" / "1.mp3");
    touch(root / "Book" / "Disc 2" / "1.mp3");
    auto books = scanLibrary(root);
    ASSERT_EQ(books.size(), 1u);
    std::vector<std::string> names;
    for (const auto& f : books[0].files) names.push_back(fs::relative(f, root / "Book").generic_string());
    EXPECT_EQ(names, (std::vector<std::string>{"1 - One.mp3", "3 - Three.mp3", "10 - Ten.mp3",
                                               "Disc 2/1.mp3", "Disc 10/1.mp3"}));
}

namespace {
std::vector<std::string> titles(const std::vector<oap::Book>& books) {
    std::vector<std::string> out;
    for (const auto& b : books) out.push_back(b.title);
    return out;
}
} // namespace

TEST_F(LibraryTest, CyrillicSortsInAlphabetOrderIgnoringCase) {
    for (const char* name : {"яблоко", "Жук", "Ёлка", "арбуз", "Дом", "Борис", "ежик"})
        touch(root / (std::string(name) + ".mp3"));
    EXPECT_EQ(titles(scanLibrary(root)),
              (std::vector<std::string>{"арбуз", "Борис", "Дом", "ежик", "Ёлка", "Жук", "яблоко"}));
}

TEST_F(LibraryTest, AccentedLatinSortsWithItsBaseLetter) {
    // "Ärmel" decomposed, as macOS may write names: A plus a combining diaeresis (U+0308).
    const std::string armel = "A\xCC\x88rmel";
    for (const std::string name : {"Zebra", "Ärger", "Banane", "Arzt", "apple", armel.c_str(), "école"})
        touch(root / (name + ".mp3"));
    EXPECT_EQ(titles(scanLibrary(root)),
              (std::vector<std::string>{"apple", "Ärger", armel, "Arzt", "Banane", "école", "Zebra"}));
}
