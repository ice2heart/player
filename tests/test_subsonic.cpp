// Subsonic over a fake HTTP client: the URLs it asks for (auth, escaping), how
// answers become results and errors, and the MusicServer contract (callbacks only
// from poll(), never after cancel(), ids unique, NotConfigured without a server).

#include "core/settings.h"
#include "net/md5.h"
#include "net/subsonic.h"
#include "subsonic_fixtures.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

using oap::Http;
using oap::RemoteAlbum;
using oap::RemoteArtist;
using oap::RemoteFolder;
using oap::RemotePlaylist;
using oap::RemoteTrack;
using oap::Subsonic;
using oap::test::subsonicFixture;
using Error = oap::MusicServer::Error;
using ServerInfo = oap::MusicServer::ServerInfo;

namespace {

// Records each GET. The test plays the HTTP thread: answer() runs the handler at once.
class FakeHttp : public Http {
public:
    struct Call {
        Id id = 0;
        std::string url;
        Headers headers;
        Handler handler;
    };
    void get(Id id, std::string url, Headers headers, Handler handler) override {
        calls.push_back({id, std::move(url), std::move(headers), std::move(handler)});
    }
    void cancel(Id id) override { cancelled.push_back(id); }

    void answer(std::size_t call, long status, std::string body) {
        Http::Response response;
        response.reached = true;
        response.status = status;
        response.body = std::move(body);
        calls.at(call).handler(std::move(response));
    }
    void unreachable(std::size_t call) { calls.at(call).handler(Http::Response{}); }
    const std::string& url(std::size_t call) const { return calls.at(call).url; }

    std::vector<Call> calls;
    std::vector<Id> cancelled;
};

constexpr long kOk = 200;

Subsonic::Config config(std::string url = "https://music.example/") {
    Subsonic::Config c;
    c.url = std::move(url);
    c.user = "ice";
    c.password = "sesame";
    return c;
}

struct Server {
    explicit Server(Subsonic::Config c = config(), Subsonic::SaltSource salt = [] { return std::string("c19b2d"); }) {
        auto owned = std::make_unique<FakeHttp>();
        http = owned.get();
        subsonic = std::make_unique<Subsonic>(std::move(c), std::move(owned), std::move(salt));
    }
    FakeHttp* http = nullptr;
    std::unique_ptr<Subsonic> subsonic;
};

template <class T> struct ListResult {
    int calls = 0;
    Error error = Error::None;
    std::vector<T> items;
    bool more = false;
    oap::MusicServer::Done<T> callback() {
        return [this](Error e, std::vector<T> list, bool m) {
            ++calls, error = e, items = std::move(list), more = m;
        };
    }
};

struct PingResult {
    int calls = 0;
    Error error = Error::None;
    ServerInfo info;
    std::function<void(Error, ServerInfo)> callback() {
        return [this](Error e, ServerInfo i) { ++calls, error = e, info = std::move(i); };
    }
};

// The value of query parameter `key` in `url`, or "<missing>".
std::string param(const std::string& url, const std::string& key) {
    auto query = url.find('?');
    if (query == std::string::npos) return "<missing>";
    std::string needle = key + "=";
    for (std::size_t pos = query + 1; pos < url.size();) {
        auto end = url.find('&', pos);
        if (end == std::string::npos) end = url.size();
        if (url.compare(pos, needle.size(), needle) == 0) return url.substr(pos + needle.size(), end - pos - needle.size());
        pos = end + 1;
    }
    return "<missing>";
}

const std::string kBase = "https://music.example/rest/";
const std::string kAuth = "?u=ice&t=26719a1196d2a940705a59634eb18eab&s=c19b2d&v=1.16.1&c=oap&f=json";

} // namespace

TEST(Subsonic, UrlCarriesTokenAndSaltNotThePassword) {
    Server server;
    server.subsonic->ping([](Error, ServerInfo) {});
    ASSERT_EQ(server.http->calls.size(), 1u);
    const auto& url = server.http->url(0);
    // t = md5("sesame" + "c19b2d"), the example in the Subsonic docs.
    EXPECT_EQ(url, kBase + "ping" + kAuth);
    EXPECT_EQ(param(url, "p"), "<missing>");
    EXPECT_EQ(url.find("sesame"), std::string::npos);
}

