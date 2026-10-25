#include "frontend/no_music_server.h"

#include <gtest/gtest.h>

using namespace oap;
using Error = MusicServer::Error;

TEST(NoMusicServer, IsNotConfiguredAndHasNoStreamUrl) {
    NoMusicServer server;
    EXPECT_FALSE(server.configured());
    EXPECT_EQ(server.stream("t1", 0).url, "");
    EXPECT_EQ(server.stream("t1", 128).url, "");
}

TEST(NoMusicServer, EveryRequestFinishesWithNotConfiguredOnTheNextPoll) {
    NoMusicServer server;
    std::vector<Error> errors;
    auto list = [&](Error e, auto items, bool more) {
        EXPECT_TRUE(items.empty());
        EXPECT_FALSE(more);
        errors.push_back(e);
    };
    std::vector<MusicServer::RequestId> ids = {
        server.ping([&](Error e, MusicServer::ServerInfo) { errors.push_back(e); }),
        server.albums("newest", 0, 50, [&](Error e, std::vector<RemoteAlbum> v, bool m) { list(e, v, m); }),
        server.artists([&](Error e, std::vector<RemoteArtist> v, bool m) { list(e, v, m); }),
        server.artistAlbums("a1", [&](Error e, std::vector<RemoteAlbum> v, bool m) { list(e, v, m); }),
        server.albumTracks("al1", [&](Error e, std::vector<RemoteTrack> v, bool m) { list(e, v, m); }),
        server.playlists([&](Error e, std::vector<RemotePlaylist> v, bool m) { list(e, v, m); }),
        server.playlistTracks("p1", [&](Error e, std::vector<RemoteTrack> v, bool m) { list(e, v, m); }),
    };
    EXPECT_TRUE(errors.empty()) << "callbacks run only from poll()";
    server.poll();
    EXPECT_EQ(errors, std::vector<Error>(7, Error::NotConfigured));
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_NE(ids[i], 0u);
        for (size_t j = 0; j < i; ++j) EXPECT_NE(ids[i], ids[j]);
    }
    server.poll();
    EXPECT_EQ(errors.size(), 7u) << "each callback runs once";
}

TEST(NoMusicServer, ACancelledRequestNeverCallsBack) {
    NoMusicServer server;
    int calls = 0;
    auto cancelled = server.artists([&](Error, std::vector<RemoteArtist>, bool) { calls += 10; });
    server.playlists([&](Error, std::vector<RemotePlaylist>, bool) { calls += 1; });
    server.cancel(cancelled);
    server.cancel(12345); // unknown: ignored
    server.poll();
    EXPECT_EQ(calls, 1);
}

TEST(NoMusicServer, ARequestCancelledByAnEarlierCallbackInTheSamePollNeverCallsBack) {
    NoMusicServer server;
    int calls = 0;
    MusicServer::RequestId second = 0;
    server.artists([&](Error, std::vector<RemoteArtist>, bool) { server.cancel(second); }); // e.g. its screen closed
    second = server.playlists([&](Error, std::vector<RemotePlaylist>, bool) { ++calls; });
    server.poll();
    EXPECT_EQ(calls, 0);
}

TEST(NoMusicServer, ARequestMadeFromACallbackIsAnsweredOnTheNextPoll) {
    NoMusicServer server;
    int calls = 0;
    server.artists([&](Error, std::vector<RemoteArtist>, bool) {
        server.playlists([&](Error, std::vector<RemotePlaylist>, bool) { ++calls; });
    });
    server.poll();
    EXPECT_EQ(calls, 0);
    server.poll();
    EXPECT_EQ(calls, 1);
}
