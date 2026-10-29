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
using Error = Server::Error;
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

TEST(FakeMusicServer, CreatePlaylistRecordsNameAndIdsAndAnswersOnlyFromPoll) {
    FakeMusicServer server;
    std::vector<Server::Error> done;
    std::vector<std::string> createdIds;
    auto id = server.createPlaylist("Queue, 1 May 10:00", {"a", "b"},
                                    [&](Server::Error e, std::string playlist) { done.push_back(e), createdIds.push_back(playlist); });
    ASSERT_EQ(server.requests.size(), 1u);
    EXPECT_EQ(server.lastRequest().id, id);
    EXPECT_EQ(server.lastRequest().kind, Kind::CreatePlaylist);
    EXPECT_EQ(server.lastRequest().arg, "Queue, 1 May 10:00");
    EXPECT_EQ(server.lastRequest().trackIds, (std::vector<std::string>{"a", "b"}));
    EXPECT_TRUE(server.replyCreated(id, "pl-9"));
    EXPECT_TRUE(done.empty()) << "delivered by poll()";
    server.poll();
    EXPECT_EQ(done, std::vector<Server::Error>{Server::Error::None});
    EXPECT_EQ(createdIds, std::vector<std::string>{"pl-9"});
    EXPECT_FALSE(server.replyCreated(id)) << "already answered";

    auto failing = server.createPlaylist("x", {"a"}, [&](Server::Error e, std::string) { done.push_back(e); });
    EXPECT_TRUE(server.fail(failing, Server::Error::Unreachable));
    server.poll();
    EXPECT_EQ(done.back(), Server::Error::Unreachable);

    auto cancelled = server.createPlaylist("y", {"a"}, [&](Server::Error e, std::string) { done.push_back(e); });
    server.cancel(cancelled);
    EXPECT_FALSE(server.replyCreated(cancelled));
    server.poll();
    EXPECT_EQ(done.size(), 2u);
}

TEST(FakeMusicServer, CreatePlaylistWithTooManyTracksFailsAsTheRealServerDoes) {
    FakeMusicServer server;
    std::vector<Server::Error> done;
    server.createPlaylist("x", std::vector<std::string>(Server::kMaxPlaylistTracks + 1, "a"),
                          [&](Server::Error e, std::string) { done.push_back(e); });
    server.poll();
    EXPECT_EQ(done, std::vector<Server::Error>{Server::Error::Server});
}

TEST(FakeMusicServer, CreatePlaylistWithoutAServerAnswersNotConfigured) {
    FakeMusicServer server;
    server.isConfigured = false;
    std::vector<Server::Error> done;
    server.createPlaylist("x", {"a"}, [&](Server::Error e, std::string) { done.push_back(e); });
    server.poll();
    EXPECT_EQ(done, std::vector<Server::Error>{Server::Error::NotConfigured});
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
    server.scrobble("t1", false, std::nullopt, {});
    server.scrobble("t1", true, 42, {});
    EXPECT_EQ(server.scrobbles,
              (std::vector<FakeMusicServer::Scrobble>{{"t1", false}, {"t1", true}}));
    EXPECT_TRUE(server.requests.empty());
    EXPECT_EQ(server.pendingCount(), 0u);
    EXPECT_EQ(server.scrobbles[0].timeMs, std::nullopt);
    EXPECT_EQ(server.scrobbles[1].timeMs, 42);
}

TEST(FakeMusicServer, ScrobblesAreAnsweredByPollOrHeldUntilTheTestAnswers) {
    FakeMusicServer server;
    std::vector<Server::Error> got;
    auto done = [&](Server::Error e) { got.push_back(e); };
    server.scrobbleError = Server::Error::Unreachable;
    server.scrobble("t1", true, 1, done);
    EXPECT_TRUE(got.empty()) << "only poll() calls back";
    server.poll();
    EXPECT_EQ(got, (std::vector<Server::Error>{Server::Error::Unreachable}));
    got.clear();
    server.holdScrobbles = true;
    server.scrobble("t2", true, 2, done);
    server.poll();
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(server.heldScrobbleCount(), 1u);
    EXPECT_TRUE(server.answerScrobble(Server::Error::Server));
    EXPECT_FALSE(server.answerScrobble());
    server.poll();
    EXPECT_EQ(got, (std::vector<Server::Error>{Server::Error::Server}));
}

