// Audiobookshelf over a fake HTTP client: the URLs and headers it sends, how answers become
// results and errors, and the MusicServer contract (callbacks only from poll(), never after
// cancel(), NotConfigured without a server). The answers are saved ones from tests/data/audiobookshelf/.

#include "audiobookshelf_fixtures.h"
#include "core/settings.h"
#include "net/audiobookshelf.h"
#include "subsonic_fixtures.h" // PrintTo for Error

#include <gtest/gtest.h>
#include <limits>
#include <thread>

#include <memory>
#include <string>
#include <vector>

using oap::Audiobookshelf;
using oap::Http;
using oap::RemoteAlbum;
using oap::RemoteArtist;
using oap::RemoteBook;
using oap::RemoteFolder;
using oap::RemotePlaylist;
using oap::RemoteTrack;
using oap::test::audiobookshelfFixture;
using Error = oap::MusicServer::Error;
using ServerInfo = oap::MusicServer::ServerInfo;

namespace {

// A made-up key: the real one never appears in a test.
const std::string kKey = "test-key-0123456789";
const std::string kBase = "https://books.example";
const std::string kBooks = "585d09d2-286c-4dba-be50-e1f75681dee9";
const std::string kNeuromancer = "171aa11a-a575-44be-b21e-8c01199a27d9";
constexpr long kOk = 200;

// Records each GET. The test plays the HTTP thread: answer() runs the handler at once.
class FakeHttp : public Http {
public:
    struct Call {
        Id id = 0;
        std::string url;
        Headers headers;
        Handler handler;
        std::string method = "GET", body;
    };
    void get(Id id, std::string url, Headers headers, Handler handler) override {
        calls.push_back({id, std::move(url), std::move(headers), std::move(handler)});
    }
    void patch(Id id, std::string url, Headers headers, std::string body, Handler handler) override {
        calls.push_back({id, std::move(url), std::move(headers), std::move(handler), "PATCH", std::move(body)});
    }
    void cancel(Id id) override { cancelled.push_back(id); }
    void finish(std::chrono::milliseconds deadline) override { finishDeadline = deadline; ++finishes; }
    int finishes = 0;
    std::chrono::milliseconds finishDeadline{};

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

Audiobookshelf::Config config(std::string library = {}) {
    Audiobookshelf::Config c;
    c.url = kBase + "/";
    c.user = "ice";
    c.apiKey = kKey;
    c.library = std::move(library);
    return c;
}

struct Server {
    explicit Server(Audiobookshelf::Config c = config()) {
        auto owned = std::make_unique<FakeHttp>();
        http = owned.get();
        abs = std::make_unique<Audiobookshelf>(std::move(c), std::move(owned));
    }
    FakeHttp* http = nullptr;
    std::unique_ptr<Audiobookshelf> abs;
};

template <class T> struct ListResult {
    int calls = 0;
    Error error = Error::None;
    std::vector<T> items;
    bool more = false;
    oap::MusicServer::Done<T> callback() {
        return [this](Error e, std::vector<T> list, bool m) { ++calls, error = e, items = std::move(list), more = m; };
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

struct BookResult {
    int calls = 0;
    Error error = Error::None;
    RemoteBook book;
    std::function<void(Error, RemoteBook)> callback() {
        return [this](Error e, RemoteBook b) { ++calls, error = e, book = std::move(b); };
    }
};

// Answers the call that asks for the libraries, then the next one is the list itself.
void answerLibraries(Server& s, std::size_t call = 0) { s.http->answer(call, kOk, audiobookshelfFixture("libraries.json")); }

} // namespace

TEST(Audiobookshelf, EveryRequestCarriesTheBearerHeaderAndNoKeyInItsUrl) {
    Server s;
    s.abs->ping([](Error, ServerInfo) {});
    s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
    s.abs->poll(); // the second request of the ping
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 10, list.callback(), kBooks);
    s.abs->musicFolders(ListResult<RemoteFolder>().callback());
    s.abs->artists(ListResult<RemoteArtist>().callback());
    s.abs->artistAlbums("a1", list.callback());
    s.abs->book("b1", BookResult().callback());
    s.abs->albumTracks("b1", ListResult<RemoteTrack>().callback());
    ASSERT_GE(s.http->calls.size(), 7u);
    for (const auto& call : s.http->calls) {
        EXPECT_EQ(call.headers, Http::Headers{"Authorization: Bearer " + kKey}) << call.url;
        EXPECT_EQ(call.url.find(kKey), std::string::npos) << call.url;
        EXPECT_EQ(call.url.find("token"), std::string::npos) << call.url;
    }
}

TEST(Audiobookshelf, StreamUrlIsRightAndTheOnlyOneWithTheToken) {
    Server s;
    auto stream = s.abs->stream(kNeuromancer + "/13502477", 0);
    EXPECT_EQ(stream.url, kBase + "/api/items/" + kNeuromancer + "/file/13502477?token=" + kKey);
    EXPECT_FALSE(stream.transcoded.has_value());
    EXPECT_EQ(s.abs->stream(kNeuromancer + "/13502477", 128).url, stream.url); // no transcode, ever
    EXPECT_TRUE(s.http->calls.empty());                                         // building it sends nothing
}

TEST(Audiobookshelf, StreamOfAnIdWithoutAnInoOrOfAnUnconfiguredServerIsEmpty) {
    Server s;
    EXPECT_TRUE(s.abs->stream("no-slash", 0).url.empty());
    EXPECT_TRUE(s.abs->stream("/13502477", 0).url.empty());
    EXPECT_TRUE(s.abs->stream("item/", 0).url.empty());
    Audiobookshelf::Config noKey = config();
    noKey.apiKey.clear();
    Server unconfigured(noKey);
    EXPECT_TRUE(unconfigured.abs->stream("item/1", 0).url.empty());
}

TEST(Audiobookshelf, StreamUrlEscapesTheKeyAndTheIds) {
    Audiobookshelf::Config c = config();
    c.apiKey = "a b&c";
    Server s(c);
    EXPECT_EQ(s.abs->stream("it em/1&2", 0).url, kBase + "/api/items/it%20em/file/1%262?token=a%20b%26c");
}

TEST(Audiobookshelf, PingAsksStatusThenMeAndGivesTypeAndVersion) {
    Server s;
    PingResult ping;
    s.abs->ping(ping.callback());
    ASSERT_EQ(s.http->calls.size(), 1u);
    EXPECT_EQ(s.http->url(0), kBase + "/status");
    s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
    EXPECT_EQ(s.http->calls.size(), 1u); // the second request waits for poll()
    s.abs->poll();
    ASSERT_EQ(s.http->calls.size(), 2u);
    EXPECT_EQ(s.http->url(1), kBase + "/api/me");
    EXPECT_EQ(ping.calls, 0);
    s.http->answer(1, kOk, audiobookshelfFixture("me.json"));
    s.abs->poll();
    EXPECT_EQ(ping.calls, 1);
    EXPECT_EQ(ping.error, Error::None);
    EXPECT_EQ(ping.info.type, "Audiobookshelf");
    EXPECT_EQ(ping.info.version, "2.37.2");
}

TEST(Audiobookshelf, PingWithAWrongKeyIsBadLogin) {
    Server s;
    PingResult ping;
    s.abs->ping(ping.callback());
    s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
    s.abs->poll();
    s.http->answer(1, 401, "Unauthorized");
    s.abs->poll();
    EXPECT_EQ(ping.calls, 1);
    EXPECT_EQ(ping.error, Error::BadLogin);
    EXPECT_TRUE(ping.info.type.empty());
}

TEST(Audiobookshelf, PingsTwoStepsShareOneIdSoCancelStopsTheSecond) {
    Server s;
    PingResult ping;
    auto id = s.abs->ping(ping.callback());
    s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
    s.abs->poll();
    ASSERT_EQ(s.http->calls.size(), 2u);
    EXPECT_EQ(s.http->calls[1].id, id);
    s.abs->cancel(id);
    s.http->answer(1, kOk, audiobookshelfFixture("me.json"));
    s.abs->poll();
    EXPECT_EQ(ping.calls, 0);
    EXPECT_EQ(s.http->cancelled, std::vector<Http::Id>{id});
}

TEST(Audiobookshelf, PingErrorsAreToldApart) {
    {
        Server s; // no answer to /status
        PingResult ping;
        s.abs->ping(ping.callback());
        s.http->unreachable(0);
        s.abs->poll();
        EXPECT_EQ(ping.error, Error::Unreachable);
        EXPECT_EQ(s.http->calls.size(), 1u);
    }
    {
        Server s; // /status is not Audiobookshelf's
        PingResult ping;
        s.abs->ping(ping.callback());
        s.http->answer(0, kOk, "<html>hello</html>");
        s.abs->poll();
        EXPECT_EQ(ping.error, Error::Server);
        EXPECT_EQ(s.http->calls.size(), 1u);
    }
    {
        Server s; // /api/me fails with a 500
        PingResult ping;
        s.abs->ping(ping.callback());
        s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
        s.abs->poll();
        s.http->answer(1, 500, "");
        s.abs->poll();
        EXPECT_EQ(ping.error, Error::Server);
    }
    {
        Server s; // /api/me answers 200 with something that is not a user
        PingResult ping;
        s.abs->ping(ping.callback());
        s.http->answer(0, kOk, audiobookshelfFixture("status.json"));
        s.abs->poll();
        s.http->answer(1, kOk, "{}");
        s.abs->poll();
        EXPECT_EQ(ping.error, Error::Server);
    }
}

TEST(Audiobookshelf, CallbacksRunOnlyFromPoll) {
    Server s;
    ListResult<RemoteFolder> folders;
    s.abs->musicFolders(folders.callback());
    answerLibraries(s);
    EXPECT_EQ(folders.calls, 0);
    s.abs->poll();
    EXPECT_EQ(folders.calls, 1);
    s.abs->poll();
    EXPECT_EQ(folders.calls, 1);
}

TEST(Audiobookshelf, NewestIsSortedByDateAddedLatestFirst) {
    Server s;
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 500, list.callback(), kBooks);
    EXPECT_EQ(s.http->url(0), kBase + "/api/libraries/" + kBooks + "/items?limit=500&page=0&sort=addedAt&desc=1");
    s.http->answer(0, kOk, audiobookshelfFixture("books_newest.json"));
    s.abs->poll();
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[1].name, "Нейромант");
    EXPECT_FALSE(list.more);
}