TEST(Subsonic, EachRequestHasAFreshSalt) {
    Server server(config(), Subsonic::randomSalt);
    std::set<std::string> salts;
    for (int i = 0; i < 3; ++i) server.subsonic->ping([](Error, ServerInfo) {});
    std::vector<std::string> urls = {server.http->url(0), server.http->url(1), server.http->url(2),
                                     server.subsonic->stream("s1", 0).url};
    for (const auto& url : urls) {
        std::string salt = param(url, "s");
        EXPECT_GE(salt.size(), 6u);
        EXPECT_EQ(param(url, "t"), oap::md5Hex("sesame" + salt));
        salts.insert(salt);
    }
    EXPECT_EQ(salts.size(), urls.size());
}

TEST(Subsonic, ParametersAreEscaped) {
    auto c = config("http://host:4533/navidrome");
    c.user = "a b&c=d";
    c.password = "p&ss=wörd";
    Server server(c);
    server.subsonic->albumTracks("x/y?z#ü", ListResult<RemoteTrack>().callback());
    const auto& url = server.http->url(0);
    EXPECT_EQ(url.rfind("http://host:4533/navidrome/rest/getAlbum?", 0), 0u) << url;
    EXPECT_EQ(param(url, "u"), "a%20b%26c%3Dd");
    EXPECT_EQ(param(url, "id"), "x%2Fy%3Fz%23%C3%BC");
    EXPECT_EQ(param(url, "t"), oap::md5Hex("p&ss=wördc19b2d"));
    EXPECT_EQ(url.find("ss=w"), std::string::npos); // nothing of the password
}

TEST(Subsonic, EachRequestAsksForItsMethod) {
    Server server;
    auto& s = *server.subsonic;
    ListResult<RemoteAlbum> albums;
    ListResult<RemoteArtist> artists;
    ListResult<RemoteTrack> tracks;
    ListResult<RemotePlaylist> playlists;
    s.albums("alphabeticalByName", 1000, 500, albums.callback());
    s.artists(artists.callback());
    s.artistAlbums("ar1", albums.callback());
    s.albumTracks("al1", tracks.callback());
    s.playlists(playlists.callback());
    s.playlistTracks("p1", tracks.callback());
    ASSERT_EQ(server.http->calls.size(), 6u);
    EXPECT_EQ(server.http->url(0), kBase + "getAlbumList2" + kAuth + "&type=alphabeticalByName&size=500&offset=1000");
    EXPECT_EQ(server.http->url(1), kBase + "getArtists" + kAuth);
    EXPECT_EQ(server.http->url(2), kBase + "getArtist" + kAuth + "&id=ar1");
    EXPECT_EQ(server.http->url(3), kBase + "getAlbum" + kAuth + "&id=al1");
    EXPECT_EQ(server.http->url(4), kBase + "getPlaylists" + kAuth);
    EXPECT_EQ(server.http->url(5), kBase + "getPlaylist" + kAuth + "&id=p1");
}

TEST(Subsonic, TheAlbumListAsksForOneMusicFolderWhenGivenAndForAllOtherwise) {
    Server server;
    ListResult<RemoteAlbum> result;
    server.subsonic->albums("alphabeticalByName", 0, 500, result.callback(), "2");
    server.subsonic->albums("alphabeticalByName", 0, 500, result.callback());
    EXPECT_EQ(server.http->url(0),
              kBase + "getAlbumList2" + kAuth + "&type=alphabeticalByName&size=500&offset=0&musicFolderId=2");
    EXPECT_EQ(server.http->url(1).find("musicFolderId"), std::string::npos);
}

