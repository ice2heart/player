#include "config/atomic_file.h"
#include "config/settings_files.h"
#include "config/stores.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace oap;
namespace fs = std::filesystem;

namespace {

class ConfigTest : public testing::Test {
protected:
    void SetUp() override {
        dir = test::testTempDir();
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    void write(const fs::path& file, const std::string& text) { std::ofstream(file) << text; }
    std::string read(const fs::path& file) { return readFile(file).value_or("<missing>"); }
    fs::perms mode(const fs::path& file) { return fs::status(file).permissions() & fs::perms::all; }

    fs::path dir;
};

constexpr fs::perms kOwnerOnly = fs::perms::owner_read | fs::perms::owner_write;

} // namespace

TEST_F(ConfigTest, LocalPlaylistsRoundTripDuplicatesAndDelete) {
    DiskLocalPlaylists store(dir / "playlists");
    auto first = store.create("Playlist, 27 October 14:05");
    auto second = store.create("Playlist, 27 October 14:05");
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->id, second->id);
    auto a = dir / "music" / "one.mp3";
    auto b = dir / "music" / "two.flac";
    auto c = dir / "music" / "three.mp3";
    ASSERT_TRUE(store.add(first->id, a));
    ASSERT_TRUE(store.add(first->id, c));
    ASSERT_TRUE(store.add(first->id, b));
    ASSERT_TRUE(store.add(first->id, a));
    DiskLocalPlaylists reopened(dir / "playlists");
    auto lists = reopened.list();
    ASSERT_EQ(lists.size(), 2u);
    EXPECT_EQ(lists[0].name, first->name);
    EXPECT_EQ(lists[0].tracks, (std::vector<fs::path>{a, c, b, a}));
    ASSERT_TRUE(reopened.removeTrack(first->id, 1));
    EXPECT_EQ(reopened.list()[0].tracks, (std::vector<fs::path>{a, b, a}));
    EXPECT_FALSE(reopened.removeTrack(first->id, 3));
    ASSERT_TRUE(reopened.remove(first->id));
    EXPECT_FALSE(reopened.remove(first->id));
    ASSERT_EQ(reopened.list().size(), 1u);
    EXPECT_EQ(reopened.list()[0].id, second->id);
}

TEST_F(ConfigTest, LocalPlaylistsReadExternalM3uAndSkipUrls) {
    auto root = dir / "playlists";
    fs::create_directories(root);
    write(root / "existing.m3u", "#EXTM3U\n#PLAYLIST:Road trip\n../music/a.mp3\nhttps://example.invalid/audio\n");
    DiskLocalPlaylists store(root);
    auto lists = store.list();
    ASSERT_EQ(lists.size(), 1u);
    EXPECT_EQ(lists[0].name, "Road trip");
    EXPECT_EQ(lists[0].tracks, (std::vector<fs::path>{dir / "music/a.mp3"}));
    EXPECT_FALSE(store.add(lists[0].id, "https://example.invalid/audio"));
    EXPECT_FALSE(store.add(lists[0].id, "relative.mp3"));
    EXPECT_FALSE(store.add("../outside.m3u", dir / "music/a.mp3"));
}

TEST_F(ConfigTest, LocalPlaylistWriteFailureLeavesOldContents) {
    auto root = dir / "playlists";
    DiskLocalPlaylists store(root);
    auto playlist = store.create("Original");
    ASSERT_TRUE(playlist);
    const auto old = read(root / playlist->id);
    fs::create_directory(root / (playlist->id + ".tmp"));
    EXPECT_FALSE(store.add(playlist->id, dir / "a.mp3"));
    EXPECT_EQ(read(root / playlist->id), old);
    EXPECT_TRUE(store.list()[0].tracks.empty());
    write(dir / "not-a-folder", "x");
    DiskLocalPlaylists broken(dir / "not-a-folder" / "playlists");
    EXPECT_FALSE(broken.create("No"));
}

// ---------------------------------------------------------------- YAML <-> dotted keys

TEST_F(ConfigTest, NestedYamlBecomesDottedKeys) {
    auto values = flattenYaml("speech:\n  rate: 250\nvolume: 70\nbuttons:\n  long_press_ms: 600\n"
                              "server:\n  url: https://music.example\n  name: Home server\n");
    EXPECT_EQ(values, (SettingsMap{{"speech.rate", "250"},
                                   {"volume", "70"},
                                   {"buttons.long_press_ms", "600"},
                                   {"server.url", "https://music.example"},
                                   {"server.name", "Home server"}}));
}