TEST(Audiobookshelf, AlphabeticalIsSortedByTitleAndPagesFromTheOffset) {
    Server s;
    s.abs->albums("alphabeticalByName", 1000, 500, ListResult<RemoteAlbum>().callback(), kBooks);
    EXPECT_EQ(s.http->url(0), kBase + "/api/libraries/" + kBooks + "/items?limit=500&page=2&sort=media.metadata.title");
}

TEST(Audiobookshelf, AFullPageHasMoreWhileOffsetPlusSizeIsBelowTheTotal) {
    Server s;
    ListResult<RemoteAlbum> list;
    s.abs->albums("alphabeticalByName", 0, 1, list.callback(), kBooks); // the fixture says total 2
    s.http->answer(0, kOk, audiobookshelfFixture("books_az.json"));
    s.abs->poll();
    EXPECT_TRUE(list.more);
    ListResult<RemoteAlbum> last;
    s.abs->albums("alphabeticalByName", 1, 1, last.callback(), kBooks);
    EXPECT_EQ(s.http->url(1), kBase + "/api/libraries/" + kBooks + "/items?limit=1&page=1&sort=media.metadata.title");
    s.http->answer(1, kOk, audiobookshelfFixture("books_az.json"));
    s.abs->poll();
    EXPECT_FALSE(last.more);
}

