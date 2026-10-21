#include "core/library.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>

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