TEST_F(ConfigTest, DottedKeysBecomeNestedYamlInAFixedOrder) {
    std::string text = nestYaml({{"zz.extra", "x"},
                                 {"server.url", "https://music.example"},
                                 {"volume", "70"},
                                 {"speech.rate", "250"},
                                 {"skip.back", "10"},
                                 {"streaming.bitrate", "128"},
                                 {"skip.forward", "30"}});
    EXPECT_EQ(text.rfind("# ", 0), 0u) << "starts with the header comment";
    EXPECT_NE(text.find("speech.rate, volume"), std::string::npos) << "the header names the keys";
    std::string body = text.substr(text.find("\nspeech:") + 1);
    EXPECT_EQ(body, "speech:\n"
                    "  rate: 250\n"
                    "volume: 70\n"
                    "skip:\n"
                    "  back: 10\n"
                    "  forward: 30\n"
                    "streaming:\n"
                    "  bitrate: 128\n"
                    "server:\n"
                    "  url: \"https://music.example\"\n"
                    "zz:\n"
                    "  extra: \"x\"\n");
}

TEST_F(ConfigTest, TheCacheSizeIsWrittenAfterStreamingBeforeTheServerAndNamedInTheHeader) {
    std::string text = nestYaml({{"server.url", "https://music.example"}, {"cache.limit_gb", "4"}, {"streaming.bitrate", "0"}});
    EXPECT_NE(text.find("cache.limit_gb"), std::string::npos) << "the header names the key";
    EXPECT_EQ(text.substr(text.find("\nstreaming:") + 1), "streaming:\n"
                                                           "  bitrate: 0\n"
                                                           "cache:\n"
                                                           "  limit_gb: 4\n"
                                                           "server:\n"
                                                           "  url: \"https://music.example\"\n");
    EXPECT_EQ(flattenYaml(text).at("cache.limit_gb"), "4");
}

TEST_F(ConfigTest, TheAudiobookFolderIsKeptInTheServerSectionAfterThePasswordAndNamedInTheHeader) {
    std::string text = nestYaml({{"server.audiobook_folder", "Books"}, {"server.password", "x"}, {"server.url", "https://music.example"}});
    EXPECT_NE(text.find("audiobook_folder (the name"), std::string::npos) << "the header names the key";
    EXPECT_EQ(text.substr(text.find("\nserver:") + 1), "server:\n"
                                                       "  url: \"https://music.example\"\n"
                                                       "  password: \"x\"\n"
                                                       "  audiobook_folder: \"Books\"\n");
    EXPECT_EQ(flattenYaml(text).at("server.audiobook_folder"), "Books");
}

TEST_F(ConfigTest, KeysSurviveARoundTrip) {
    SettingsMap values{{"speech.rate", "250"},     {"menu.wrap", "0"},           {"server.name", "Home: server #1"},
                       {"server.user", "ice"},     {"streaming.bitrate", "0"},   {"server.type", "subsonic"},
                       {"server.password", "p@ss \"word\"\n"}, {"servers.0.url", "a"}, {"servers.1.url", "b"},
                       {"servers.10.url", "k"},   {"note", "true"},             {"empty", ""},
                       {"negative", "-5"},         {"leading", "007"}};
    for (int i = 2; i < 10; ++i) values["servers." + std::to_string(i) + ".url"] = std::to_string(i);
    EXPECT_EQ(flattenYaml(nestYaml(values)), values);
}

TEST_F(ConfigTest, NumbersAreWrittenPlainAndEverythingElseQuoted) {
    std::string yaml = nestYaml({{"a", "70"}, {"b", "-5"}, {"c", "007"}, {"d", "true"}, {"e", "1e3"}, {"f", ""}});
    EXPECT_NE(yaml.find("a: 70\nb: -5\nc: \"007\"\nd: \"true\"\ne: \"1e3\"\nf: \"\"\n"), std::string::npos) << yaml;
}

TEST_F(ConfigTest, AValueAndASectionOfTheSameNameKeepTheSection) {
    EXPECT_EQ(flattenYaml(nestYaml({{"skip", "10"}, {"skip.back", "5"}})), (SettingsMap{{"skip.back", "5"}}));
    EXPECT_EQ(flattenYaml(nestYaml({{"a", "1"}, {"a.b", "2"}})), (SettingsMap{{"a.b", "2"}})) << "value first";
}

TEST_F(ConfigTest, ListsAreWrittenBackAsLists) {
    std::string yaml = nestYaml(flattenYaml("servers:\n  - url: a\n  - url: b\n"));
    EXPECT_NE(yaml.find("servers:\n  - url: \"a\"\n  - url: \"b\"\n"), std::string::npos) << yaml;
}

TEST_F(ConfigTest, AnAllDigitQuotedPasswordStaysAString) {
    auto values = flattenYaml("server:\n  password: \"0123\"\n");
    EXPECT_EQ(values.at("server.password"), "0123");
    std::string yaml = nestYaml({{"server.password", "1234"}});
    EXPECT_NE(yaml.find("password: \"1234\""), std::string::npos) << "written quoted: " << yaml;
    EXPECT_EQ(flattenYaml(yaml).at("server.password"), "1234");
}