TEST(Audiobookshelf, ThePageSizeIsKeptInRange) {
    Server s;
    s.abs->albums("newest", -5, 100000, ListResult<RemoteAlbum>().callback(), kBooks);
    s.abs->albums("newest", 0, 0, ListResult<RemoteAlbum>().callback(), kBooks);
    EXPECT_NE(s.http->url(0).find("limit=500&page=0"), std::string::npos) << s.http->url(0);
    EXPECT_NE(s.http->url(1).find("limit=1&page=0"), std::string::npos) << s.http->url(1);
}

TEST(Audiobookshelf, OtherListTypesAreEmptyAndSendNothing) {
    Server s;
    ListResult<RemoteAlbum> list;
    s.abs->albums("recent", 0, 10, list.callback(), kBooks);
    s.abs->albums("random", 0, 10, list.callback());
    EXPECT_EQ(list.calls, 0);
    s.abs->poll();
    EXPECT_EQ(list.calls, 2);
    EXPECT_EQ(list.error, Error::None);
    EXPECT_TRUE(list.items.empty());
    EXPECT_FALSE(list.more);
    EXPECT_TRUE(s.http->calls.empty());
}

TEST(Audiobookshelf, WithoutAFolderTheFirstBookLibraryIsAskedFirstAndKept) {
    Server s;
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 500, list.callback());
    ASSERT_EQ(s.http->calls.size(), 1u);
    EXPECT_EQ(s.http->url(0), kBase + "/api/libraries");
    answerLibraries(s);
    EXPECT_EQ(s.http->calls.size(), 1u); // the list waits for poll()
    s.abs->poll();
    ASSERT_EQ(s.http->calls.size(), 2u);
    EXPECT_EQ(s.http->url(1), kBase + "/api/libraries/" + kBooks + "/items?limit=500&page=0&sort=addedAt&desc=1");
    s.http->answer(1, kOk, audiobookshelfFixture("books_newest.json"));
    s.abs->poll();
    EXPECT_EQ(list.calls, 1);
    EXPECT_EQ(list.items.size(), 2u);

    s.abs->artists(ListResult<RemoteArtist>().callback()); // the library is known now: no second lookup
    ASSERT_EQ(s.http->calls.size(), 3u);
    EXPECT_EQ(s.http->url(2), kBase + "/api/libraries/" + kBooks + "/authors");
}

TEST(Audiobookshelf, TheTwoStepsOfARequestShareOneIdAndCancelStopsEither) {
    Server s;
    ListResult<RemoteAlbum> list;
    auto id = s.abs->albums("newest", 0, 500, list.callback());
    answerLibraries(s);
    s.abs->poll();
    ASSERT_EQ(s.http->calls.size(), 2u);
    EXPECT_EQ(s.http->calls[0].id, id);
    EXPECT_EQ(s.http->calls[1].id, id);
    s.abs->cancel(id); // while the second step is in flight
    s.http->answer(1, kOk, audiobookshelfFixture("books_newest.json"));
    s.abs->poll();
    EXPECT_EQ(list.calls, 0);
    EXPECT_EQ(s.http->cancelled, std::vector<Http::Id>{id});

    Server t;
    ListResult<RemoteAlbum> first;
    auto firstId = t.abs->albums("newest", 0, 500, first.callback());
    t.abs->cancel(firstId); // while the library lookup is in flight
    answerLibraries(t);
    t.abs->poll();
    EXPECT_EQ(first.calls, 0);
    EXPECT_EQ(t.http->calls.size(), 1u); // the list is never asked for
}

TEST(Audiobookshelf, TheConfiguredLibraryIsUsedByName) {
    Server s(config("Books"));
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 500, list.callback());
    answerLibraries(s);
    s.abs->poll();
    EXPECT_EQ(s.http->url(1).find(kBase + "/api/libraries/" + kBooks + "/items"), 0u) << s.http->url(1);
}

