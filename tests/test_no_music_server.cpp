#include "core/no_music_server.h"

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
        server.catalogueVersion([&](Error e, std::string version) {
            EXPECT_TRUE(version.empty());
            errors.push_back(e);
        }),
        server.albums("newest", 0, 50, [&](Error e, std::vector<RemoteAlbum> v, bool m) { list(e, v, m); }),
        server.artists([&](Error e, std::vector<RemoteArtist> v, bool m) { list(e, v, m); }),
        server.artistAlbums("a1", [&](Error e, std::vector<RemoteAlbum> v, bool m) { list(e, v, m); }),
        server.albumTracks("al1", [&](Error e, std::vector<RemoteTrack> v, bool m) { list(e, v, m); }),
        server.playlists([&](Error e, std::vector<RemotePlaylist> v, bool m) { list(e, v, m); }),
        server.playlistTracks("p1", [&](Error e, std::vector<RemoteTrack> v, bool m) { list(e, v, m); }),
    };
    EXPECT_TRUE(errors.empty()) << "callbacks run only from poll()";
    server.poll();
    EXPECT_EQ(errors, std::vector<Error>(8, Error::NotConfigured));
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_NE(ids[i], 0u);
        for (size_t j = 0; j < i; ++j) EXPECT_NE(ids[i], ids[j]);
    }
    server.poll();
    EXPECT_EQ(errors.size(), 8u) << "each callback runs once";
}

TEST(NoMusicServer, CreatePlaylistAnswersNotConfiguredAndSendsNothing) {
    NoMusicServer server;
    std::vector<Error> got;
    auto id = server.createPlaylist("Queue", {"a"}, [&](Error e, std::string) { got.push_back(e); });
    EXPECT_NE(id, 0u);
    EXPECT_TRUE(got.empty()) << "answered from poll()";
    server.poll();
    EXPECT_EQ(got, std::vector<Error>{Error::NotConfigured});
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

TEST(NoMusicServer, BookFinishesWithNotConfiguredAndAnEmptyBookOnTheNextPoll) {
    NoMusicServer server;
    int calls = 0;
    auto id = server.book("al1", [&](Error e, RemoteBook book) {
        ++calls;
        EXPECT_EQ(e, Error::NotConfigured);
        EXPECT_TRUE(book.tracks.empty());
        EXPECT_TRUE(book.chapters.empty());
    });
    EXPECT_NE(id, 0u);
    EXPECT_EQ(calls, 0);
    server.poll();
    EXPECT_EQ(calls, 1);
    int cancelledCalls = 0;
    server.cancel(server.book("al1", [&](Error, RemoteBook) { ++cancelledCalls; }));
    server.poll();
    EXPECT_EQ(cancelledCalls, 0);
    EXPECT_EQ(server.catalogue(), MusicServer::Catalogue::Music);
}

TEST(NoMusicServer, ProgressIsNotConfiguredAndAcknowledgedFromPoll) {
    NoMusicServer server;
    EXPECT_FALSE(server.syncsProgress());
    int read = 0, saved = 0;
    server.progress("a", [&](Error error, std::optional<oap::RemoteProgress> value) {
        ++read;
        EXPECT_EQ(error, Error::NotConfigured);
        EXPECT_FALSE(value);
    });
    server.saveProgress("a", {}, [&](Error error) { ++saved; EXPECT_EQ(error, Error::NotConfigured); });
    server.saveProgress("a", {});
    EXPECT_EQ(read, 0);
    EXPECT_EQ(saved, 0);
    server.poll();
    EXPECT_EQ(read, 1);
    EXPECT_EQ(saved, 1);
    server.cancel(server.progress("a", [&](Error, std::optional<oap::RemoteProgress>) { ++read; }));
    server.poll();
    EXPECT_EQ(read, 1);
}