TEST(Subsonic, MusicFoldersAreParsedWithNumberAndTextIds) {
    Server server;
    ListResult<RemoteFolder> result;
    server.subsonic->musicFolders(result.callback());
    EXPECT_EQ(server.http->url(0), kBase + "getMusicFolders" + kAuth);
    server.http->answer(0, kOk, subsonicFixture("music_folders.json"));
    server.subsonic->poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Error::None);
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].id, "1");
    EXPECT_EQ(result.items[0].name, "Music");
    EXPECT_EQ(result.items[1].id, "2");
    EXPECT_EQ(result.items[1].name, "Аудиокниги");
    EXPECT_FALSE(result.more);
}

TEST(Subsonic, APageLargerThanTheServerAllowsIsCappedSoMoreStillWorks) {
    Server server;
    ListResult<RemoteAlbum> result;
    server.subsonic->albums("newest", -5, 2000, result.callback());
    EXPECT_EQ(param(server.http->url(0), "size"), "500");
    EXPECT_EQ(param(server.http->url(0), "offset"), "0");
    server.http->answer(0, kOk, subsonicFixture("album_list_500_one_line.json")); // the server's full page
    server.subsonic->poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.items.size(), 500u);
    EXPECT_TRUE(result.more);
}

TEST(Subsonic, DownloadUrlIsTheDownloadMethodWithTheSameAuthAsAStream) {
    auto urls = Subsonic::downloadUrls(config(), [] { return std::string("c19b2d"); });
    EXPECT_EQ(urls("s1"), kBase + "download" + kAuth + "&id=s1");
    EXPECT_EQ(urls("a b&c"), kBase + "download" + kAuth + "&id=a%20b%26c") << "ids are escaped";
    EXPECT_EQ(urls("s1").find("sesame"), std::string::npos);
}

TEST(Subsonic, DownloadUrlsHaveAFreshSaltAndTokenEach) {
    auto urls = Subsonic::downloadUrls(config());
    std::string first = urls("s1"), second = urls("s1");
    EXPECT_NE(param(first, "s"), param(second, "s"));
    EXPECT_EQ(param(second, "t"), oap::md5Hex("sesame" + param(second, "s")));
}

TEST(Subsonic, DownloadUrlIsEmptyWithoutAServerUrl) {
    EXPECT_EQ(Subsonic::downloadUrls(config(""))("s1"), "");
}

TEST(Subsonic, TheOriginalFileHasNeitherFormatNorMaxBitRate) {
    Server server;
    auto stream = server.subsonic->stream("s1", 0);
    EXPECT_EQ(stream.url, kBase + "stream" + kAuth + "&id=s1");
    EXPECT_FALSE(stream.transcoded) << "the original file seeks with Range";
    EXPECT_TRUE(server.http->calls.empty()) << "nothing to ask the server for it";
}

TEST(Subsonic, ATranscodeCarriesFormatAndMaxBitRate) {
    Server server;
    auto stream = server.subsonic->stream("s1", 192);
    EXPECT_EQ(stream.url, kBase + "stream" + kAuth + "&id=s1&format=mp3&maxBitRate=192");
    ASSERT_TRUE(stream.transcoded);
    EXPECT_EQ(stream.transcoded->offsetUrl, "") << "not known yet whether the server can start it later";
    EXPECT_EQ(stream.transcoded->duration, 0) << "App knows the length, not the server";
    ASSERT_EQ(server.http->calls.size(), 1u);
    EXPECT_EQ(server.http->url(0), kBase + "getOpenSubsonicExtensions" + kAuth);
    server.subsonic->stream("s2", 128);
    EXPECT_EQ(server.http->calls.size(), 1u) << "asked once while the answer is out";
}

// Navidrome has transcodeOffset: a transcode then starts later with timeOffset (seconds).
TEST(Subsonic, ATranscodeSeeksWithTimeOffsetWhenTheServerHasTranscodeOffset) {
    Server server;
    server.subsonic->stream("s1", 128);
    server.http->answer(0, kOk, subsonicFixture("extensions.json"));
    server.subsonic->poll();
    auto stream = server.subsonic->stream("s1", 128);
    ASSERT_TRUE(stream.transcoded);
    EXPECT_EQ(stream.transcoded->offsetUrl, stream.url + "&timeOffset=");
    EXPECT_EQ(server.subsonic->stream("s1", 0).transcoded, std::nullopt) << "the original file still seeks with Range";
    EXPECT_EQ(server.http->calls.size(), 1u) << "asked once";
}

