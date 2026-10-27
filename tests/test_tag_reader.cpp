// Tests for FfmpegTagReader on small files the tests write with the ffmpeg command
// (a format whose encoder the local FFmpeg lacks is skipped).

#include "media/tag_reader.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace oap;
namespace fs = std::filesystem;

namespace {

constexpr double kSeconds = 2.0;
constexpr double kDurationTolerance = 0.15;

using Metadata = std::vector<std::pair<std::string, std::string>>;

struct Format {
    const char* name;
    const char* extension;
    const char* encoder;
    const char* extraArgs;
};

const Format kMp3{"mp3", "mp3", "libmp3lame", "-id3v2_version 3"};
const Format kFlac{"flac", "flac", "flac", ""};
// use_metadata_tags keeps keys the MP4 atoms have no name for, such as MusicBrainz Album Id.
// Ogg Opus keeps the comments in the stream, the others in the container.
const Format kOgg{"opus", "opus", "libopus", ""};
const Format kM4a{"m4a", "m4a", "aac", "-movflags use_metadata_tags"};

bool haveEncoder(const char* encoder) {
    std::string cmd = std::string("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' ") + encoder + " '";
    return std::system(cmd.c_str()) == 0;
}

// Values go between single quotes, so the tests keep apostrophes out of them.
bool makeAudio(const fs::path& path, const Format& format, const Metadata& metadata, bool stripTags = false) {
    std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=" +
                      std::to_string(kSeconds) + "' -c:a " + format.encoder + " " + format.extraArgs;
    if (stripTags) cmd += " -map_metadata -1 -id3v2_version 0 -write_xing 0";
    for (const auto& [key, value] : metadata) cmd += " -metadata '" + key + "=" + value + "'";
    cmd += " '" + path.string() + "'";
    return std::system(cmd.c_str()) == 0;
}

class TagReaderTest : public testing::Test {
protected:
    void SetUp() override {
        dir_ = test::testTempDir();
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Writes the file into `out`, or skips the test when ffmpeg or its encoder is missing
    // (the caller then returns on IsSkipped()).
    void make(fs::path& out, const Format& format, const Metadata& metadata, bool stripTags = false) {
        if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
            GTEST_SKIP() << "no ffmpeg command";
        } else if (!haveEncoder(format.encoder)) {
            GTEST_SKIP() << "this FFmpeg has no " << format.encoder << " encoder";
        }
        out = dir_ / (std::string("track.") + format.extension);
        EXPECT_TRUE(makeAudio(out, format, metadata, stripTags)) << "could not write " << out;
    }

    fs::path dir_;
    FfmpegTagReader reader_;
};

class TagReaderFormats : public TagReaderTest, public testing::WithParamInterface<Format> {};

} // namespace