TEST(Audiobookshelf, ALibraryNamedAsAPodcastLibraryIsNeverUsed) {
    Server s(config("Podcasts"));
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 500, list.callback());
    answerLibraries(s);
    s.abs->poll();
    EXPECT_EQ(list.calls, 1);
    EXPECT_EQ(list.error, Error::Server);
    EXPECT_EQ(s.http->calls.size(), 1u);
}

TEST(Audiobookshelf, MusicFoldersAreTheBookLibrariesNeverAPodcastLibrary) {
    Server s;
    ListResult<RemoteFolder> folders;
    s.abs->musicFolders(folders.callback());
    EXPECT_EQ(s.http->url(0), kBase + "/api/libraries");
    answerLibraries(s);
    s.abs->poll();
    ASSERT_EQ(folders.items.size(), 1u);
    EXPECT_EQ(folders.items[0].id, kBooks);
    EXPECT_EQ(folders.items[0].name, "Books");
    EXPECT_FALSE(folders.more);
}

TEST(Audiobookshelf, MusicFoldersWithALibraryNamedGiveOnlyThatOne) {
    {
        Server s(config("Books"));
        ListResult<RemoteFolder> folders;
        s.abs->musicFolders(folders.callback());
        answerLibraries(s);
        s.abs->poll();
        ASSERT_EQ(folders.items.size(), 1u);
        EXPECT_EQ(folders.items[0].name, "Books");
    }
    {
        Server s(config("Elsewhere"));
        ListResult<RemoteFolder> folders;
        s.abs->musicFolders(folders.callback());
        answerLibraries(s);
        s.abs->poll();
        EXPECT_EQ(folders.error, Error::None);
        EXPECT_TRUE(folders.items.empty());
    }
}

TEST(Audiobookshelf, LibraryLookupFailuresAreReported) {
    Server s;
    ListResult<RemoteAlbum> list;
    s.abs->albums("newest", 0, 500, list.callback());
    s.http->unreachable(0);
    s.abs->poll();
    EXPECT_EQ(list.error, Error::Unreachable);
    EXPECT_EQ(s.http->calls.size(), 1u);

    Server noBooks;
    ListResult<RemoteAlbum> none;
    noBooks.abs->albums("newest", 0, 500, none.callback());
    noBooks.http->answer(0, kOk, R"({"libraries":[{"id":"p","name":"Pods","mediaType":"podcast","displayOrder":1}]})");
    noBooks.abs->poll();
    EXPECT_EQ(none.error, Error::Server);
}

TEST(Audiobookshelf, AuthorsAndTheirBooks) {
    Server s;
    ListResult<RemoteArtist> authors;
    s.abs->artists(authors.callback());
    answerLibraries(s);
    s.abs->poll();
    EXPECT_EQ(s.http->url(1), kBase + "/api/libraries/" + kBooks + "/authors");
    s.http->answer(1, kOk, audiobookshelfFixture("authors.json"));
    s.abs->poll();
    ASSERT_EQ(authors.items.size(), 2u);
    EXPECT_EQ(authors.items[0].name, "Уильям Гибсон");

    ListResult<RemoteAlbum> books;
    s.abs->artistAlbums(authors.items[0].id, books.callback());
    EXPECT_EQ(s.http->url(2), kBase + "/api/authors/7b1945ad-7fc0-458c-9fbe-877c290c4770?include=items");
    s.http->answer(2, kOk, audiobookshelfFixture("author_items.json"));
    s.abs->poll();
    ASSERT_EQ(books.items.size(), 1u);
    EXPECT_EQ(books.items[0].name, "Нейромант");
}

TEST(Audiobookshelf, BookIsTheExpandedItem) {
    Server s;
    BookResult book;
    s.abs->book(kNeuromancer, book.callback());
    EXPECT_EQ(s.http->url(0), kBase + "/api/items/" + kNeuromancer + "?expanded=1");
    s.http->answer(0, kOk, audiobookshelfFixture("item_neuromancer.json"));
    s.abs->poll();
    EXPECT_EQ(book.error, Error::None);
    EXPECT_EQ(book.book.tracks.size(), 24u);
    EXPECT_EQ(book.book.chapters.size(), 14u);
}

TEST(Audiobookshelf, AlbumTracksAreTheBooksFilesWithoutMore) {
    Server s;
    ListResult<RemoteTrack> tracks;
    s.abs->albumTracks("d5088199-1f1a-4fe1-b8aa-c7c4fb400838", tracks.callback());
    EXPECT_NE(s.http->url(0).find("/api/items/d5088199-1f1a-4fe1-b8aa-c7c4fb400838?expanded=1"), std::string::npos);
    s.http->answer(0, kOk, audiobookshelfFixture("item_m4b.json"));
    s.abs->poll();
    ASSERT_EQ(tracks.items.size(), 1u);
    EXPECT_FALSE(tracks.more);
    EXPECT_EQ(tracks.items[0].suffix, "m4b");
}

TEST(Audiobookshelf, ErrorsAreToldApart) {
    Server s;
    BookResult none, unauthorized, broken, bad;
    s.abs->book("a", none.callback());
    s.abs->book("b", unauthorized.callback());
    s.abs->book("c", broken.callback());
    s.abs->book("d", bad.callback());
    s.http->unreachable(0);
    s.http->answer(1, 401, "Unauthorized");
    s.http->answer(2, 500, "oops");
    s.http->answer(3, kOk, "this is not json");
    s.abs->poll();
    EXPECT_EQ(none.error, Error::Unreachable);
    EXPECT_EQ(unauthorized.error, Error::BadLogin);
    EXPECT_EQ(broken.error, Error::Server);
    EXPECT_EQ(bad.error, Error::Server);
    for (const BookResult* r : {&none, &unauthorized, &broken, &bad}) {
        EXPECT_TRUE(r->book.tracks.empty());
        EXPECT_TRUE(r->book.chapters.empty());
    }
}