TEST_F(ConfigTest, AnAllDigitApiKeyStaysAQuotedString) {
    std::string yaml = nestYaml({{"servers.0.api_key", "1234"}, {"servers.0.password", "5678"}, {"servers.0.url", "https://a"}});
    EXPECT_NE(yaml.find("api_key: \"1234\""), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("password: \"5678\""), std::string::npos) << yaml;
    auto values = flattenYaml(yaml);
    EXPECT_EQ(values.at("servers.0.api_key"), "1234");
    Settings config;
    for (const auto& [k, v] : values) config.setString(k, v);
    EXPECT_EQ(serverConfigs(config).at(0).apiKey, "1234");
}

TEST_F(ConfigTest, AServerEntryIsWrittenInTheDocumentedKeyOrder) {
    SettingsMap values;
    for (int i = 0; i < 2; ++i)
        for (const char* k : {"library", "audiobook_folder", "api_key", "password", "user", "url", "name", "type"})
            values["servers." + std::to_string(i) + "." + k] = std::string(k) + std::to_string(i);
    values["speech.rate"] = "250";
    std::string yaml = nestYaml(values);
    EXPECT_NE(yaml.find("servers:\n  - type: \"type0\"\n    name: \"name0\"\n    url: \"url0\"\n    user: \"user0\"\n"
                        "    password: \"password0\"\n    api_key: \"api_key0\"\n    audiobook_folder: \"audiobook_folder0\"\n"
                        "    library: \"library0\"\n  - type: \"type1\""),
              std::string::npos)
        << yaml;
    EXPECT_EQ(flattenYaml(yaml), values);
}

TEST_F(ConfigTest, TheHeaderNamesTheServersKeys) {
    std::string yaml = nestYaml({});
    for (const char* text : {"a servers: list, each entry with type (subsonic or audiobookshelf), name, url, user,",
                             "password (quote it), api_key (quote it), audiobook_folder and library."})
        EXPECT_NE(yaml.find(text), std::string::npos) << text;
}

TEST_F(ConfigTest, AMissingServerUrlMeansNotConfigured) {
    for (const char* yaml : {"volume: 70\n", "server:\n", "server:\n  url:\n  user: ice\n", "server:\n  url: ~\n",
                             "server:\n  url: \"\"\n"}) {
        Settings config;
        YamlStore store(dir / "config.yaml");
        write(dir / "config.yaml", yaml);
        config = Settings(&store);
        config.load();
        EXPECT_FALSE(serverConfigured(config)) << yaml;
    }
    write(dir / "config.yaml", "server:\n  url: https://music.example\n");
    YamlStore store(dir / "config.yaml");
    Settings config(&store);
    config.load();
    EXPECT_TRUE(serverConfigured(config));
}

// ---------------------------------------------------------------- files

TEST_F(ConfigTest, YamlStoreSavesAndLoads) {
    YamlStore store(dir / "sub" / "config.yaml");
    EXPECT_TRUE(store.load().empty()) << "no file yet";
    store.save({{"speech.rate", "250"}, {"server.url", "https://music.example"}});
    EXPECT_EQ(YamlStore(dir / "sub" / "config.yaml").load(),
              (SettingsMap{{"speech.rate", "250"}, {"server.url", "https://music.example"}}));
}

TEST_F(ConfigTest, AFileWithOnlyCommentsIsEmptyAndCanBeSaved) {
    write(dir / "config.yaml", "# nothing set yet\n");
    YamlStore store(dir / "config.yaml");
    EXPECT_TRUE(store.load().empty());
    store.save({{"volume", "50"}});
    EXPECT_EQ(YamlStore(dir / "config.yaml").load(), (SettingsMap{{"volume", "50"}}));
}

TEST_F(ConfigTest, AFileThatCannotBeParsedIsNeverOverwritten) {
    std::string broken = "server:\n  url: [unclosed\n";
    write(dir / "config.yaml", broken);
    YamlStore store(dir / "config.yaml");
    EXPECT_TRUE(store.load().empty());
    store.save({{"volume", "50"}});
    EXPECT_EQ(read(dir / "config.yaml"), broken);
}

TEST_F(ConfigTest, AFileThatIsNotASetOfKeysIsNeverOverwritten) {
    std::string tsv = "speech.rate\t220\npos:/books/A\t1:42.5\n"; // an old settings.tsv passed as --config
    for (const std::string& text : {tsv, std::string("- 1\n- 2\n")}) {
        write(dir / "config.yaml", text);
        YamlStore store(dir / "config.yaml");
        EXPECT_TRUE(store.load().empty());
        store.save({{"volume", "50"}});
        EXPECT_EQ(read(dir / "config.yaml"), text);
    }
}

