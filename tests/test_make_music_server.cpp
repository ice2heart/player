#include "frontend/make_music_server.h"

#include <gtest/gtest.h>

using namespace oap;

namespace {

Settings serverConfig(const std::string& type) {
    Settings config;
    if (!type.empty()) config.setString("server.type", type);
    config.setString("server.url", "https://music.example.com");
    config.setString("server.user", "alice");
    config.setString("server.password", "secret");
    return config;
}

} // namespace

TEST(MakeMusicServer, NoServerSetUpIsNotConfiguredAndSaysNothing) {
    testing::internal::CaptureStderr();
    auto server = makeMusicServer(Settings());
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(server, nullptr);
    EXPECT_FALSE(server->configured());
    EXPECT_EQ(err, "");
}

// OAP_TESTS_EXPECT_NET is set by CMake for a build with OAP_BUILD_NET.
TEST(MakeMusicServer, AnotherServerTypeIsNotConfiguredAndSaysWhy) {
    testing::internal::CaptureStderr();
    auto server = makeMusicServer(serverConfig("audiobookshelf"));
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(server, nullptr);
    EXPECT_FALSE(server->configured());
#ifdef OAP_TESTS_EXPECT_NET
    EXPECT_EQ(err, "server: type 'audiobookshelf' is not supported; streaming is off\n");
#else
    EXPECT_NE(err.find("no network support"), std::string::npos) << err;
#endif
}

// With network support a Subsonic server streams; without it the front end
// gets a server that is not configured and is told it is the build.
TEST(MakeMusicServer, SubsonicServerStreamsOnlyWithNetworkSupport) {
    testing::internal::CaptureStderr();
    auto server = makeMusicServer(serverConfig("subsonic"));
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(server, nullptr);
#ifdef OAP_TESTS_EXPECT_NET
    EXPECT_TRUE(server->configured());
    std::string url = server->stream("t1", 0).url; // the original file: no request is made for it
    EXPECT_EQ(url.rfind("https://music.example.com/rest/stream", 0), 0u) << url;
    EXPECT_EQ(err, "");
#else
    EXPECT_FALSE(server->configured());
    EXPECT_EQ(err, "server: this build has no network support (OAP_BUILD_NET=OFF); streaming is off\n");
#endif
}