TEST(Audiobookshelf, AnErrorStatusWithAGoodBodyIsStillAnError) {
    Server s;
    BookResult book;
    s.abs->book(kNeuromancer, book.callback());
    s.http->answer(0, 404, audiobookshelfFixture("item_neuromancer.json"));
    s.abs->poll();
    EXPECT_EQ(book.error, Error::Server);
    EXPECT_TRUE(book.book.tracks.empty());
}

TEST(Audiobookshelf, TheCatalogueVersionIsAlwaysEmptySoASyncAlwaysFetches) {
    Server s;
    Error error = Error::Server;
    std::string version = "x";
    int calls = 0;
    s.abs->catalogueVersion([&](Error e, std::string v) { ++calls, error = e, version = v; });
    EXPECT_EQ(calls, 0);
    s.abs->poll();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(error, Error::None);
    EXPECT_TRUE(version.empty());
    EXPECT_TRUE(s.http->calls.empty());
}

TEST(Audiobookshelf, ThereAreNoPlaylists) {
    Server s;
    ListResult<RemotePlaylist> playlists;
    s.abs->playlists(playlists.callback());
    ListResult<RemoteTrack> entries;
    s.abs->playlistTracks("p", entries.callback());
    Error created = Error::None;
    int createdCalls = 0;
    s.abs->createPlaylist("x", {"a"}, [&](Error e, std::string id) {
        ++createdCalls;
        created = e;
        EXPECT_TRUE(id.empty());
    });
    s.abs->poll();
    EXPECT_EQ(playlists.calls, 1);
    EXPECT_EQ(playlists.error, Error::None);
    EXPECT_TRUE(playlists.items.empty());
    EXPECT_EQ(entries.error, Error::Server);
    EXPECT_EQ(createdCalls, 1);
    EXPECT_EQ(created, Error::Server);
    s.abs->scrobble("a/1", true, std::nullopt, [](Error) { ADD_FAILURE() << "a scrobble is not answered"; });
    s.abs->poll();
    EXPECT_TRUE(s.http->calls.empty());
}

TEST(Audiobookshelf, IsABooksCatalogue) {
    Server s;
    EXPECT_EQ(s.abs->catalogue(), oap::MusicServer::Catalogue::Books);
}

TEST(Audiobookshelf, NothingIsConfiguredWithoutAUrlOrAKey) {
    for (int missing = 0; missing < 2; ++missing) {
        Audiobookshelf::Config c = config();
        (missing ? c.apiKey : c.url).clear();
        Server s(c);
        EXPECT_FALSE(s.abs->configured());
        ListResult<RemoteAlbum> albums;
        BookResult book;
        PingResult ping;
        ListResult<RemoteFolder> folders;
        Error versionError = Error::None;
        s.abs->albums("newest", 0, 10, albums.callback());
        s.abs->book("b", book.callback());
        s.abs->ping(ping.callback());
        s.abs->musicFolders(folders.callback());
        s.abs->catalogueVersion([&](Error e, std::string) { versionError = e; });
        s.abs->poll();
        EXPECT_EQ(albums.error, Error::NotConfigured);
        EXPECT_EQ(book.error, Error::NotConfigured);
        EXPECT_EQ(ping.error, Error::NotConfigured);
        EXPECT_EQ(folders.error, Error::NotConfigured);
        EXPECT_EQ(versionError, Error::NotConfigured);
        EXPECT_TRUE(s.http->calls.empty());
    }
}

TEST(Audiobookshelf, ACancelledRequestsCallbackDoesNotRun) {
    Server s;
    BookResult before, after, kept;
    auto beforeId = s.abs->book("a", before.callback());
    auto afterId = s.abs->book("b", after.callback());
    s.abs->book("c", kept.callback());
    s.abs->cancel(beforeId); // before the answer came
    s.http->answer(0, kOk, audiobookshelfFixture("item_m4b.json"));
    s.http->answer(1, kOk, audiobookshelfFixture("item_m4b.json"));
    s.http->answer(2, kOk, audiobookshelfFixture("item_m4b.json"));
    s.abs->cancel(afterId); // answered, but not polled yet
    s.abs->poll();
    EXPECT_EQ(before.calls, 0);
    EXPECT_EQ(after.calls, 0);
    EXPECT_EQ(kept.calls, 1);
    EXPECT_EQ(s.http->cancelled, (std::vector<Http::Id>{beforeId, afterId}));
    s.abs->cancel(9999); // unknown ids are ignored
    EXPECT_EQ(s.http->cancelled.size(), 2u);
}

TEST(Audiobookshelf, CancelledInstantAnswersDoNotRun) {
    Server s;
    ListResult<RemoteAlbum> list;
    auto id = s.abs->albums("random", 0, 10, list.callback());
    s.abs->cancel(id);
    s.abs->poll();
    EXPECT_EQ(list.calls, 0);
}

