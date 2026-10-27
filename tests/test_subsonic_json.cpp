// Parsing saved Subsonic answers (tests/data/subsonic/) with yaml-cpp, including
// the ways JSON read as YAML can go wrong: \uXXXX surrogate pairs, \/ and one
// very long line.

#include "net/subsonic_json.h"
#include "subsonic_fixtures.h"

#include <gtest/gtest.h>

using namespace oap::subsonic;
using oap::test::subsonicFixture;

TEST(SubsonicJson, PingReturnsServerTypeAndVersion) {
    auto result = parsePing(subsonicFixture("ping.json"));
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.info.type, "navidrome");
    EXPECT_EQ(result.info.version, "0.64.2"); // without the build "(a1b2c3d4)"
}

TEST(SubsonicJson, PingFromAClassicServerSaysSubsonicAndTheApiVersion) {
    auto result = parsePing(subsonicFixture("ping_classic.json"));
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.info.type, "subsonic");
    EXPECT_EQ(result.info.version, "1.13.0");
}

TEST(SubsonicJson, ExtensionsAreListedByName) {
    auto result = parseExtensions(subsonicFixture("extensions.json")); // as the test server answered
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.items, (std::vector<std::string>{"transcodeOffset", "formPost", "songLyrics", "indexBasedQueue",
                                                      "transcoding", "playbackReport", "topSongsByArtistId"}));
    EXPECT_TRUE(parseExtensions(subsonicFixture("ping_classic.json")).items.empty()) << "a server without any";
    EXPECT_EQ(parseExtensions(subsonicFixture("error_70_not_found.json")).error, Error::Server);
}

TEST(SubsonicJson, WrongLoginIsBadLogin) {
    auto body = subsonicFixture("error_40_wrong_login.json");
    auto ping = parsePing(body);
    EXPECT_EQ(ping.error, Error::BadLogin);
    EXPECT_EQ(ping.info.type, "");
    auto list = parseAlbumList(body, 1);
    EXPECT_EQ(list.error, Error::BadLogin);
    EXPECT_TRUE(list.items.empty());
    EXPECT_FALSE(list.more);
}

TEST(SubsonicJson, NotAuthorizedAndNotFoundAreServerErrors) {
    EXPECT_EQ(parsePing(subsonicFixture("error_50_not_authorized.json")).error, Error::Server);
    auto notFound = parseAlbum(subsonicFixture("error_70_not_found.json"));
    EXPECT_EQ(notFound.error, Error::Server);
    EXPECT_TRUE(notFound.items.empty());
}

TEST(SubsonicJson, AnAnswerThatIsNotJsonIsAServerError) {
    EXPECT_EQ(parsePing(subsonicFixture("not_json.html")).error, Error::Server);
    EXPECT_EQ(parseArtists(subsonicFixture("not_json.html")).error, Error::Server);
    EXPECT_EQ(parsePing("").error, Error::Server);
    EXPECT_EQ(parsePing("{\"subsonic-response\": [1, 2").error, Error::Server); // cut off
}

TEST(SubsonicJson, AlbumListKeepsNonAsciiAndEmojiTitles) {
    auto list = parseAlbumList(subsonicFixture("album_list_unicode.json"), 10);
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 3u);
    EXPECT_EQ(list.items[0].id, "3a1f9c");
    EXPECT_EQ(list.items[0].name, "Времена года");
    EXPECT_EQ(list.items[0].artist, "Пётр Чайковский");
    EXPECT_EQ(list.items[0].songCount, 12);
    EXPECT_EQ(list.items[1].name, "Café del Mar 🎵");
    EXPECT_EQ(list.items[1].artist, "Björk & Sigur Rós");
    EXPECT_EQ(list.items[2].name, "東京 Nights");
    EXPECT_EQ(list.items[2].artist, ""); // JSON null, not "null"
}

TEST(SubsonicJson, AlbumListHasMoreOnlyWhenThePageIsFull) {
    auto body = subsonicFixture("album_list_unicode.json"); // 3 albums
    EXPECT_TRUE(parseAlbumList(body, 3).more);
    EXPECT_FALSE(parseAlbumList(body, 4).more);
    auto empty = parseAlbumList(subsonicFixture("album_list_empty.json"), 3);
    EXPECT_EQ(empty.error, Error::None);
    EXPECT_TRUE(empty.items.empty());
    EXPECT_FALSE(empty.more);
}

TEST(SubsonicJson, SurrogatePairEscapesBecomeEmoji) {
    auto list = parseAlbumList(subsonicFixture("album_list_escaped_emoji.json"), 10);
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[0].name, "Songs 🎵 for 😀 days");
    EXPECT_EQ(list.items[0].artist, "Café АБ");
    EXPECT_EQ(list.items[1].name, "Broken � pair"); // a lone half is replaced, the rest still reads
}

