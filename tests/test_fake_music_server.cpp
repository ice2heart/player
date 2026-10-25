// The FakeMusicServer is what later App tests use to stand in for a server, so
// its contract is checked here: callbacks run only from poll() and only after
// the test answered, at most once, never after cancel(), and ids are unique.

#include "fakes.h"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>

using oap::RemoteAlbum;
using oap::RemoteTrack;
using Server = oap::MusicServer;
using oap::test::FakeMusicServer;
using Kind = FakeMusicServer::Kind;

namespace {

RemoteAlbum album(std::string id, std::string name) {
    RemoteAlbum a;
    a.id = std::move(id);
    a.name = std::move(name);
    return a;
}

// Collects what an album callback received.
struct AlbumResult {
    int calls = 0;
    Server::Error error = Server::Error::Server;
    std::vector<RemoteAlbum> albums;
    bool more = false;

    Server::Done<RemoteAlbum> callback() {
        return [this](Server::Error e, std::vector<RemoteAlbum> a, bool m) {
            ++calls;
            error = e;
            albums = std::move(a);
            more = m;
        };
    }
};

} // namespace

TEST(FakeMusicServer, CallbackRunsOnlyFromPollAfterReplyAndOnce) {
    FakeMusicServer server;
    AlbumResult result;
    auto id = server.albums("newest", 0, 500, result.callback());

    server.poll();
    EXPECT_EQ(result.calls, 0) << "no answer yet: still loading";

    ASSERT_TRUE(server.reply(id, {album("al-1", "Storm")}, true));
    EXPECT_EQ(result.calls, 0) << "answered, but callbacks run only from poll()";

    server.poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Server::Error::None);
    ASSERT_EQ(result.albums.size(), 1u);
    EXPECT_EQ(result.albums[0].name, "Storm");
    EXPECT_TRUE(result.more);

    server.poll();
    EXPECT_EQ(result.calls, 1) << "a callback runs once";
    EXPECT_FALSE(server.reply(id, std::vector<RemoteAlbum>{})) << "already answered";
}

TEST(FakeMusicServer, RecordsEachRequestWithItsArguments) {
    FakeMusicServer server;
    auto ignore = [](auto...) {};
    auto a = server.albums("alphabeticalByName", 500, 250, ignore);
    auto t = server.albumTracks("al-7", ignore);
    auto p = server.playlistTracks("pl-3", ignore);

    ASSERT_EQ(server.requests.size(), 3u);
    EXPECT_EQ(server.requests[0].id, a);
    EXPECT_EQ(server.requests[0].kind, Kind::Albums);
    EXPECT_EQ(server.requests[0].arg, "alphabeticalByName");
    EXPECT_EQ(server.requests[0].offset, 500);
    EXPECT_EQ(server.requests[0].size, 250);
    EXPECT_EQ(server.requests[1].id, t);
    EXPECT_EQ(server.requests[1].kind, Kind::AlbumTracks);
    EXPECT_EQ(server.requests[1].arg, "al-7");
    EXPECT_EQ(server.lastRequest().id, p);
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "pl-3");
    EXPECT_EQ(server.pendingCount(), 3u);
}

TEST(FakeMusicServer, IdsAreUniqueAndNeverZero) {
    FakeMusicServer server;
    auto ignore = [](auto...) {};
    std::set<Server::RequestId> ids{server.ping(ignore), server.artists(ignore), server.artistAlbums("ar-1", ignore),
                                    server.playlists(ignore), server.albumTracks("al-1", ignore)};
    EXPECT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids.count(0), 0u);
}

TEST(FakeMusicServer, CancelBeforeTheAnswerDropsIt) {
    FakeMusicServer server;
    AlbumResult result;
    auto id = server.albums("newest", 0, 500, result.callback());

    server.cancel(id);
    EXPECT_FALSE(server.reply(id, {album("al-1", "Storm")})) << "a late answer is dropped";
    server.poll();
    EXPECT_EQ(result.calls, 0);
    EXPECT_EQ(server.cancelled, std::vector<Server::RequestId>{id});
}