TEST(FakeMusicServer, CatalogueVersionIsAnsweredFailedAndCancelledLikeAnyRequest) {
    FakeMusicServer server;
    std::vector<std::pair<Server::Error, std::string>> got;
    auto done = [&](Server::Error e, std::string v) { got.emplace_back(e, std::move(v)); };
    auto id = server.catalogueVersion(done);
    EXPECT_EQ(server.lastRequest().kind, Kind::CatalogueVersion);
    server.poll();
    EXPECT_TRUE(got.empty());
    ASSERT_TRUE(server.replyVersion(id, "v1"));
    server.poll();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], std::make_pair(Server::Error::None, std::string("v1")));
    auto failing = server.catalogueVersion(done);
    ASSERT_TRUE(server.fail(failing, Server::Error::Unreachable));
    server.poll();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1], std::make_pair(Server::Error::Unreachable, std::string()));
    auto cancelled = server.catalogueVersion(done);
    server.cancel(cancelled);
    EXPECT_FALSE(server.replyVersion(cancelled, "v2"));
    server.poll();
    EXPECT_EQ(got.size(), 2u);
}

TEST(FakeMusicServer, BookIsAnsweredOnlyWhenTheTestSaysSoAndKeepsItsChapters) {
    FakeMusicServer server;
    std::vector<std::pair<Server::Error, oap::RemoteBook>> got;
    auto done = [&](Server::Error e, oap::RemoteBook b) { got.emplace_back(e, std::move(b)); };
    auto id = server.book("item-1", done);
    EXPECT_EQ(server.lastRequest().kind, Kind::Book);
    EXPECT_EQ(server.lastRequest().arg, "item-1");
    server.poll();
    EXPECT_TRUE(got.empty()) << "the fake answers a book only when told to";

    oap::RemoteBook book;
    book.tracks.push_back(RemoteTrack{.id = "item-1/7"});
    book.chapters.push_back({"One", 0});
    book.chapters.push_back({"Two", 61.5});
    ASSERT_TRUE(server.replyBook(id, book));
    EXPECT_FALSE(server.replyBook(id, book)) << "answered once";
    server.poll();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].first, Server::Error::None);
    ASSERT_EQ(got[0].second.tracks.size(), 1u);
    EXPECT_EQ(got[0].second.tracks[0].id, "item-1/7");
    ASSERT_EQ(got[0].second.chapters.size(), 2u);
    EXPECT_EQ(got[0].second.chapters[1].title, "Two");
    EXPECT_DOUBLE_EQ(got[0].second.chapters[1].start, 61.5);

    auto failing = server.book("item-2", done);
    ASSERT_TRUE(server.fail(failing, Server::Error::Server));
    server.poll();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].first, Server::Error::Server);
    EXPECT_TRUE(got[1].second.tracks.empty());

    auto cancelled = server.book("item-3", done);
    server.cancel(cancelled);
    EXPECT_FALSE(server.replyBook(cancelled, book));
    server.poll();
    EXPECT_EQ(got.size(), 2u);

    auto wrong = server.albumTracks("al", [](Server::Error, std::vector<RemoteTrack>, bool) {});
    EXPECT_THROW(server.replyBook(wrong, book), std::logic_error);
}

TEST(FakeMusicServer, BookOfAServerThatIsNotConfiguredFailsOnPoll) {
    FakeMusicServer server;
    server.isConfigured = false;
    Server::Error error = Server::Error::None;
    server.book("x", [&](Server::Error e, oap::RemoteBook) { error = e; });
    server.poll();
    EXPECT_EQ(error, Server::Error::NotConfigured);
}