TEST(SubsonicJson, JsonForYamlRewritesOnlySurrogates) {
    // Checked on the text too, as yaml-cpp 0.9 would read the pairs itself.
    // (Plain strings: a compiler may read \u in a raw string as a character.)
    EXPECT_EQ(jsonForYaml("\"\\ud83c\\udfb5\""), "\"\\U0001F3B5\"");
    EXPECT_EQ(jsonForYaml("\"\\uD83D\\uDE00 \\u00e9\""), "\"\\U0001F600 \\u00e9\"");
    EXPECT_EQ(jsonForYaml("\"\\udfb5\\ud83c\""), "\"\\uFFFD\\uFFFD\""); // wrong order
    EXPECT_EQ(jsonForYaml("\"\\\\ud83c\""), "\"\\\\ud83c\"");         // an escaped backslash, not an escape
    EXPECT_EQ(jsonForYaml("\"a\\/b\\n\""), "\"a\\/b\\n\"");
}

TEST(SubsonicJson, EscapedSlashAndQuotes) {
    auto list = parseAlbumList(subsonicFixture("album_list_escaped_slash.json"), 10);
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 1u);
    EXPECT_EQ(list.items[0].id, "al/42");
    EXPECT_EQ(list.items[0].name, "AC/DC \"Live\" \\ Back");
    EXPECT_EQ(list.items[0].artist, "AC/DC");
}

TEST(SubsonicJson, VeryLongSingleLineDocument) {
    auto body = subsonicFixture("album_list_500_one_line.json"); // a full page, 80 kB on one line
    auto list = parseAlbumList(body, 500);
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 500u);
    EXPECT_TRUE(list.more);
    EXPECT_EQ(list.items[0].name, "Album 0 — Ünïcödé");
    EXPECT_EQ(list.items[499].id, "1001f3");
    EXPECT_EQ(list.items[499].name, "Album 499 — Ünïcödé");
    EXPECT_EQ(list.items[499].songCount, 499 % 20 + 1);
}

TEST(SubsonicJson, ArtistsFromAllIndexesInOrder) {
    auto list = parseArtists(subsonicFixture("artists.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 4u);
    EXPECT_EQ(list.items[0].id, "a1");
    EXPECT_EQ(list.items[0].name, "ABBA");
    EXPECT_EQ(list.items[0].albumCount, 3);
    EXPECT_EQ(list.items[1].name, "Aphex Twin");
    EXPECT_EQ(list.items[2].name, "2Pac"); // an index with one artist as an object, not a list
    EXPECT_EQ(list.items[3].name, "Би-2");
    EXPECT_FALSE(list.more);
}

TEST(SubsonicJson, ArtistAlbums) {
    auto list = parseArtist(subsonicFixture("artist.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[0].id, "al1");
    EXPECT_EQ(list.items[0].name, "Arrival");
    EXPECT_EQ(list.items[0].artist, "ABBA");
    EXPECT_EQ(list.items[1].name, "Voulez-Vous");
}

TEST(SubsonicJson, AlbumTracks) {
    auto list = parseAlbum(subsonicFixture("album.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    const auto& first = list.items[0];
    EXPECT_EQ(first.id, "s1");
    EXPECT_EQ(first.title, "When I Kissed the Teacher");
    EXPECT_EQ(first.artist, "ABBA");
    EXPECT_EQ(first.album, "Arrival");
    EXPECT_EQ(first.track, 1);
    EXPECT_DOUBLE_EQ(first.duration, 181);
    EXPECT_EQ(list.items[1].track, 2);
    EXPECT_DOUBLE_EQ(list.items[1].duration, 214.5);
}

TEST(SubsonicJson, TrackSuffixAndSize) {
    auto list = parseAlbum(subsonicFixture("album.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[0].suffix, "flac");
    EXPECT_EQ(list.items[0].size, 7020000);
    EXPECT_EQ(list.items[1].suffix, "flac");
    EXPECT_EQ(list.items[1].size, 0); // the server did not say
    auto playlist = parsePlaylist(subsonicFixture("playlist.json"));
    ASSERT_EQ(playlist.error, Error::None);
    ASSERT_FALSE(playlist.items.empty());
    EXPECT_EQ(playlist.items[0].suffix, "");
    EXPECT_EQ(playlist.items[0].size, 0);
}

TEST(SubsonicJson, Playlists) {
    auto list = parsePlaylists(subsonicFixture("playlists.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[0].id, "p1");
    EXPECT_EQ(list.items[0].name, "Morning");
    EXPECT_EQ(list.items[0].songCount, 12);
    EXPECT_EQ(list.items[1].name, "Сон 💤");
}

TEST(SubsonicJson, PlaylistTracks) {
    auto list = parsePlaylist(subsonicFixture("playlist.json"));
    ASSERT_EQ(list.error, Error::None);
    ASSERT_EQ(list.items.size(), 2u);
    EXPECT_EQ(list.items[0].id, "s2");
    EXPECT_EQ(list.items[1].title, "Jóga");
    EXPECT_EQ(list.items[1].artist, "Björk");
    EXPECT_EQ(list.items[1].album, "Homogenic");
    EXPECT_DOUBLE_EQ(list.items[1].duration, 305);
}