// A server without the extension has no offset: the player opens the
// transcode from its start to seek.
TEST(Subsonic, WithoutTranscodeOffsetATranscodeHasNoOffsetUrl) {
    Server server;
    server.subsonic->stream("s1", 128);
    server.http->answer(0, kOk, subsonicFixture("extensions_no_offset.json"));
    server.subsonic->poll();
    auto stream = server.subsonic->stream("s1", 128);
    ASSERT_TRUE(stream.transcoded);
    EXPECT_EQ(stream.transcoded->offsetUrl, "");
    EXPECT_EQ(server.http->calls.size(), 1u) << "an answer is an answer: not asked again";
}

// A server that is not OpenSubsonic answers with an error each time; a proxy
// may fail once. After three errors it has no extensions.
TEST(Subsonic, AServerThatAnswersWithErrorsHasNoTranscodeOffset) {
    struct Answer {
        long status;
        std::string body;
    };
    for (const Answer& answer : {Answer{kOk, subsonicFixture("error_70_not_found.json")},
                                 Answer{404, subsonicFixture("not_json.html")}}) {
        Server server;
        server.subsonic->stream("s1", 128);
        for (std::size_t call = 0; call < 3; ++call) {
            ASSERT_EQ(server.http->calls.size(), call + 1) << "asked again after an error " << answer.body;
            server.http->answer(call, answer.status, answer.body);
            server.subsonic->poll();
            EXPECT_EQ(server.subsonic->stream("s1", 128).transcoded->offsetUrl, "");
        }
        server.subsonic->stream("s1", 128);
        EXPECT_EQ(server.http->calls.size(), 3u) << "not asked a fourth time " << answer.body;
    }
}

TEST(Subsonic, ExtensionsAreAskedForAgainWhenNoAnswerCame) {
    Server server;
    server.subsonic->stream("s1", 128);
    server.http->unreachable(0);
    server.subsonic->poll();
    EXPECT_EQ(server.subsonic->stream("s1", 128).transcoded->offsetUrl, "");
    ASSERT_EQ(server.http->calls.size(), 2u) << "the next transcode asks again";
    server.http->answer(1, kOk, subsonicFixture("extensions.json"));
    server.subsonic->poll();
    EXPECT_NE(server.subsonic->stream("s1", 128).transcoded->offsetUrl, "");
}

TEST(Subsonic, PingReturnsServerTypeAndVersionFromPoll) {
    Server server;
    PingResult result;
    server.subsonic->ping(result.callback());
    server.http->answer(0, kOk, subsonicFixture("ping.json"));
    EXPECT_EQ(result.calls, 0); // answered on the HTTP thread; the callback waits for poll()
    server.subsonic->poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.info.type, "navidrome");
    EXPECT_EQ(result.info.version, "0.64.2");
    server.subsonic->poll();
    EXPECT_EQ(result.calls, 1); // only once
}

TEST(Subsonic, RequestsCarryNoExtraHeaders) {
    Server server;
    PingResult result;
    server.subsonic->ping(result.callback());
    ASSERT_EQ(server.http->calls.size(), 1u);
    EXPECT_TRUE(server.http->calls[0].headers.empty());
}

TEST(Subsonic, CatalogueVersionAsksGetArtistsAndGivesLastModified) {
    Server server;
    int calls = 0;
    Error error = Error::Server;
    std::string version;
    server.subsonic->catalogueVersion([&](Error e, std::string v) { ++calls, error = e, version = std::move(v); });
    ASSERT_EQ(server.http->calls.size(), 1u);
    EXPECT_EQ(server.http->url(0), kBase + "getArtists" + kAuth);
    server.http->answer(0, kOk, subsonicFixture("artists.json"));
    EXPECT_EQ(calls, 0);
    server.subsonic->poll();
    ASSERT_EQ(calls, 1);
    EXPECT_EQ(error, Error::None);
    EXPECT_EQ(version, "1729791000000");
}

