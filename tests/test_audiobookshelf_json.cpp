// The Audiobookshelf parsers on answers saved from the test server (tests/data/audiobookshelf/).

#include "audiobookshelf_fixtures.h"
#include "net/audiobookshelf_json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <limits>

using namespace oap::audiobookshelf;
using oap::test::audiobookshelfFixture;

namespace {
const char* const kNeuromancerId = "171aa11a-a575-44be-b21e-8c01199a27d9";
const char* const kM4bId = "d5088199-1f1a-4fe1-b8aa-c7c4fb400838";
const char* const kBooksLibrary = "585d09d2-286c-4dba-be50-e1f75681dee9";
} // namespace

TEST(AudiobookshelfJson, StatusGivesTypeAndVersion) {
    auto result = parseStatus(audiobookshelfFixture("status.json"));
    EXPECT_EQ(result.error, Error::None);
    EXPECT_EQ(result.info.type, "Audiobookshelf");
    EXPECT_EQ(result.info.version, "2.37.2");
}

TEST(AudiobookshelfJson, StatusOfAnotherAppOrNotJsonIsServerError) {
    EXPECT_EQ(parseStatus(R"({"app":"something-else","serverVersion":"1"})").error, Error::Server);
    EXPECT_EQ(parseStatus("<html>login</html>").error, Error::Server);
    EXPECT_EQ(parseStatus("").error, Error::Server);
}

TEST(AudiobookshelfJson, MeIsCheckedAndNotKept) {
    EXPECT_EQ(parseMe(audiobookshelfFixture("me.json")), Error::None);
    EXPECT_EQ(parseMe("{}"), Error::Server);
    EXPECT_EQ(parseMe("<html></html>"), Error::Server);
}

TEST(AudiobookshelfJson, LibrariesAreOnlyBooksInDisplayOrder) {
    auto parsed = parseLibraries(audiobookshelfFixture("libraries.json"));
    ASSERT_EQ(parsed.error, Error::None);
    ASSERT_EQ(parsed.items.size(), 1u); // the Podcasts library is left out
    EXPECT_EQ(parsed.items[0].id, kBooksLibrary);
    EXPECT_EQ(parsed.items[0].name, "Books");

    auto ordered = parseLibraries(R"({"libraries":[
        {"id":"b","name":"Second","mediaType":"book","displayOrder":2},
        {"id":"p","name":"Pods","mediaType":"podcast","displayOrder":0},
        {"id":"a","name":"First","mediaType":"book","displayOrder":1}]})");
    ASSERT_EQ(ordered.items.size(), 2u);
    EXPECT_EQ(ordered.items[0].id, "a");
    EXPECT_EQ(ordered.items[1].id, "b");
}

TEST(AudiobookshelfJson, BookListGivesCyrillicTitlesAndAuthors) {
    auto parsed = parseBookList(audiobookshelfFixture("books_newest.json"), 0, 500);
    ASSERT_EQ(parsed.error, Error::None);
    ASSERT_EQ(parsed.items.size(), 2u);
    EXPECT_EQ(parsed.items[0].id, kM4bId);
    EXPECT_EQ(parsed.items[0].name, "Вперед в прошлое");
    EXPECT_EQ(parsed.items[0].artist, "Ратманов Денис");
    EXPECT_EQ(parsed.items[0].songCount, 1);
    EXPECT_EQ(parsed.items[0].genre, "Историческая фантастика");
    EXPECT_EQ(parsed.items[0].year, 0); // no year
    EXPECT_EQ(parsed.items[0].created, "2026-10-25T16:08:49Z");
    EXPECT_EQ(parsed.items[1].id, kNeuromancerId);
    EXPECT_EQ(parsed.items[1].name, "Нейромант");
    EXPECT_EQ(parsed.items[1].artist, "Уильям Гибсон");
    EXPECT_EQ(parsed.items[1].songCount, 24);
    EXPECT_EQ(parsed.items[1].genre, "Фантастика"); // the first of two
}

TEST(AudiobookshelfJson, MoreWhileOffsetPlusSizeIsBelowTotal) {
    const std::string body = audiobookshelfFixture("books_newest.json"); // total 2
    EXPECT_TRUE(parseBookList(body, 0, 1).more);
    EXPECT_FALSE(parseBookList(body, 0, 2).more);   // the page reaches the total
    EXPECT_FALSE(parseBookList(body, 0, 500).more);
    EXPECT_FALSE(parseBookList(body, 1, 2).more);
    EXPECT_FALSE(parseBookList(body, 1, 1).more);
}

TEST(AudiobookshelfJson, BookListOfTitlesKeepsTheServersOrder) {
    auto parsed = parseBookList(audiobookshelfFixture("books_az.json"), 0, 500);
    ASSERT_EQ(parsed.items.size(), 2u);
    EXPECT_EQ(parsed.items[0].name, "Вперед в прошлое");
    EXPECT_EQ(parsed.items[1].name, "Нейромант");
}