TEST(Audiobookshelf, IdsAreUnique) {
    Server s;
    auto a = s.abs->book("a", BookResult().callback());
    auto b = s.abs->albums("random", 0, 1, ListResult<RemoteAlbum>().callback());
    auto c = s.abs->playlists(ListResult<RemotePlaylist>().callback());
    EXPECT_NE(a, 0u);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST(Audiobookshelf, ConfigComesFromTheServerEntry) {
    oap::ServerConfig entry;
    entry.url = "https://x";
    entry.user = "ice";
    entry.apiKey = "k";
    entry.library = "Books";
    entry.password = "ignored";
    auto c = Audiobookshelf::configFrom(entry);
    EXPECT_EQ(c.url, "https://x");
    EXPECT_EQ(c.user, "ice");
    EXPECT_EQ(c.apiKey, "k");
    EXPECT_EQ(c.library, "Books");
}

TEST(Audiobookshelf, DestroyedWithRequestsInFlightIsSafe) {
    auto s = std::make_unique<Server>();
    s->abs->book("a", BookResult().callback());
    s->abs->albums("newest", 0, 10, ListResult<RemoteAlbum>().callback());
    s.reset();
}

TEST(Audiobookshelf, ProgressReadsTheTimelineAnd404MeansNoSavedPlace) {
    Server s;
    EXPECT_TRUE(s.abs->syncsProgress());
    int calls = 0;
    auto receive = [&](Error error, std::optional<oap::RemoteProgress> progress) {
        EXPECT_EQ(error, Error::None);
        if (++calls == 1) {
            ASSERT_TRUE(progress);
            EXPECT_DOUBLE_EQ(progress->seconds, 1234.5);
            EXPECT_DOUBLE_EQ(progress->duration, 22141.167955);
            EXPECT_FALSE(progress->finished);
            EXPECT_EQ(progress->updated, 1793260800123LL);
        } else EXPECT_FALSE(progress);
    };
    s.abs->progress("item /one", receive);
    ASSERT_EQ(s.http->calls.size(), 1u);
    EXPECT_EQ(s.http->url(0), kBase + "/api/me/progress/item%20%2Fone");
    EXPECT_EQ(s.http->calls[0].method, "GET");
    EXPECT_EQ(s.http->calls[0].headers, Http::Headers{"Authorization: Bearer " + kKey});
    s.http->answer(0, 200, audiobookshelfFixture("progress.json"));
    EXPECT_EQ(calls, 0);
    s.abs->poll();
    EXPECT_EQ(calls, 1);
    s.abs->progress("new", receive);
    s.http->answer(1, 404, "Not found");
    s.abs->poll();
    EXPECT_EQ(calls, 2);
}

TEST(Audiobookshelf, ProgressErrorsAndCancellationKeepNoPlace) {
    Server s;
    for (long status : {0, 401, 500, 200}) {
        int calls = 0;
        s.abs->progress("a", [&](Error error, std::optional<oap::RemoteProgress> value) {
            ++calls;
            EXPECT_EQ(error, status == 0 ? Error::Unreachable : status == 401 ? Error::BadLogin : Error::Server);
            EXPECT_FALSE(value);
        });
        auto n = s.http->calls.size() - 1;
        if (!status) s.http->unreachable(n);
        else s.http->answer(n, status, "{}");
        s.abs->poll();
        EXPECT_EQ(calls, 1);
    }
    int cancelled = 0;
    auto id = s.abs->progress("a", [&](Error, std::optional<oap::RemoteProgress>) { ++cancelled; });
    s.http->answer(4, 200, audiobookshelfFixture("progress.json"));
    s.abs->cancel(id);
    s.abs->poll();
    EXPECT_EQ(cancelled, 0);
    EXPECT_EQ(s.http->cancelled, std::vector<Http::Id>{id});
}

TEST(Audiobookshelf, SaveProgressPatchesJsonAndAcknowledgesOnlyFromPoll) {
    Server s;
    int calls = 0;
    s.abs->saveProgress("item /one", {1234.5, 4000, false, 1793260800123LL}, [&](Error error) {
        EXPECT_EQ(error, Error::None);
        ++calls;
    });
    ASSERT_EQ(s.http->calls.size(), 1u);
    const auto& request = s.http->calls[0];
    EXPECT_EQ(request.method, "PATCH");
    EXPECT_EQ(request.url, kBase + "/api/me/progress/item%20%2Fone");
    EXPECT_EQ(request.headers, Http::Headers{"Authorization: Bearer " + kKey});
    EXPECT_NE(request.body.find("\"currentTime\": 1234.5"), std::string::npos);
    EXPECT_NE(request.body.find("\"duration\": 4000"), std::string::npos);
    const auto fraction = request.body.find("\"progress\":");
    ASSERT_NE(fraction, std::string::npos);
    EXPECT_DOUBLE_EQ(std::stod(request.body.substr(fraction + 11)), 1234.5 / 4000);
    EXPECT_NE(request.body.find("\"isFinished\": false"), std::string::npos);
    // Quoted keys make the flow map JSON, not just YAML accepted by this test.
    EXPECT_NE(request.body.find("\"currentTime\":"), std::string::npos);
    s.http->answer(0, 204, "");
    EXPECT_EQ(calls, 0);
    s.abs->poll();
    s.abs->poll();
    EXPECT_EQ(calls, 1);
    s.abs->saveProgress("done", {4000, 4000, true, 0}); // no callback is valid
    const auto& finished = s.http->calls[1].body;
    EXPECT_NE(finished.find("\"isFinished\": true"), std::string::npos);
    EXPECT_NE(finished.find("\"progress\": 1"), std::string::npos);
    s.http->answer(1, 200, "{}");
    s.abs->poll();
}

TEST(Audiobookshelf, SaveProgressFailureReportsOnlyItsKindAndAcknowledgesTheError) {
    Server s;
    for (long status : {0, 401, 404, 500}) {
        int calls = 0;
        Error result = Error::None;
        testing::internal::CaptureStderr();
        s.abs->saveProgress("secret-book", {30, 100, false, 0}, [&](Error error) { ++calls; result = error; });
        auto n = s.http->calls.size() - 1;
        if (status) s.http->answer(n, status, kKey + " response-secret");
        else s.http->unreachable(n);
        EXPECT_EQ(calls, 0);
        s.abs->poll();
        auto logged = testing::internal::GetCapturedStderr();
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(result, !status ? Error::Unreachable : status == 401 ? Error::BadLogin : Error::Server);
        EXPECT_NE(logged.find("progress save failed"), std::string::npos);
        for (const auto& secret : {kKey, kBase, std::string("secret-book"), std::string("response-secret")})
            EXPECT_EQ(logged.find(secret), std::string::npos);
    }
}

TEST(Audiobookshelf, ProgressWithoutConfigurationAndInvalidSavesSendNothing) {
    auto c = config();
    c.apiKey.clear();
    Server absent(c);
    int read = 0, saved = 0;
    absent.abs->progress("a", [&](Error error, std::optional<oap::RemoteProgress> value) {
        ++read;
        EXPECT_EQ(error, Error::NotConfigured);
        EXPECT_FALSE(value);
    });
    absent.abs->saveProgress("a", {0, 0, false, 0}, [&](Error error) { ++saved; EXPECT_EQ(error, Error::NotConfigured); });
    EXPECT_EQ(read, 0);
    EXPECT_EQ(saved, 0);
    absent.abs->poll();
    EXPECT_EQ(read, 1);
    EXPECT_EQ(saved, 1);
    EXPECT_TRUE(absent.http->calls.empty());
    Server s;
    int invalid = 0;
    s.abs->saveProgress("a", {std::numeric_limits<double>::infinity(), 100, false, 0}, [&](Error error) {
        ++invalid;
        EXPECT_EQ(error, Error::Server);
    });
    s.abs->poll();
    EXPECT_EQ(invalid, 1);
    EXPECT_TRUE(s.http->calls.empty());
}

TEST(Audiobookshelf, ExitDrainDeliversOnlySaveAcknowledgementsOnTheCallingThread) {
    Server s;
    const auto caller = std::this_thread::get_id();
    int saves = 0, reads = 0;
    s.abs->progress("book", [&](Error, std::optional<oap::RemoteProgress>) { ++reads; });
    s.abs->saveProgress("book", {10, 100, false, 0}, [&](Error error) {
        EXPECT_EQ(error, Error::None);
        EXPECT_EQ(std::this_thread::get_id(), caller);
        ++saves;
    });
    s.http->answer(0, 200, audiobookshelfFixture("progress.json"));
    s.http->answer(1, 200, "{}");
    std::thread drain([&] { s.abs->finishTransport(std::chrono::milliseconds(123)); });
    drain.join();
    EXPECT_EQ(saves, 0);
    EXPECT_EQ(reads, 0);
    s.abs->finish(std::chrono::milliseconds(0));
    EXPECT_EQ(s.http->finishes, 1);
    EXPECT_EQ(s.http->finishDeadline.count(), 123);
    EXPECT_EQ(saves, 1);
    EXPECT_EQ(reads, 0);
    s.abs->poll();
    EXPECT_EQ(reads, 0);
}

TEST(Audiobookshelf, InProgressUsesTheChosenBookLibraryAndBearerHeader) {
    const std::string libraries = R"({"libraries":[{"id":"pod","name":"Podcasts","mediaType":"podcast","displayOrder":0},{"id":"first","name":"First","mediaType":"book","displayOrder":1},{"id":"chosen","name":"Books","mediaType":"book","displayOrder":2}]})";
    const std::string items = R"({"libraryItems":[{"id":"other","libraryId":"first","mediaType":"book","media":{"metadata":{"title":"Other"}}},{"id":"podcast","libraryId":"pod","mediaType":"podcast","media":{"metadata":{"title":"Podcast"}}},{"id":"b1","libraryId":"chosen","mediaType":"book","media":{"metadata":{"title":"Нейромант","authorName":"Уильям Гибсон"}}},{"id":"badtype","libraryId":"chosen","mediaType":"podcast","media":{"metadata":{"title":"Wrong type"}}}]})";
    for (const std::string library : {std::string("Books"), std::string("chosen"), std::string()}) {
        Server s(config(library));
        ListResult<RemoteAlbum> result;
        const auto id = s.abs->inProgress(result.callback());
        EXPECT_EQ(s.http->url(0), kBase + "/api/libraries");
        s.http->answer(0, kOk, libraries);
        EXPECT_EQ(result.calls, 0);
        s.abs->poll();
        ASSERT_EQ(s.http->calls.size(), 2u);
        EXPECT_EQ(s.http->calls[1].id, id);
        EXPECT_EQ(s.http->url(1), kBase + "/api/me/items-in-progress");
        for (const auto& request : s.http->calls) {
            EXPECT_EQ(request.headers, (Http::Headers{"Authorization: Bearer " + kKey}));
            EXPECT_EQ(request.url.find(kKey), std::string::npos);
        }
        s.http->answer(1, kOk, items);
        EXPECT_EQ(result.calls, 0);
        s.abs->poll();
        ASSERT_EQ(result.calls, 1);
        EXPECT_EQ(result.error, Error::None);
        EXPECT_FALSE(result.more);
        ASSERT_EQ(result.items.size(), 1u);
        EXPECT_EQ(result.items[0].id, library.empty() ? "other" : "b1");
        if (!library.empty()) {
            EXPECT_EQ(result.items[0].name, "Нейромант");
            EXPECT_EQ(result.items[0].artist, "Уильям Гибсон");
        }
    }
}

TEST(Audiobookshelf, InProgressErrorsEmptyAndCancellationFollowTheListContract) {
    for (const std::string body : {std::string("not json"), std::string("{}"), std::string(R"({"libraryItems":[]})")}) {
        Server s;
        ListResult<RemoteAlbum> result;
        s.abs->inProgress(result.callback());
        s.http->answer(0, kOk, audiobookshelfFixture("libraries.json")); s.abs->poll();
        s.http->answer(1, kOk, body); s.abs->poll();
        EXPECT_EQ(result.error, body == R"({"libraryItems":[]})" ? Error::None : Error::Server);
        EXPECT_TRUE(result.items.empty());
        EXPECT_FALSE(result.more);
    }
    for (int stage : {0, 1}) {
        Server s;
        ListResult<RemoteAlbum> result;
        auto id = s.abs->inProgress(result.callback());
        if (stage == 1) { s.http->answer(0, kOk, audiobookshelfFixture("libraries.json")); s.abs->poll(); }
        s.http->answer(stage, kOk, stage == 0 ? audiobookshelfFixture("libraries.json") : R"({"libraryItems":[]})");
        s.abs->cancel(id); s.abs->poll();
        EXPECT_EQ(result.calls, 0);
        EXPECT_EQ(s.http->calls.size(), size_t(stage + 1));
        EXPECT_EQ(s.http->cancelled, (std::vector<Http::Id>{id}));
    }
    for (long status : {401L, 500L}) {
        Server s;
        ListResult<RemoteAlbum> result;
        s.abs->inProgress(result.callback());
        s.http->answer(0, kOk, audiobookshelfFixture("libraries.json")); s.abs->poll();
        s.http->answer(1, status, "{}"); s.abs->poll();
        EXPECT_EQ(result.error, status == 401 ? Error::BadLogin : Error::Server);
    }
    Server absent(config());
    ListResult<RemoteAlbum> failed;
    absent.abs->inProgress(failed.callback());
    absent.http->unreachable(0); absent.abs->poll();
    EXPECT_EQ(failed.error, Error::Unreachable);
    Server unknown(config("Absent library"));
    ListResult<RemoteAlbum> missing;
    unknown.abs->inProgress(missing.callback());
    unknown.http->answer(0, kOk, audiobookshelfFixture("libraries.json")); unknown.abs->poll();
    EXPECT_EQ(missing.error, Error::Server);
    EXPECT_EQ(unknown.http->calls.size(), 1u);
    auto noConfig = config(); noConfig.apiKey.clear();
    Server none(noConfig);
    ListResult<RemoteAlbum> noServer;
    none.abs->inProgress(noServer.callback()); none.abs->poll();
    EXPECT_EQ(noServer.error, Error::NotConfigured);
    EXPECT_TRUE(none.http->calls.empty());
}

TEST(AudiobookshelfStars, UnsupportedRequestsFailFromPollWithoutHttp) {
    Server s;
    EXPECT_FALSE(s.abs->supportsStars());
    int calls = 0;
    s.abs->trackDetails("x", [&](Error e, std::vector<RemoteTrack> tracks, bool more) {
        EXPECT_EQ(e, Error::Server); EXPECT_TRUE(tracks.empty()); EXPECT_FALSE(more); ++calls;
    });
    s.abs->starredTracks([&](Error e, auto, bool) { EXPECT_EQ(e, Error::Server); ++calls; });
    s.abs->setStarred("x", false, [&](Error e) { EXPECT_EQ(e, Error::Server); ++calls; });
    EXPECT_EQ(calls, 0); s.abs->poll(); EXPECT_EQ(calls, 3); EXPECT_TRUE(s.http->calls.empty());
}

TEST(Audiobookshelf, PlaylistEditsAreUnsupportedAndCancellable) {
    Server s;
    std::vector<Error> got;
    auto done = [&](Error e) { got.push_back(e); };
    s.abs->addPlaylistTrack("p", "t", done);
    s.abs->removePlaylistTrack("p", 0, done);
    auto id = s.abs->deletePlaylist("p", done);
    s.abs->cancel(id);
    EXPECT_TRUE(got.empty());
    s.abs->poll();
    EXPECT_EQ(got, std::vector<Error>(2, Error::Server));
    EXPECT_TRUE(s.http->calls.empty());
}
