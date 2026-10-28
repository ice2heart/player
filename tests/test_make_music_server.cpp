#include "core/track_cache.h"
#include "frontend/make_music_server.h"

#include <gtest/gtest.h>

using namespace oap;

namespace {

// One `servers:` entry of a config.
void addServer(Settings& config, int index, const std::string& type, const std::string& url, const std::string& user,
               const std::string& name = {}) {
    const std::string prefix = "servers." + std::to_string(index) + ".";
    if (!type.empty()) config.setString(prefix + "type", type);
    if (!name.empty()) config.setString(prefix + "name", name);
    config.setString(prefix + "url", url);
    config.setString(prefix + "user", user);
    config.setString(prefix + "password", "secret");
}

struct Made {
    Servers servers;
    std::string err;
};

Made make(const Settings& config) {
    testing::internal::CaptureStderr();
    Made made{makeServers(config), {}};
    made.err = testing::internal::GetCapturedStderr();
    return made;
}

} // namespace

TEST(MakeServers, NoServerSetUpGivesNoLinkAndSaysNothing) {
    Made made = make(Settings());
    EXPECT_TRUE(made.servers.links().empty());
    EXPECT_EQ(made.err, "");
}

// OAP_TESTS_EXPECT_NET is set by CMake for a build with OAP_BUILD_NET.
TEST(MakeServers, TwoSubsonicEntriesGiveTwoLinksWithTheirNamesAndKeysOnlyWithNetworkSupport) {
    Settings config;
    addServer(config, 0, "subsonic", "https://music.example.com", "alice", "Music");
    addServer(config, 1, "", "https://other.example.com/", "bob");
    Made made = make(config);
#ifdef OAP_TESTS_EXPECT_NET
    ASSERT_EQ(made.servers.links().size(), 2u);
    const ServerLink& first = made.servers.links()[0];
    const ServerLink& second = made.servers.links()[1];
    EXPECT_EQ(first.name, "Music");
    EXPECT_EQ(second.name, "Server 2") << "no name: its place in the list";
    EXPECT_EQ(first.key, TrackCache::serverKey("https://music.example.com", "alice"));
    EXPECT_EQ(second.key, TrackCache::serverKey("https://other.example.com", "bob"));
    EXPECT_NE(first.key, second.key);
    EXPECT_EQ(first.config.url, "https://music.example.com");
    EXPECT_EQ(second.config.user, "bob");
    EXPECT_NE(&first.server, &second.server);
    EXPECT_NE(&first.downloader, &second.downloader);
    EXPECT_TRUE(first.server.configured());
    EXPECT_TRUE(first.downloader.configured());
    // Each link talks to its own server.
    EXPECT_EQ(first.server.stream("t1", 0).url.rfind("https://music.example.com/rest/stream", 0), 0u);
    EXPECT_EQ(second.server.stream("t1", 0).url.rfind("https://other.example.com/rest/stream", 0), 0u);
    EXPECT_EQ(made.err, "");
#else
    EXPECT_TRUE(made.servers.links().empty());
    EXPECT_EQ(made.err, "server: this build has no network support (OAP_BUILD_NET=OFF); streaming is off\n")
        << "once, not once per server";
#endif
}

TEST(MakeServers, ALoneServerWithoutANameIsCalledServer) {
    Settings config;
    config.setString("server.url", "https://music.example.com");
    config.setString("server.user", "alice");
    Made made = make(config);
#ifdef OAP_TESTS_EXPECT_NET
    ASSERT_EQ(made.servers.links().size(), 1u) << "the old server: section still works";
    EXPECT_EQ(made.servers.links()[0].name, "Server");
    EXPECT_EQ(made.servers.links()[0].key, TrackCache::serverKey("https://music.example.com", "alice"));
    EXPECT_EQ(made.err, "");
#else
    EXPECT_TRUE(made.servers.links().empty());
#endif
}

TEST(MakeServers, AServerOfAnUnknownTypeGetsNoLinkAndSaysWhy) {
    Settings config;
    addServer(config, 0, "plex", "https://plex.example.com", "alice");
    Made made = make(config);
    EXPECT_TRUE(made.servers.links().empty());
#ifdef OAP_TESTS_EXPECT_NET
    EXPECT_EQ(made.err, "server: type 'plex' is not supported; streaming is off\n");
#else
    EXPECT_NE(made.err.find("no network support"), std::string::npos) << made.err;
#endif
}

// A server that gets no link still takes its place in the list for the names of the others.
TEST(MakeServers, AnUnusableEntryDoesNotMoveTheNamesOfTheOthers) {
    Settings config;
    addServer(config, 0, "plex", "https://plex.example.com", "alice");
    addServer(config, 1, "subsonic", "https://music.example.com", "alice");
    Made made = make(config);
#ifdef OAP_TESTS_EXPECT_NET
    ASSERT_EQ(made.servers.links().size(), 1u);
    EXPECT_EQ(made.servers.links()[0].name, "Server 2");
    EXPECT_EQ(made.err, "server: type 'plex' is not supported; streaming is off\n");
#else
    EXPECT_TRUE(made.servers.links().empty());
#endif
}

TEST(MakeServers, WhatTheConfigIgnoresIsSaidOnStderr) {
    Settings config;
    config.setString("server.url", "https://old.example.com");
    addServer(config, 0, "plex", "https://plex.example.com", "alice");
    Made made = make(config);
    EXPECT_NE(made.err.find("server: is ignored"), std::string::npos) << made.err;
}

TEST(MakeServers, FinishWithNoServerReturnsAtOnce) {
    Made made = make(Settings());
    made.servers.finish(std::chrono::milliseconds(10));
}