TEST(Subsonic, CatalogueVersionErrorsHaveNoVersion) {
    Server server;
    Error error = Error::None;
    std::string version = "stale";
    auto done = [&](Error e, std::string v) { error = e, version = std::move(v); };
    server.subsonic->catalogueVersion(done);
    server.http->unreachable(0);
    server.subsonic->poll();
    EXPECT_EQ(error, Error::Unreachable);
    EXPECT_EQ(version, "");
    server.subsonic->catalogueVersion(done);
    server.http->answer(1, kOk, subsonicFixture("error_40_wrong_login.json"));
    server.subsonic->poll();
    EXPECT_EQ(error, Error::BadLogin);
}

TEST(Subsonic, CatalogueVersionOfAServerNotConfiguredIsNotConfigured) {
    Server server(config(""));
    Error error = Error::None;
    server.subsonic->catalogueVersion([&](Error e, std::string) { error = e; });
    server.subsonic->poll();
    EXPECT_EQ(error, Error::NotConfigured);
}

TEST(Subsonic, AlbumWithNonAsciiAndEmojiTitles) {
    Server server;
    ListResult<RemoteAlbum> result;
    server.subsonic->albums("newest", 0, 3, result.callback());
    server.http->answer(0, kOk, subsonicFixture("album_list_unicode.json"));
    server.subsonic->poll();
    ASSERT_EQ(result.items.size(), 3u);
    EXPECT_EQ(result.items[1].name, "Café del Mar 🎵");
    EXPECT_TRUE(result.more); // 3 of 3: there may be another page
}

TEST(Subsonic, WrongPasswordIsBadLogin) {
    Server server;
    PingResult ping;
    ListResult<RemoteArtist> artists;
    server.subsonic->ping(ping.callback());
    server.subsonic->artists(artists.callback());
    server.http->answer(0, kOk, subsonicFixture("error_40_wrong_login.json"));
    server.http->answer(1, kOk, subsonicFixture("error_40_wrong_login.json"));
    server.subsonic->poll();
    EXPECT_EQ(ping.error, Error::BadLogin);
    EXPECT_EQ(artists.error, Error::BadLogin);
    EXPECT_TRUE(artists.items.empty());
    EXPECT_FALSE(artists.more);
}

TEST(Subsonic, NoAnswerIsUnreachable) {
    Server server;
    ListResult<RemoteAlbum> result;
    server.subsonic->albums("newest", 0, 3, result.callback());
    server.http->unreachable(0);
    server.subsonic->poll();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Error::Unreachable);
    EXPECT_TRUE(result.items.empty());
    EXPECT_FALSE(result.more);
}

TEST(Subsonic, AnHttpErrorStatusIsAServerError) {
    Server server;
    ListResult<RemotePlaylist> gateway, notFound;
    server.subsonic->playlists(gateway.callback());
    server.subsonic->playlists(notFound.callback());
    server.http->answer(0, 502, subsonicFixture("not_json.html"));
    server.http->answer(1, 404, subsonicFixture("playlists.json")); // a body that reads fine does not count
    server.subsonic->poll();
    EXPECT_EQ(gateway.error, Error::Server);
    EXPECT_EQ(notFound.error, Error::Server);
    EXPECT_TRUE(notFound.items.empty());
}

TEST(Subsonic, CancelledRequestCallbackDoesNotRun) {
    Server server;
    ListResult<RemoteTrack> before, after, kept;
    auto beforeId = server.subsonic->albumTracks("al1", before.callback());
    auto afterId = server.subsonic->albumTracks("al1", after.callback());
    server.subsonic->albumTracks("al1", kept.callback());

    server.subsonic->cancel(beforeId); // before the answer came
    server.http->answer(0, kOk, subsonicFixture("album.json"));
    server.http->answer(1, kOk, subsonicFixture("album.json"));
    server.http->answer(2, kOk, subsonicFixture("album.json"));
    server.subsonic->cancel(afterId); // answered, but not polled yet
    server.subsonic->poll();

    EXPECT_EQ(before.calls, 0);
    EXPECT_EQ(after.calls, 0);
    EXPECT_EQ(kept.calls, 1);
    EXPECT_EQ(server.http->cancelled, (std::vector<Http::Id>{beforeId, afterId})); // the transfer is stopped too
}