TEST(AudiobookshelfJson, BadBodiesAreServerErrorsWithNothingElse) {
    auto parsed = parseBookList("<html>", 0, 10);
    EXPECT_EQ(parsed.error, Error::Server);
    EXPECT_TRUE(parsed.items.empty());
    EXPECT_FALSE(parsed.more);
    EXPECT_EQ(parseAuthors("nope").error, Error::Server);
    EXPECT_EQ(parseBook("nope").error, Error::Server);
    EXPECT_EQ(parseBook("{}").error, Error::Server);
    EXPECT_EQ(parseLibraries("[1,2]").error, Error::Server);
}

TEST(AudiobookshelfJson, AuthorsGiveNamesAndBookCounts) {
    auto parsed = parseAuthors(audiobookshelfFixture("authors.json"));
    ASSERT_EQ(parsed.error, Error::None);
    ASSERT_EQ(parsed.items.size(), 2u);
    EXPECT_EQ(parsed.items[0].id, "7b1945ad-7fc0-458c-9fbe-877c290c4770");
    EXPECT_EQ(parsed.items[0].name, "Уильям Гибсон");
    EXPECT_EQ(parsed.items[0].albumCount, 1);
    EXPECT_EQ(parsed.items[1].name, "Ратманов Денис");
}

TEST(AudiobookshelfJson, AuthorBooksAreTheirLibraryItems) {
    auto parsed = parseAuthorBooks(audiobookshelfFixture("author_items.json"));
    ASSERT_EQ(parsed.error, Error::None);
    ASSERT_EQ(parsed.items.size(), 1u);
    EXPECT_EQ(parsed.items[0].id, kNeuromancerId);
    EXPECT_EQ(parsed.items[0].name, "Нейромант");
    EXPECT_FALSE(parsed.more);
}

TEST(AudiobookshelfJson, ExpandedBookGivesTwentyFourTracksInOrderAndFourteenChapters) {
    auto result = parseBook(audiobookshelfFixture("item_neuromancer.json"));
    ASSERT_EQ(result.error, Error::None);
    const auto& tracks = result.book.tracks;
    ASSERT_EQ(tracks.size(), 24u);
    for (std::size_t i = 0; i < tracks.size(); ++i) EXPECT_EQ(tracks[i].track, int(i) + 1);
    EXPECT_EQ(tracks[0].id, std::string(kNeuromancerId) + "/13502477");
    EXPECT_EQ(tracks[0].title, "001_Часть первая.Тиба-Сити блюз_Глава_1");
    EXPECT_EQ(tracks[0].suffix, "mp3");
    EXPECT_EQ(tracks[0].size, 87366148);
    EXPECT_DOUBLE_EQ(tracks[0].duration, 3626.717333);
    EXPECT_EQ(tracks[0].album, "Нейромант");
    EXPECT_EQ(tracks[0].artist, "Уильям Гибсон");
    EXPECT_EQ(tracks[0].albumId, kNeuromancerId);
    EXPECT_EQ(tracks[1].id, std::string(kNeuromancerId) + "/13502343");
    EXPECT_EQ(tracks[1].size, 49351081);
    EXPECT_EQ(tracks[23].id, std::string(kNeuromancerId) + "/13502480");
    EXPECT_EQ(tracks[23].size, 16648149);

    const auto& chapters = result.book.chapters;
    ASSERT_EQ(chapters.size(), 14u); // chapters span files: not one per file
    EXPECT_EQ(chapters[0].title, "001_Часть первая.Тиба-Сити блюз_Глава_1");
    EXPECT_DOUBLE_EQ(chapters[0].start, 0);
    EXPECT_DOUBLE_EQ(chapters[1].start, 3626.717333);
    EXPECT_EQ(chapters[13].title, "023_Часть_четвёртая_Рейд_на_виллу_«Блуждающий огонек»_23");
    EXPECT_NEAR(chapters[13].start, 21081.066622, 1e-6);
}

TEST(AudiobookshelfJson, ExpandedM4bGivesOneTrackAndTwentyThreeChapters) {
    auto result = parseBook(audiobookshelfFixture("item_m4b.json"));
    ASSERT_EQ(result.error, Error::None);
    ASSERT_EQ(result.book.tracks.size(), 1u);
    EXPECT_EQ(result.book.tracks[0].id, std::string(kM4bId) + "/13502582");
    EXPECT_EQ(result.book.tracks[0].suffix, "m4b");
    EXPECT_EQ(result.book.tracks[0].size, 244012642);
    ASSERT_EQ(result.book.chapters.size(), 23u);
    EXPECT_EQ(result.book.chapters[22].title, "Глава 23. ДЕСЯТЬ БЕСТОЛКОВЫХ ДНЕЙ");
    EXPECT_NEAR(result.book.chapters[22].start, 31948.232812, 1e-6);
}

