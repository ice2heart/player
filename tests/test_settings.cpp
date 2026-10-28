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

TEST(Settings, ClearPrefixRemovesOnlyThoseKeys) {
    Settings s;
    s.setString("last_stream.id", "al1");
    s.setString("last_stream.pos", "4.0");
    s.setString("last_book", "/books/Alpha");
    s.clearPrefix("last_stream.");
    EXPECT_EQ(s.values(), (oap::SettingsMap{{"last_book", "/books/Alpha"}}));
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
    s.clearExcept({oap::kServerPrefix});
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

namespace {

Settings withServers(oap::SettingsMap values) {
    Settings s;
    for (const auto& [key, value] : values) s.setString(key, value);
    return s;
}

} // namespace

TEST(Settings, ClearExceptKeepsSeveralSections) {
    Settings s = withServers({{"server.url", "https://a"}, {"servers.0.url", "https://b"}, {"volume", "10"}, {"serversX", "y"}});
    s.clearExcept({oap::kServerPrefix, oap::kServersPrefix});
    EXPECT_EQ(s.getString("server.url"), "https://a");
    EXPECT_EQ(s.getString("servers.0.url"), "https://b");
    EXPECT_EQ(s.getInt("volume", 70), 70);
    EXPECT_EQ(s.getString("serversX", "gone"), "gone");
}

TEST(ServerConfigs, AListOfTwoGivesTwoConfigsInOrder) {
    Settings s = withServers({{"servers.0.type", "subsonic"}, {"servers.0.name", "Music"},
                              {"servers.0.url", "https://music.example/"}, {"servers.0.user", "ice"},
                              {"servers.0.password", "pw"}, {"servers.0.audiobook_folder", "Books"},
                              {"servers.1.type", "audiobookshelf"}, {"servers.1.url", "https://books.example"},
                              {"servers.1.api_key", "key"}, {"servers.1.library", "Novels"}});
    auto c = oap::serverConfigs(s);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0].type, "subsonic");
    EXPECT_EQ(c[0].name, "Music");
    EXPECT_EQ(c[0].url, "https://music.example") << "no trailing slash";
    EXPECT_EQ(c[0].user, "ice");
    EXPECT_EQ(c[0].password, "pw");
    EXPECT_EQ(c[0].audiobookFolder, "Books");
    EXPECT_EQ(c[1].type, "audiobookshelf");
    EXPECT_EQ(c[1].url, "https://books.example");
    EXPECT_EQ(c[1].apiKey, "key");
    EXPECT_EQ(c[1].library, "Novels");
    EXPECT_EQ(c[1].name, "");
}

TEST(ServerConfigs, TenOrMoreEntriesStayInNumericOrder) {
    Settings s;
    for (int i = 0; i < 12; ++i) s.setString("servers." + std::to_string(i) + ".url", "https://s" + std::to_string(i));
    auto c = oap::serverConfigs(s);
    ASSERT_EQ(c.size(), 12u);
    EXPECT_EQ(c[2].url, "https://s2");
    EXPECT_EQ(c[10].url, "https://s10");
}

TEST(ServerConfigs, AMissingTypeIsSubsonicAndTheTypeIsLowerCased) {
    Settings s = withServers({{"servers.0.url", "https://a"}, {"servers.1.url", "https://b"}, {"servers.1.type", "Subsonic"},
                              {"servers.2.url", "https://c"}, {"servers.2.type", "AudioBookShelf"}});
    auto c = oap::serverConfigs(s);
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[0].type, "subsonic");
    EXPECT_EQ(c[1].type, "subsonic");
    EXPECT_EQ(c[2].type, "audiobookshelf");
}

TEST(ServerConfigs, AnEntryWithoutAUrlIsLeftOut) {
    Settings s = withServers({{"servers.0.user", "ice"}, {"servers.1.url", "https://b"}, {"servers.2.url", ""}, {"servers.2.name", "x"}});
    auto c = oap::serverConfigs(s);
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].url, "https://b");
}

TEST(ServerConfigs, ALoneServerSectionIsAListOfOne) {
    Settings s = withServers({{"server.url", "https://a"}, {"server.user", "ice"}, {"server.password", "pw"}, {"server.type", "Subsonic"},
                              {"server.name", "Home"}, {"server.audiobook_folder", "Books"}});
    std::vector<std::string> warnings;
    auto c = oap::serverConfigs(s, &warnings);
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].url, "https://a");
    EXPECT_EQ(c[0].user, "ice");
    EXPECT_EQ(c[0].password, "pw");
    EXPECT_EQ(c[0].type, "subsonic");
    EXPECT_EQ(c[0].name, "Home");
    EXPECT_EQ(c[0].audiobookFolder, "Books");
    EXPECT_TRUE(warnings.empty());
}

TEST(ServerConfigs, ServersWinOverServerAndAWarningSaysSo) {
    Settings s = withServers({{"server.url", "https://old"}, {"servers.0.url", "https://new"}});
    std::vector<std::string> warnings;
    auto c = oap::serverConfigs(s, &warnings);
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].url, "https://new");
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("server: is ignored"), std::string::npos) << warnings[0];
}

TEST(ServerConfigs, TheSameUrlAndUserTwiceIsOneServer) {
    Settings s = withServers({{"servers.0.url", "https://a"}, {"servers.0.user", "ice"}, {"servers.0.name", "First"},
                              {"servers.1.url", "https://a/"}, {"servers.1.user", "ice"}, {"servers.1.name", "Second"},
                              {"servers.2.url", "https://a"}, {"servers.2.user", "bob"}});
    std::vector<std::string> warnings;
    auto c = oap::serverConfigs(s, &warnings);
    ASSERT_EQ(c.size(), 2u) << "another user is another server";
    EXPECT_EQ(c[0].name, "First");
    EXPECT_EQ(c[1].user, "bob");
    EXPECT_EQ(warnings.size(), 1u);
}

TEST(ServerConfigs, AnIndexWithALeadingZeroIsNotAnEntry) {
    Settings s = withServers({{"servers.01.url", "https://b"}, {"server.url", "https://old"}});
    auto c = oap::serverConfigs(s);
    ASSERT_EQ(c.size(), 1u) << "no servers: list, so server: is read";
    EXPECT_EQ(c[0].url, "https://old");
}

TEST(Settings, ServerConfiguredWithAServersList) {
    EXPECT_TRUE(oap::serverConfigured(withServers({{"servers.0.url", "https://a"}})));
    EXPECT_FALSE(oap::serverConfigured(withServers({{"servers.0.user", "ice"}})));
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

TEST(Settings, EraseRemovesThatKeyAndNotTheKeysThatStartWithIt) {
    oap::Settings s;
    s.setString("catalogue.version", "v1");
    s.setString("catalogue.version-ab12.version", "v2");
    s.erase("catalogue.version");
    EXPECT_EQ(s.getString("catalogue.version"), "");
    EXPECT_EQ(s.getString("catalogue.version-ab12.version"), "v2");
    s.erase("never.set"); // nothing happens
}