TEST(Subsonic, CancelFromInsideAnotherCallback) {
    Server server;
    ListResult<RemoteTrack> second;
    oap::MusicServer::RequestId secondId = 0;
    int firstCalls = 0;
    server.subsonic->albumTracks("al1", [&](Error, std::vector<RemoteTrack>, bool) {
        ++firstCalls;
        server.subsonic->cancel(secondId);
    });
    secondId = server.subsonic->albumTracks("al1", second.callback());
    server.http->answer(0, kOk, subsonicFixture("album.json"));
    server.http->answer(1, kOk, subsonicFixture("album.json"));
    server.subsonic->poll(); // both are ready in the same poll
    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(second.calls, 0);
}

TEST(Subsonic, NotConfiguredFinishesOnPollWithoutNetwork) {
    Server server(config(""));
    EXPECT_FALSE(server.subsonic->configured());
    PingResult ping;
    ListResult<RemoteAlbum> albums;
    server.subsonic->ping(ping.callback());
    server.subsonic->albums("newest", 0, 10, albums.callback());
    EXPECT_TRUE(server.http->calls.empty());
    EXPECT_EQ(ping.calls, 0);
    server.subsonic->poll();
    EXPECT_EQ(ping.error, Error::NotConfigured);
    EXPECT_EQ(albums.calls, 1);
    EXPECT_EQ(albums.error, Error::NotConfigured);
    EXPECT_EQ(server.subsonic->stream("s1", 0).url, "");
    EXPECT_EQ(server.subsonic->stream("s1", 128).url, "");
    EXPECT_TRUE(server.http->calls.empty()) << "no extensions asked for";
}

TEST(Subsonic, IdsAreUniqueAndNeverZero) {
    Server server;
    std::set<oap::MusicServer::RequestId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.insert(server.subsonic->ping([](Error, ServerInfo) {}));
        ids.insert(server.subsonic->artists([](Error, std::vector<RemoteArtist>, bool) {}));
    }
    EXPECT_EQ(ids.size(), 10u);
    EXPECT_EQ(ids.count(0), 0u);
}

TEST(Subsonic, ConfigComesFromAServerEntry) {
    oap::ServerConfig entry;
    entry.url = "https://music.example";
    entry.user = "ice";
    entry.password = "sesame";
    entry.apiKey = "not-for-subsonic";
    Server server(Subsonic::configFrom(entry));
    EXPECT_TRUE(server.subsonic->configured());
    EXPECT_EQ(server.subsonic->stream("s1", 0).url, kBase + "stream" + kAuth + "&id=s1");
    EXPECT_FALSE(Server(Subsonic::configFrom(oap::ServerConfig())).subsonic->configured());
}

TEST(Subsonic, ScrobbleSaysNowPlayingOrPlayed) {
    Server server;
    server.subsonic->scrobble("s1", false, std::nullopt, {});
    server.subsonic->scrobble("s1", true, std::nullopt, {});
    ASSERT_EQ(server.http->calls.size(), 2u);
    EXPECT_EQ(server.http->url(0), kBase + "scrobble" + kAuth + "&id=s1&submission=false");
    EXPECT_EQ(server.http->url(1), kBase + "scrobble" + kAuth + "&id=s1&submission=true");
}

TEST(Subsonic, ALateSubmissionCarriesTheTimeItWasHeard) {
    Server server;
    server.subsonic->scrobble("s1", true, 1700000123456, {});
    ASSERT_EQ(server.http->calls.size(), 1u);
    EXPECT_EQ(server.http->url(0), kBase + "scrobble" + kAuth + "&id=s1&submission=true&time=1700000123456");
}

