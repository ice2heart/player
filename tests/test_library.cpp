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

TEST_F(LibraryTest, CacheFolderInsideTheLibraryIsSkipped) {
    touch(root / "Book" / "01.mp3");
    touch(root / "cache" / "server-1234" / "al1" / "01.flac");
    touch(root / "Book" / "nested" / "cache" / "02.mp3"); // another folder called cache is a book's own
    auto books = scanLibrary(root, root / "cache");
    ASSERT_EQ(books.size(), 1u);
    EXPECT_EQ(books[0].title, "Book");
    EXPECT_EQ(books[0].files.size(), 2u);
    // The same path spelled another way is the same folder.
    books = scanLibrary(root, root / "Book" / ".." / "cache");
    ASSERT_EQ(books.size(), 1u);
    // A cache folder deeper in a book is left out of that book.
    touch(root / "Book" / "dl" / "03.mp3");
    books = scanLibrary(root, root / "Book" / "dl");
    ASSERT_EQ(books.size(), 2u);
    EXPECT_EQ(books[0].files.size(), 2u);
    // Without a cache folder everything is a book, as before.
    EXPECT_EQ(scanLibrary(root).size(), 2u);
}

TEST_F(LibraryTest, CacheFolderThatDoesNotExistSkipsNothing) {
    touch(root / "Book" / "01.mp3");
    EXPECT_EQ(scanLibrary(root, root / "cache").size(), 1u);
}

TEST(NameGroup, ALatinNameIsGroupedByItsFirstLetterInCapitals) {
    EXPECT_EQ(oap::nameGroup("Dune"), "D");
    EXPECT_EQ(oap::nameGroup("dune"), "D");
    EXPECT_EQ(oap::nameGroup("Zeta"), "Z");
}

TEST(NameGroup, LatinAccentsAreIgnored) {
    EXPECT_EQ(oap::nameGroup("\xC3\x89t\xC3\xA9"), "E") << "Été";
}

TEST(NameGroup, LeadingPunctuationAndSpacesAreInTheNumberGroupWhereTheySort) {
    EXPECT_EQ(oap::nameGroup("(What's the Story)"), "#");
    EXPECT_EQ(oap::nameGroup("...And Justice"), "#");
    EXPECT_EQ(oap::nameGroup(" Padded"), "#");
    EXPECT_TRUE(oap::naturalLess("...And Justice", "1984")) << "and they sort before the digits";
}

TEST(NameGroup, CaseOfOtherLatinAndGreekLettersIsIgnored) {
    EXPECT_EQ(oap::nameGroup("\xC5\x81\xC3\xB3" "d\xC5\xBA"), oap::nameGroup("\xC5\x82\xC3\xB3" "d\xC5\xBA")) << "Łódź, łódź";
    EXPECT_EQ(oap::nameGroup("\xC5\x9A"), oap::nameGroup("\xC5\x9B")) << "Ś, ś";
    EXPECT_EQ(oap::nameGroup("\xCE\x91"), oap::nameGroup("\xCE\xB1")) << "Α, α";
    EXPECT_NE(oap::nameGroup("\xCE\x91"), oap::nameGroup("\xCE\x92")) << "Α, Β";
}

TEST(NameGroup, DigitsAndNamesWithNoLetterShareAGroup) {
    EXPECT_EQ(oap::nameGroup("1984"), "#");
    EXPECT_EQ(oap::nameGroup("0 to 100"), "#");
    EXPECT_EQ(oap::nameGroup("9 Lives"), "#");
    EXPECT_EQ(oap::nameGroup("!!!"), "#");
    EXPECT_EQ(oap::nameGroup(""), "#");
}

TEST(NameGroup, AnotherScriptIsAGroupPerFirstCharacterAndNotALatinOne) {
    EXPECT_EQ(oap::nameGroup("\xD0\x90\xD0\xB3\xD0\xB0"), "\xD0\xB0") << "Cyrillic, folded to lower case";
    EXPECT_NE(oap::nameGroup("\xD0\x90\xD0\xB3\xD0\xB0"), oap::nameGroup("\xD0\x91\xD0\xB8"));
    EXPECT_EQ(oap::nameGroup("\xE6\x97\xA5\xE6\x9C\xAC"), "\xE6\x97\xA5") << "three-byte character";
}
