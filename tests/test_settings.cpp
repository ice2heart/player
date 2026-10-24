#include "core/settings.h"
#include "fakes.h"

#include <gtest/gtest.h>

using oap::Settings;
using oap::test::FakeStore;

TEST(Settings, FallbackWhenMissingOrNotANumber) {
    Settings s;
    EXPECT_EQ(s.getInt("volume", 70), 70);
    s.setString("volume", "loud");
    EXPECT_EQ(s.getInt("volume", 70), 70);
    EXPECT_EQ(s.getString("nope", "x"), "x");
}

TEST(Settings, LoadsFromAndSavesToItsStore) {
    FakeStore store;
    store.file = {{"volume", "35"}};
    Settings s(&store);
    s.load();
    EXPECT_EQ(s.getInt("volume", 0), 35);
    s.setInt("volume", 40);
    EXPECT_EQ(store.saves, 0) << "only save() writes";
    s.save();
    EXPECT_EQ(store.saves, 1);
    EXPECT_EQ(store.onDisk("volume"), "40");
}

TEST(Settings, ClearForgetsEverything) {
    Settings s;
    s.setInt("volume", 10);
    s.clear();
    EXPECT_EQ(s.getInt("volume", 70), 70);
}

TEST(Settings, ClearExceptKeepsOnlyThatSection) {
    Settings s;
    s.setInt("volume", 10);
    s.setString("server.url", "https://x");
    s.setString("serverless", "y");      // not under "server."
    s.setString("backup.server.url", "z"); // nor this
    s.clearExcept(oap::kServerPrefix);
    EXPECT_EQ(s.getString("server.url"), "https://x");
    EXPECT_EQ(s.getString("serverless", "gone"), "gone");
    EXPECT_EQ(s.getString("backup.server.url", "gone"), "gone");
    EXPECT_EQ(s.getInt("volume", 70), 70);
}

TEST(Settings, AServerIsConfiguredOnlyWithAUrl) {
    Settings s;
    EXPECT_FALSE(oap::serverConfigured(s));
    s.setString("server.user", "ice");
    EXPECT_FALSE(oap::serverConfigured(s)) << "a server section without url";
    s.setString("server.url", "");
    EXPECT_FALSE(oap::serverConfigured(s));
    s.setString("server.url", "https://music.example");
    EXPECT_TRUE(oap::serverConfigured(s));
}

TEST(Settings, TsvRoundTrip) {
    oap::SettingsMap values{{"pos:/books/A Book", "2:41.5"}, {"last_book", "/books/A Book"}};
    std::string text = oap::formatTsv(values);
    EXPECT_EQ(text, "last_book\t/books/A Book\npos:/books/A Book\t2:41.5\n");
    EXPECT_EQ(oap::parseTsv(text), values); // spaces and colons survive
}

TEST(Settings, TsvSkipsLinesWithoutTab) {
    auto values = oap::parseTsv("garbage\nvolume\t20\n");
    EXPECT_EQ(values, (oap::SettingsMap{{"volume", "20"}}));
}