// E.g. edited with sudo and now owned by root: the server set up in it must survive.
TEST_F(ConfigTest, AFileThatCannotBeReadIsNeverOverwritten) {
    std::string text = "server:\n  url: https://music.example\n";
    write(dir / "config.yaml", text);
    fs::permissions(dir / "config.yaml", fs::perms::none);
    if (readFile(dir / "config.yaml")) { // root reads it anyway
        fs::permissions(dir / "config.yaml", kOwnerOnly);
        GTEST_SKIP() << "the file stays readable for this user";
    }
    YamlStore store(dir / "config.yaml");
    EXPECT_TRUE(store.load().empty());
    EXPECT_FALSE(store.save({{"volume", "50"}}));
    fs::permissions(dir / "config.yaml", kOwnerOnly);
    EXPECT_EQ(read(dir / "config.yaml"), text);
}

TEST_F(ConfigTest, TsvStoreSavesAndLoads) {
    TsvStore store(dir / "state.tsv");
    EXPECT_TRUE(store.load().empty());
    store.save({{"pos:/books/A Book", "2:41.5"}});
    EXPECT_EQ(read(dir / "state.tsv"), "pos:/books/A Book\t2:41.5\n");
    EXPECT_EQ(TsvStore(dir / "state.tsv").load(), (SettingsMap{{"pos:/books/A Book", "2:41.5"}}));
}

TEST_F(ConfigTest, AWrittenFileKeepsMode0600) {
    YamlStore yaml(dir / "config.yaml");
    yaml.save({{"server.password", "secret"}});
    EXPECT_EQ(mode(dir / "config.yaml"), kOwnerOnly);
    TsvStore tsv(dir / "state.tsv");
    tsv.save({{"last_book", "/x"}});
    EXPECT_EQ(mode(dir / "state.tsv"), kOwnerOnly);

    // An old file readable by everyone, and a temp file left by a crash, both with mode 0644.
    fs::permissions(dir / "config.yaml", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                             fs::perms::others_read);
    write(dir / "config.yaml.tmp", "old");
    fs::permissions(dir / "config.yaml.tmp", fs::perms::owner_read | fs::perms::owner_write |
                                                 fs::perms::group_read | fs::perms::others_read);
    yaml.save({{"server.password", "secret"}});
    EXPECT_EQ(mode(dir / "config.yaml"), kOwnerOnly);
}

TEST_F(ConfigTest, AnAtomicWriteReplacesTheFileAndLeavesNoTempFile) {
    write(dir / "a.txt", "old");
    EXPECT_TRUE(writeFileAtomically(dir / "a.txt", "new"));
    EXPECT_EQ(read(dir / "a.txt"), "new");
    EXPECT_FALSE(fs::exists(dir / "a.txt.tmp"));
}

// E.g. config.yaml linked to a file on the boot partition, to edit it from a PC.
TEST_F(ConfigTest, AnAtomicWriteToALinkWritesTheFileItPointsTo) {
    fs::create_directories(dir / "boot");
    write(dir / "boot" / "config.yaml", "old");
    fs::create_symlink(fs::path("boot") / "config.yaml", dir / "config.yaml");
    EXPECT_TRUE(writeFileAtomically(dir / "config.yaml", "new"));
    EXPECT_TRUE(fs::is_symlink(dir / "config.yaml")) << "the link stays a link";
    EXPECT_EQ(read(dir / "boot" / "config.yaml"), "new");
    EXPECT_FALSE(fs::exists(dir / "config.yaml.tmp"));
    EXPECT_FALSE(fs::exists(dir / "boot" / "config.yaml.tmp"));
}

TEST_F(ConfigTest, AFailedWriteKeepsTheOldFile) {
    write(dir / "a.txt", "old");
    fs::create_directories(dir / "a.txt.tmp"); // the temp file cannot be created
    EXPECT_FALSE(writeFileAtomically(dir / "a.txt", "new"));
    EXPECT_EQ(read(dir / "a.txt"), "old");
}

// ---------------------------------------------------------------- front ends

TEST_F(ConfigTest, SettingsFilesLoadBoth) {
    write(dir / "config.yaml", "volume: 40\n");
    write(dir / "state.tsv", "last_book\t/books/A\n");
    SettingsFiles files(dir / "config.yaml", defaultStateFile(dir / "config.yaml"));
    EXPECT_EQ(files.config().getInt("volume", 0), 40);
    EXPECT_EQ(files.config().getString("last_book"), "");
    EXPECT_EQ(files.state().getString("last_book"), "/books/A");
    EXPECT_EQ(files.state().getInt("volume", 0), 0);
    files.state().setString("last_book", "/books/B");
    files.state().save();
    EXPECT_EQ(read(dir / "state.tsv"), "last_book\t/books/B\n");
}