TEST(Subsonic, AScrobbleDoneGetsTheErrorOfTheAnswer) {
    Server server;
    std::vector<oap::MusicServer::Error> errors;
    auto done = [&](oap::MusicServer::Error e) { errors.push_back(e); };
    for (int i = 0; i < 3; ++i) server.subsonic->scrobble("s1", true, 5, done);
    server.http->answer(0, kOk, subsonicFixture("ping.json"));
    server.http->unreachable(1);
    server.http->answer(2, kOk, subsonicFixture("error_40_wrong_login.json"));
    testing::internal::CaptureStderr();
    server.subsonic->poll();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(errors, (std::vector<oap::MusicServer::Error>{oap::MusicServer::Error::None, oap::MusicServer::Error::Unreachable,
                                                            oap::MusicServer::Error::BadLogin}));
}

// finish() is the last call before exit: the scrobble the server took is reported to its callback (so the
// outbox drops it), but no other callback runs, and nothing is delivered twice.
TEST(Subsonic, FinishAnswersTheScrobblesOnly) {
    Server server;
    std::vector<oap::MusicServer::Error> errors;
    int pings = 0;
    server.subsonic->scrobble("s1", true, 5, [&](oap::MusicServer::Error e) { errors.push_back(e); });
    server.subsonic->ping([&](Error, ServerInfo) { ++pings; });
    server.http->answer(0, kOk, subsonicFixture("ping.json"));
    server.http->answer(1, kOk, subsonicFixture("ping.json"));
    server.subsonic->finish(std::chrono::milliseconds(0));
    EXPECT_EQ(errors, (std::vector<oap::MusicServer::Error>{Error::None}));
    EXPECT_EQ(pings, 0);
    server.subsonic->poll();
    EXPECT_EQ(errors.size(), 1u);
}

// ---- createPlaylist (queue.md, step 2.9)

struct CreateResult {
    int calls = 0;
    Error error = Error::None;
    std::string playlistId;
    std::function<void(Error, std::string)> callback() {
        return [this](Error e, std::string id) { ++calls, error = e, playlistId = std::move(id); };
    }
};

TEST(Subsonic, CreatePlaylistSendsTheNameAndOneSongIdPerTrackWithTokenAuthAndNoPassword) {
    Server server;
    CreateResult result;
    server.subsonic->createPlaylist("Queue, 27 October 14:05", {"t1", "t3", "t2"}, result.callback());
    ASSERT_EQ(server.http->calls.size(), 1u);
    EXPECT_EQ(server.http->url(0), kBase + "createPlaylist" + kAuth +
                                       "&name=Queue%2C%2027%20October%2014%3A05&songId=t1&songId=t3&songId=t2");
    EXPECT_EQ(param(server.http->url(0), "p"), "<missing>") << "never the password";
    EXPECT_EQ(result.calls, 0) << "the callback runs from poll()";
    server.http->answer(0, kOk, subsonicFixture("ping.json"));
    server.subsonic->poll();
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.playlistId, "") << "an answer without the playlist";
}

TEST(Subsonic, CreatePlaylistGivesTheIdOfTheNewPlaylist) {
    Server server;
    CreateResult result;
    server.subsonic->createPlaylist("q", {"t1"}, result.callback());
    server.http->answer(0, kOk, subsonicFixture("playlist_created.json"));
    server.subsonic->poll();
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.playlistId, "pl-77");
}

TEST(Subsonic, CreatePlaylistEncodesIdsThatHoldSpecialCharacters) {
    Server server;
    CreateResult result;
    server.subsonic->createPlaylist("a&b", {"x&y", "z=1"}, result.callback());
    EXPECT_EQ(param(server.http->url(0), "name"), "a%26b");
    EXPECT_NE(server.http->url(0).find("&songId=x%26y&songId=z%3D1"), std::string::npos);
}

TEST(Subsonic, CreatePlaylistMapsFailuresToErrors) {
    Server server;
    CreateResult unreachable, login, broken;
    server.subsonic->createPlaylist("q", {"t1"}, unreachable.callback());
    server.subsonic->createPlaylist("q", {"t1"}, login.callback());
    server.subsonic->createPlaylist("q", {"t1"}, broken.callback());
    server.http->unreachable(0);
    server.http->answer(1, kOk, subsonicFixture("error_40_wrong_login.json"));
    server.http->answer(2, 500, subsonicFixture("not_json.html"));
    server.subsonic->poll();
    EXPECT_EQ(unreachable.error, Error::Unreachable);
    EXPECT_EQ(login.error, Error::BadLogin);
    EXPECT_EQ(broken.error, Error::Server);
    EXPECT_EQ(unreachable.calls + login.calls + broken.calls, 3);
}