TEST(AudiobookshelfJson, TrackIdSurvivesTheSeparator) {
    // "<item id>/<ino>": the item id has no '/', so the last part is the ino.
    auto result = parseBook(audiobookshelfFixture("item_m4b.json"));
    const std::string& id = result.book.tracks.at(0).id;
    EXPECT_EQ(id.find('/'), id.rfind('/'));
}

TEST(AudiobookshelfJson, IsoTimeIsUtc) {
    EXPECT_EQ(isoTime(0), "");
    EXPECT_EQ(isoTime(-5), "");
    EXPECT_EQ(isoTime(1000), "1970-01-01T00:00:01Z");
    EXPECT_EQ(isoTime(1792944529772), "2026-10-25T16:08:49Z");
    EXPECT_EQ(isoTime(951782400000), "2000-02-29T00:00:00Z"); // a leap day
    EXPECT_EQ(isoTime(1709251199999), "2024-02-29T23:59:59Z");
}

TEST(AudiobookshelfJson, FixturesHoldNeitherTheTestKeyNorAToken) {
    // The key is never in a tracked file, so it is not in this test either: .env is read when it is there.
    std::string key;
    std::ifstream env(std::string(OAP_AUDIOBOOKSHELF_DATA) + "/../../../.env");
    for (std::string line; std::getline(env, line);) {
        const std::string name = "OAP_BOOKSHELF_API_KEY=";
        if (line.compare(0, name.size(), name) == 0) key = line.substr(name.size());
    }
    while (!key.empty() && (key.front() == '"' || key.front() == '\'')) key.erase(key.begin());
    while (!key.empty() && (key.back() == '"' || key.back() == '\'' || key.back() == '\r')) key.pop_back();
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(OAP_AUDIOBOOKSHELF_DATA)) {
        if (!entry.is_regular_file()) continue;
        ++files;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        const std::string body = all.str();
        const std::string name = entry.path().filename().string();
        EXPECT_EQ(body.find("\"token\""), std::string::npos) << name;
        EXPECT_EQ(body.find("token="), std::string::npos) << name;
        EXPECT_EQ(body.find("eyJ"), std::string::npos) << name; // the start of a JWT
        if (!key.empty()) EXPECT_EQ(body.find(key), std::string::npos) << name;
    }
    EXPECT_GE(files, 9);
}

TEST(AudiobookshelfJson, ProgressRejectsMalformedMissingAndNonFiniteFields) {
    for (const std::string& body : {std::string("not json"), std::string("{}"),
            std::string(R"({"duration":10,"isFinished":false,"lastUpdate":1000})"),
            std::string(R"({"currentTime":-1,"duration":10,"isFinished":false,"lastUpdate":1000})"),
            std::string(R"({"currentTime":1,"duration":-10,"isFinished":false,"lastUpdate":1000})"),
            std::string(R"({"currentTime":1,"duration":10,"isFinished":false,"lastUpdate":-1})"),
            std::string(R"({"currentTime":".nan","duration":10,"isFinished":false,"lastUpdate":1000})"),
            std::string(R"({"currentTime":1,"duration":".inf","isFinished":false,"lastUpdate":1000})")}) {
        auto result = parseProgress(body);
        EXPECT_EQ(result.error, Error::Server) << body;
        EXPECT_FALSE(result.progress) << body;
    }
    auto finished = parseProgress(R"({"currentTime":0,"duration":0,"isFinished":true,"lastUpdate":0})");
    ASSERT_TRUE(finished.progress);
    EXPECT_TRUE(finished.progress->finished);
    EXPECT_EQ(finished.error, Error::None);
}

TEST(AudiobookshelfJson, ProgressBodyHandlesUnknownDurationFinishedAndInvalidNumbers) {
    for (auto progress : {oap::RemoteProgress{0, 0, false, 0}, {5, 0, true, 0}, {200, 100, false, 0}}) {
        auto encoded = progressBody(progress);
        ASSERT_TRUE(encoded);
        const auto fraction = encoded->find("\"progress\":");
        ASSERT_NE(fraction, std::string::npos);
        EXPECT_DOUBLE_EQ(std::stod(encoded->substr(fraction + 11)), progress.finished || progress.duration > 0 ? 1 : 0);
    }
    for (auto progress : {oap::RemoteProgress{-1, 10, false, 0}, {1, -10, false, 0},
                          {std::numeric_limits<double>::quiet_NaN(), 10, false, 0},
                          {1, std::numeric_limits<double>::infinity(), false, 0}})
        EXPECT_FALSE(progressBody(progress));
}