TEST_P(TagReaderFormats, ReadsEveryField) {
    fs::path file;
    make(file, GetParam(), {{"title", "Blue Train"},
                                      {"artist", "John Coltrane"},
                                      {"album_artist", "Various Artists"},
                                      {"album", "Giant Steps"},
                                      {"genre", "Jazz"},
                                      {"track", "3/12"},
                                      {"disc", "2/4"},
                                      {"date", "1960-05-03"},
                                      {"MusicBrainz Album Id", "b84ee12a-09ef-421b-82de-0441a926375b"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Blue Train");
    EXPECT_EQ(tags->artist, "John Coltrane");
    EXPECT_EQ(tags->albumArtist, "Various Artists");
    EXPECT_EQ(tags->album, "Giant Steps");
    EXPECT_EQ(tags->genre, "Jazz");
    EXPECT_EQ(tags->track, 3);
    EXPECT_EQ(tags->disc, 2);
    EXPECT_EQ(tags->year, 1960);
    EXPECT_EQ(tags->musicBrainzAlbumId, "b84ee12a-09ef-421b-82de-0441a926375b");
    EXPECT_NEAR(tags->duration, kSeconds, kDurationTolerance);
}

TEST_P(TagReaderFormats, CyrillicAndEmojiSurvive) {
    fs::path file;
    make(file, GetParam(), {{"title", "Привет, мир \xF0\x9F\x8E\xA7"},
                                      {"artist", "Кино \xF0\x9F\x8E\xB8"},
                                      {"album", "Звезда по имени Солнце"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Привет, мир \xF0\x9F\x8E\xA7");
    EXPECT_EQ(tags->artist, "Кино \xF0\x9F\x8E\xB8");
    EXPECT_EQ(tags->album, "Звезда по имени Солнце");
}

TEST_P(TagReaderFormats, FileWithoutTagsGivesDurationAndEmptyTags) {
    fs::path file;
    make(file, GetParam(), {}, /*stripTags=*/true);
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_NEAR(tags->duration, kSeconds, kDurationTolerance);
    tags->duration = 0;
    EXPECT_EQ(*tags, TrackTags{});
}

INSTANTIATE_TEST_SUITE_P(Formats, TagReaderFormats, testing::Values(kMp3, kFlac, kM4a, kOgg),
                         [](const testing::TestParamInfo<Format>& info) { return std::string(info.param.name); });

// Vorbis comment names have any case in the wild.
TEST_F(TagReaderTest, KeysAreComparedWithoutCase) {
    fs::path file;
    make(file, kFlac, {{"TiTlE", "Mixed"},
                                 {"ARTIST", "Upper"},
                                 {"albumartist", "Joined"},
                                 {"Album", "Capital"},
                                 {"TRACKNUMBER", "7"},
                                 {"discnumber", "1"},
                                 {"musicbrainz_albumid", "11111111-2222-3333-4444-555555555555"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Mixed");
    EXPECT_EQ(tags->artist, "Upper");
    EXPECT_EQ(tags->albumArtist, "Joined");
    EXPECT_EQ(tags->album, "Capital");
    EXPECT_EQ(tags->track, 7);
    EXPECT_EQ(tags->disc, 1);
    EXPECT_EQ(tags->musicBrainzAlbumId, "11111111-2222-3333-4444-555555555555");
}

TEST_F(TagReaderTest, AlternativeKeyNamesAreRead) {
    fs::path file;
    make(file, kFlac, {{"MUSICBRAINZ_ALBUMID", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
                                 {"album artist", "Spaced"},
                                 {"year", "1999"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->musicBrainzAlbumId, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    EXPECT_EQ(tags->albumArtist, "Spaced");
    EXPECT_EQ(tags->year, 1999);
}

TEST_F(TagReaderTest, UnreadableNumbersAndDatesAreZero) {
    fs::path file;
    make(file, kFlac, {{"title", "Odd"}, {"track", "abc"}, {"disc", "/3"}, {"date", "05/03/2019"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Odd");
    EXPECT_EQ(tags->track, 0);
    EXPECT_EQ(tags->disc, 0);
    EXPECT_EQ(tags->year, 0);
}

TEST_F(TagReaderTest, LongDigitStringsAreNotYearsOrNumbers) {
    fs::path file;
    make(file, kFlac, {{"track", "123456789012"}, {"date", "20190"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->track, 0);
    EXPECT_EQ(tags->year, 0);
}

TEST_F(TagReaderTest, ShortOrDayFirstDatesAreNotYears) {
    fs::path file;
    make(file, kFlac, {{"date", "19"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->year, 0);
}

// A relative name that FFmpeg would take for a protocol ("file:") is still a file.
TEST_F(TagReaderTest, RelativeNameWithColonIsAFile) {
    fs::path file;
    make(file, kFlac, {{"title", "Other"}}); // colon.flac: what "file:colon.flac" opens when read as a URL
    if (IsSkipped()) return;
    fs::path colon = "file:colon.flac";
    ASSERT_TRUE(makeAudio(dir_ / colon, kFlac, {{"title", "Colon"}}));
    fs::rename(file, dir_ / "colon.flac");
    fs::path before = fs::current_path();
    fs::current_path(dir_);
    auto tags = reader_.read(colon);
    fs::current_path(before);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Colon");
}

TEST_F(TagReaderTest, ThreeDigitTrackNumbersAreRead) {
    fs::path file;
    make(file, kFlac, {{"track", "128/240"}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->track, 128);
}

TEST_F(TagReaderTest, ValuesAreTrimmed) {
    fs::path file;
    make(file, kFlac, {{"title", "  Padded  "}, {"artist", "   "}});
    if (IsSkipped()) return;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Padded");
    EXPECT_EQ(tags->artist, "");
}

// A tag the container leaves blank does not hide the one the audio stream has.
TEST_F(TagReaderTest, BlankContainerTagDoesNotHideStreamTag) {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) GTEST_SKIP() << "no ffmpeg command";
    if (!haveEncoder("flac")) GTEST_SKIP() << "this FFmpeg has no flac encoder";
    fs::path file = dir_ / "track.mka";
    std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i 'sine=duration=1' -c:a flac -metadata 'title= ' "
                      "-metadata:s:a:0 'title=Real' '" + file.string() + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
    auto tags = reader_.read(file);
    ASSERT_TRUE(tags);
    EXPECT_EQ(tags->title, "Real");
}

TEST_F(TagReaderTest, TextFileGivesNone) {
    fs::path file = dir_ / "notes.mp3";
    std::ofstream(file) << "This is not audio at all, only some words in a file.\n";
    EXPECT_FALSE(reader_.read(file));
}

TEST_F(TagReaderTest, MissingFileGivesNone) { EXPECT_FALSE(reader_.read(dir_ / "nothing.flac")); }

TEST_F(TagReaderTest, EmptyFileGivesNone) {
    fs::path file = dir_ / "empty.flac";
    std::ofstream{file};
    EXPECT_FALSE(reader_.read(file));
}

// A valid media file with no audio stream is not a track.
TEST_F(TagReaderTest, PictureWithoutAudioGivesNone) {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) GTEST_SKIP() << "no ffmpeg command";
    fs::path file = dir_ / "cover.png";
    std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i 'color=c=red:s=16x16' -frames:v 1 '" + file.string() + "'";
    if (std::system(cmd.c_str()) != 0) GTEST_SKIP() << "this FFmpeg cannot write a PNG";
    EXPECT_FALSE(reader_.read(file));
}

TEST_F(TagReaderTest, SeveralThreadsReadAtOnce) {
    fs::path file;
    make(file, kFlac, {{"title", "Shared"}, {"track", "5"}});
    if (IsSkipped()) return;
    constexpr int kThreads = 8;
    std::vector<std::optional<TrackTags>> results(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&, i] {
            for (int n = 0; n < 5; ++n) results[size_t(i)] = reader_.read(file);
        });
    for (auto& t : threads) t.join();
    for (const auto& r : results) {
        ASSERT_TRUE(r);
        EXPECT_EQ(r->title, "Shared");
        EXPECT_EQ(r->track, 5);
    }
}