TEST(Subsonic, CreatePlaylistLogsNothingNotEvenAFailure) {
    Server server;
    CreateResult result;
    server.subsonic->createPlaylist("q", {"t1"}, result.callback());
    server.http->unreachable(0);
    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    server.subsonic->poll();
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "") << "the URL holds the token";
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
}

TEST(Subsonic, CreatePlaylistWithoutAServerIsNotConfiguredAndSendsNothing) {
    Server server(config(""));
    CreateResult result;
    server.subsonic->createPlaylist("q", {"t1"}, result.callback());
    server.subsonic->poll();
    EXPECT_EQ(result.error, Error::NotConfigured);
    EXPECT_EQ(result.calls, 1);
    EXPECT_TRUE(server.http->calls.empty());
}

TEST(Subsonic, CreatePlaylistWithTooManyTracksSendsNothingAndFails) {
    Server server;
    CreateResult result;
    std::vector<std::string> ids(Subsonic::kMaxPlaylistTracks + 1, "t1");
    server.subsonic->createPlaylist("q", ids, result.callback());
    EXPECT_TRUE(server.http->calls.empty()) << "the URL would be too long";
    server.subsonic->poll();
    EXPECT_EQ(result.error, Error::Server);
    EXPECT_EQ(result.calls, 1);
    CreateResult exact;
    ids.pop_back();
    server.subsonic->createPlaylist("q", ids, exact.callback());
    EXPECT_EQ(server.http->calls.size(), 1u) << "exactly the limit is sent";
}

TEST(Subsonic, CreatePlaylistWithTooManyTracksButNoServerIsNotConfigured) {
    Server server(config(""));
    CreateResult result;
    server.subsonic->createPlaylist("q", std::vector<std::string>(Subsonic::kMaxPlaylistTracks + 1, "t1"), result.callback());
    server.subsonic->poll();
    EXPECT_EQ(result.error, Error::NotConfigured);
}

TEST(Subsonic, ACancelledCreatePlaylistNeverCallsBack) {
    Server server;
    CreateResult result;
    auto id = server.subsonic->createPlaylist("q", {"t1"}, result.callback());
    server.subsonic->cancel(id);
    server.http->answer(0, kOk, subsonicFixture("ping.json"));
    server.subsonic->poll();
    EXPECT_EQ(result.calls, 0);
    EXPECT_EQ(server.http->cancelled, std::vector<oap::Http::Id>{id});
}

TEST(Subsonic, NoScrobbleWithoutAServer) {
    Server server(config(""));
    server.subsonic->scrobble("s1", true, std::nullopt, {});
    testing::internal::CaptureStderr();
    server.subsonic->poll();
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "") << "nothing failed: nothing was sent";
    EXPECT_TRUE(server.http->calls.empty());
}

// Nothing comes back from a scrobble: a failure is only logged, by its kind,
// and never with the URL, which holds the login.
TEST(Subsonic, AFailedScrobbleIsLoggedWithoutTheUrl) {
    Server server;
    for (int i = 0; i < 4; ++i) server.subsonic->scrobble("s1", true, std::nullopt, {});
    server.http->unreachable(0);
    server.http->answer(1, kOk, subsonicFixture("error_40_wrong_login.json"));
    server.http->answer(2, 500, subsonicFixture("not_json.html"));
    server.http->answer(3, kOk, subsonicFixture("ping.json")); // this one worked
    testing::internal::CaptureStderr();
    server.subsonic->poll();
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err, "server: scrobble failed: server not reachable\n"
                   "server: scrobble failed: wrong user name or password\n"
                   "server: scrobble failed: server error\n");
    for (const char* secret : {"music.example", "ice", "26719a1196d2a940705a59634eb18eab", "c19b2d", "s1"})
        EXPECT_EQ(err.find(secret), std::string::npos) << secret;
}