TEST(FakeMusicServer, CancelAfterTheAnswerButBeforePollDropsIt) {
    FakeMusicServer server;
    AlbumResult cancelledResult, keptResult;
    auto dropped = server.albums("newest", 0, 500, cancelledResult.callback());
    auto kept = server.albums("random", 0, 50, keptResult.callback());
    server.reply(dropped, {album("al-1", "Storm")});
    server.reply(kept, {album("al-2", "Calm")});

    server.cancel(dropped);
    server.poll();
    EXPECT_EQ(cancelledResult.calls, 0);
    EXPECT_EQ(keptResult.calls, 1) << "cancel drops only its own request";
}

TEST(FakeMusicServer, CancelFromAnEarlierCallbackInTheSamePollDropsIt) {
    FakeMusicServer server;
    AlbumResult second;
    Server::RequestId secondId = 0;
    auto first = server.ping([&](Server::Error, Server::ServerInfo) { server.cancel(secondId); });
    secondId = server.albums("newest", 0, 500, second.callback());
    server.replyPing(first, {"navidrome", "0.64.2"});
    server.reply(secondId, {album("al-1", "Storm")});

    server.poll();
    EXPECT_EQ(second.calls, 0);
}

TEST(FakeMusicServer, FailDeliversTheErrorWithNoItems) {
    FakeMusicServer server;
    AlbumResult result;
    auto id = server.albums("newest", 0, 500, result.callback());
    Server::Error pingError = Server::Error::None;
    auto ping = server.ping([&](Server::Error e, Server::ServerInfo) { pingError = e; });

    server.fail(id, Server::Error::Unreachable);
    server.fail(ping, Server::Error::BadLogin);
    server.poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Server::Error::Unreachable);
    EXPECT_TRUE(result.albums.empty());
    EXPECT_FALSE(result.more);
    EXPECT_EQ(pingError, Server::Error::BadLogin);
    EXPECT_THROW(server.fail(server.artists([](auto...) {}), Server::Error::None), std::logic_error)
        << "Error::None is not a failure: a test bug";
}

TEST(FakeMusicServer, NotConfiguredAnswersEveryRequestWithThatError) {
    FakeMusicServer server;
    server.isConfigured = false;
    AlbumResult result;
    auto id = server.albums("newest", 0, 500, result.callback());
    EXPECT_EQ(result.calls, 0) << "still only from poll()";
    EXPECT_FALSE(server.reply(id, {album("al-1", "Storm")})) << "already answered";

    server.poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Server::Error::NotConfigured);
    EXPECT_TRUE(result.albums.empty());
    EXPECT_EQ(server.lastRequest().kind, Kind::Albums) << "still recorded";
}

TEST(FakeMusicServer, PingReturnsTheServerInfo) {
    FakeMusicServer server;
    Server::ServerInfo info;
    auto id = server.ping([&](Server::Error, Server::ServerInfo i) { info = i; });
    server.replyPing(id, {"navidrome", "0.64.2"});
    server.poll();
    EXPECT_EQ(info.type, "navidrome");
    EXPECT_EQ(info.version, "0.64.2");
}

TEST(FakeMusicServer, ReplyWithTheWrongItemTypeIsATestBug) {
    FakeMusicServer server;
    auto id = server.albumTracks("al-1", [](auto...) {});
    EXPECT_THROW(server.reply(id, std::vector<RemoteAlbum>{}), std::logic_error);
    EXPECT_TRUE(server.reply(id, std::vector<RemoteTrack>(1)));
}

TEST(FakeMusicServer, StreamUrlNamesTheTrackAndATranscode) {
    FakeMusicServer server;
    auto original = server.stream("tr-42", 0);
    EXPECT_EQ(original.url, "fake://stream/tr-42");
    EXPECT_FALSE(original.transcoded);
    auto transcode = server.stream("tr-42", 128);
    EXPECT_EQ(transcode.url, "fake://stream/tr-42?maxBitRate=128");
    ASSERT_TRUE(transcode.transcoded);
    EXPECT_EQ(transcode.transcoded->offsetUrl, "fake://stream/tr-42?maxBitRate=128&timeOffset=");
}

TEST(FakeMusicServer, ScrobblesAreRecordedInOrderAndMakeNoRequest) {
    FakeMusicServer server;
    server.scrobble("t1", false);
    server.scrobble("t1", true);
    EXPECT_EQ(server.scrobbles,
              (std::vector<FakeMusicServer::Scrobble>{{"t1", false}, {"t1", true}}));
    EXPECT_TRUE(server.requests.empty());
    EXPECT_EQ(server.pendingCount(), 0u);
}
