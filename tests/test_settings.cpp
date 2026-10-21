#include "core/settings.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using oap::Settings;
namespace fs = std::filesystem;

TEST(Settings, FallbackWhenMissingOrNotANumber) {
    Settings s({});
    EXPECT_EQ(s.getInt("volume", 70), 70);
    s.setString("volume", "loud");
    EXPECT_EQ(s.getInt("volume", 70), 70);
    EXPECT_EQ(s.getString("nope", "x"), "x");
}

TEST(Settings, SaveAndLoadRoundTrip) {
    fs::path file = oap::test::testTempDir() / "settings.tsv";
    fs::remove_all(file.parent_path());
    {
        Settings s(file);
        s.setInt("volume", 35);
        s.setString("pos:/books/A Book", "2:41.5"); // spaces and colons survive
        s.save();
    }
    Settings loaded(file);
    loaded.load();
    EXPECT_EQ(loaded.getInt("volume", 0), 35);
    EXPECT_EQ(loaded.getString("pos:/books/A Book"), "2:41.5");
    fs::remove_all(file.parent_path());
}

TEST(Settings, LoadSkipsLinesWithoutTab) {
    fs::path file = oap::test::testTempDir() / "settings.tsv";
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "garbage\nvolume\t20\n";
    Settings s(file);
    s.load();
    EXPECT_EQ(s.getInt("volume", 0), 20);
    EXPECT_EQ(s.getString("garbage", "none"), "none");
    fs::remove_all(file.parent_path());
}

TEST(Settings, ClearForgetsEverything) {
    Settings s({});
    s.setInt("volume", 10);
    s.clear();
    EXPECT_EQ(s.getInt("volume", 70), 70);
}
