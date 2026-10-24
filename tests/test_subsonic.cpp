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
        Handler handler;
    };
    void get(Id id, std::string url, Handler handler) override { calls.push_back({id, std::move(url), std::move(handler)}); }
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
                                     server.subsonic->streamUrl("s1")};
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

TEST(Subsonic, StreamUrlHasTheTokenAndTheChosenFormat) {
    Server original; // no format: the file as it is
    EXPECT_EQ(original.subsonic->streamUrl("s1"), kBase + "stream" + kAuth + "&id=s1");
    auto c = config();
    c.format = "raw"; // the same, said in config.yaml
    Server raw(c);
    EXPECT_EQ(raw.subsonic->streamUrl("s1"), kBase + "stream" + kAuth + "&id=s1");

    c.format = "mp3";
    c.maxBitrate = 192;
    Server transcoded(c);
    EXPECT_EQ(transcoded.subsonic->streamUrl("s1"), kBase + "stream" + kAuth + "&id=s1&format=mp3&maxBitRate=192");
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
    EXPECT_EQ(server.subsonic->streamUrl("s1"), "");
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

TEST(Subsonic, ConfigComesFromTheServerSettings) {
    oap::Settings settings;
    settings.setString("server.url", "https://music.example");
    settings.setString("server.user", "ice");
    settings.setString("server.password", "sesame");
    settings.setString("server.format", "opus");
    settings.setString("server.max_bitrate", "128");
    Server server(Subsonic::configFrom(settings));
    EXPECT_TRUE(server.subsonic->configured());
    EXPECT_EQ(server.subsonic->streamUrl("s1"), kBase + "stream" + kAuth + "&id=s1&format=opus&maxBitRate=128");
    EXPECT_FALSE(Server(Subsonic::configFrom(oap::Settings())).subsonic->configured());
}

TEST(Subsonic, IsMadeFromConfigOnlyForTypeSubsonicWithAUrl) {
    oap::Settings settings;
    settings.setString("server.url", "https://music.example");
    EXPECT_NE(Subsonic::fromConfig(settings), nullptr) << "no type: subsonic";
    settings.setString("server.type", "subsonic");
    auto server = Subsonic::fromConfig(settings);
    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->configured());
    settings.setString("server.type", "Subsonic");
    EXPECT_NE(Subsonic::fromConfig(settings), nullptr) << "a type is not case sensitive";
    settings.setString("server.type", "audiobookshelf");
    EXPECT_EQ(Subsonic::fromConfig(settings), nullptr) << "another type of server";
    EXPECT_EQ(Subsonic::fromConfig(oap::Settings()), nullptr) << "no server section";
}