TEST(FakeMusicServer, CatalogueIsMusicUnlessTheTestSaysBooks) {
    FakeMusicServer server;
    EXPECT_EQ(server.catalogue(), Server::Catalogue::Music);
    server.catalogueKind = Server::Catalogue::Books;
    EXPECT_EQ(server.catalogue(), Server::Catalogue::Books);
}

TEST(FakeMusicServer, ProgressRecordsReadsSavesAndHeldAcknowledgements) {
    FakeMusicServer server;
    EXPECT_FALSE(server.syncsProgress());
    server.progressSync = true;
    EXPECT_TRUE(server.syncsProgress());
    int calls = 0;
    auto id = server.progress("book", [&](Error error, std::optional<oap::RemoteProgress> value) {
        ++calls;
        EXPECT_EQ(error, Error::None);
        ASSERT_TRUE(value);
        EXPECT_DOUBLE_EQ(value->seconds, 35);
        EXPECT_EQ(value->updated, 1234567890123LL);
    });
    EXPECT_EQ(server.lastRequest().kind, Kind::Progress);
    EXPECT_EQ(server.lastRequest().arg, "book");
    server.poll();
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(server.replyProgress(id, oap::RemoteProgress{35, 100, false, 1234567890123LL}));
    EXPECT_EQ(calls, 0);
    server.poll();
    EXPECT_EQ(calls, 1);
    int saved = 0;
    server.holdProgressSaves = true;
    server.saveProgress("book", {35, 100, false, 1234567890123LL}, [&](Error error) {
        ++saved;
        EXPECT_EQ(error, Error::Unreachable);
    });
    ASSERT_EQ(server.progressSaves.size(), 1u);
    EXPECT_EQ(server.progressSaves[0].albumId, "book");
    EXPECT_DOUBLE_EQ(server.progressSaves[0].progress.seconds, 35);
    EXPECT_DOUBLE_EQ(server.progressSaves[0].progress.duration, 100);
    EXPECT_FALSE(server.progressSaves[0].progress.finished);
    EXPECT_EQ(server.progressSaves[0].progress.updated, 1234567890123LL);
    server.poll();
    EXPECT_EQ(saved, 0);
    EXPECT_TRUE(server.answerProgressSave(Error::Unreachable));
    EXPECT_EQ(saved, 0);
    server.poll();
    EXPECT_EQ(saved, 1);
    EXPECT_FALSE(server.answerProgressSave());
    server.holdProgressSaves = false;
    server.saveProgress("book", {100, 100, true, 1234567890124LL}, [&](Error error) {
        ++saved;
        EXPECT_EQ(error, Error::None);
    });
    server.poll();
    EXPECT_EQ(saved, 2);
    EXPECT_EQ(server.progressSaves.size(), 2u);
    EXPECT_TRUE(server.progressSaves[1].progress.finished);
}

TEST(FakeMusicServer, ProgressMissingFailedCancelledAndNotConfiguredMatchTheContract) {
    FakeMusicServer server;
    int calls = 0;
    auto callback = [&](Error error, std::optional<oap::RemoteProgress> value) {
        EXPECT_EQ(error, calls == 0 ? Error::None : calls == 1 ? Error::Server : Error::NotConfigured);
        EXPECT_FALSE(value);
        ++calls;
    };
    auto id = server.progress("new", callback);
    EXPECT_TRUE(server.replyProgress(id, std::nullopt));
    server.poll();
    id = server.progress("bad", callback);
    EXPECT_TRUE(server.fail(id, Error::Server));
    server.poll();
    id = server.progress("cancel", callback);
    EXPECT_TRUE(server.replyProgress(id, oap::RemoteProgress{}));
    server.cancel(id);
    server.poll();
    EXPECT_EQ(calls, 2);
    server.isConfigured = false;
    server.progress("a", callback);
    int saved = 0;
    server.saveProgress("a", {}, [&](Error error) { ++saved; EXPECT_EQ(error, Error::NotConfigured); });
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(saved, 0);
    server.poll();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(saved, 1);
    EXPECT_TRUE(server.progressSaves.empty());
}
