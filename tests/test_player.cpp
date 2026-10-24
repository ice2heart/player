// Tests for the FFmpeg Player on real files. WAV files are written here; the
// tests that need MP3 make them with the ffmpeg command and are skipped without it.

#include "media/player.h"
#include "temp_dir.h"

extern "C" {
#include <libavutil/error.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

using namespace oap;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// 16-bit PCM WAV with a 440 Hz tone.
std::vector<char> wavBytes(int sampleRate, int channels, double seconds) {
    auto frames = uint32_t(sampleRate * seconds);
    uint32_t dataBytes = frames * uint32_t(channels) * 2;
    std::vector<char> out;
    auto put = [&](uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
    };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    tag("RIFF"), put(36 + dataBytes, 4), tag("WAVE");
    tag("fmt "), put(16, 4), put(1, 2), put(uint32_t(channels), 2), put(uint32_t(sampleRate), 4);
    put(uint32_t(sampleRate * channels * 2), 4), put(uint32_t(channels * 2), 2), put(16, 2);
    tag("data"), put(dataBytes, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        auto s = int16_t(8000 * std::sin(2 * M_PI * 440 * i / sampleRate));
        for (int c = 0; c < channels; ++c) put(uint16_t(s), 2);
    }
    return out;
}

fs::path writeWav(const fs::path& path, int sampleRate, int channels, double seconds) {
    auto bytes = wavBytes(sampleRate, channels, seconds);
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    return path;
}

bool haveFfmpeg() { return std::system("ffmpeg -version >/dev/null 2>&1") == 0; }

// A tone as an MP3 without Xing or ID3 headers, so that files can simply be concatenated.
fs::path makeMp3(const fs::path& path, int sampleRate, int channels, double seconds) {
    std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i 'sine=frequency=440:sample_rate=" +
                      std::to_string(sampleRate) + ":duration=" + std::to_string(seconds) + "' -ac " +
                      std::to_string(channels) + " -c:a libmp3lame -write_xing 0 -id3v2_version 0 '" +
                      path.string() + "'";
    EXPECT_EQ(std::system(cmd.c_str()), 0) << cmd;
    return path;
}

fs::path concat(const fs::path& out, const std::vector<fs::path>& parts) {
    std::ofstream o(out, std::ios::binary);
    for (const auto& p : parts) o << std::ifstream(p, std::ios::binary).rdbuf();
    return out;
}

// Plays like the audio device would, as fast as the decoder goes, for at most `limit`
// frames or until the player has finished. Returns the frames heard.
size_t consume(Player& player, Mixer& mixer, size_t limit = SIZE_MAX) {
    std::vector<float> buf(1024 * AudioStream::kChannels);
    size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (total < limit && !player.finished() && std::chrono::steady_clock::now() < deadline) {
        size_t got = mixer.content().read(buf.data(), std::min<size_t>(1024, limit - total));
        total += got;
        if (got == 0) std::this_thread::sleep_for(1ms);
    }
    return total;
}

double seconds(size_t frames) { return double(frames) / Mixer::kSampleRate; }

class PlayerTest : public testing::Test {
protected:
    void SetUp() override {
        dir_ = test::testTempDir();
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
    Mixer mixer_;
};

} // namespace

TEST_F(PlayerTest, PlaysAWholeFile) {
    auto file = writeWav(dir_ / "tone.wav", 44100, 1, 1.5);
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string()));
    EXPECT_NEAR(player.duration(), 1.5, 0.01);
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 1.5, 0.02);
    EXPECT_TRUE(player.finished());
}

TEST_F(PlayerTest, StartPositionIsClampedToTheDuration) {
    auto file = writeWav(dir_ / "tone.wav", 48000, 2, 2.0);
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string(), 1000));
    EXPECT_LE(player.position(), player.duration());
    player.play();
    consume(player, mixer_);
    EXPECT_TRUE(player.finished());
    EXPECT_LE(player.position(), player.duration());
}

TEST_F(PlayerTest, StartPositionInsideTheFileIsUsed) {
    auto file = writeWav(dir_ / "tone.wav", 48000, 2, 2.0);
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string(), 1.5));
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 0.5, 0.02);
}

TEST_F(PlayerTest, SeekWhilePlayingJumpsToTheTarget) {
    auto file = writeWav(dir_ / "tone.wav", 48000, 2, 3.0);
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string()));
    player.play();
    consume(player, mixer_, Mixer::kSampleRate / 2);
    auto epoch = mixer_.content().epoch();
    player.seek(2.0);
    EXPECT_NEAR(player.position(), 2.0, 0.001);
    // Unlike a device, consume() would read the old queued audio before the decoder flushes it.
    for (int i = 0; i < 2000 && mixer_.content().epoch() == epoch; ++i) std::this_thread::sleep_for(1ms);
    ASSERT_NE(mixer_.content().epoch(), epoch) << "the decoder never seeked";
    EXPECT_NEAR(seconds(consume(player, mixer_)), 1.0, 0.01); // exactly from the target
    EXPECT_TRUE(player.finished());
}

// The resampler is set up for the first part; the second part has another rate
// and channel count. Converting it with the old setup plays it at the wrong
// speed (or reads planes that do not exist).
TEST_F(PlayerTest, FormatChangeMidStreamKeepsTheRightLength) {
    if (!haveFfmpeg()) GTEST_SKIP() << "needs the ffmpeg command";
    auto file = concat(dir_ / "chained.mp3", {makeMp3(dir_ / "a.mp3", 24000, 1, 1.0),
                                              makeMp3(dir_ / "b.mp3", 48000, 2, 1.0)});
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string()));
    player.play();
    // Each MP3 part has some encoder delay and padding.
    EXPECT_NEAR(seconds(consume(player, mixer_)), 2.07, 0.1);
}

TEST_F(PlayerTest, FormatChangeToFewerChannels) {
    if (!haveFfmpeg()) GTEST_SKIP() << "needs the ffmpeg command";
    auto file = concat(dir_ / "chained.mp3", {makeMp3(dir_ / "a.mp3", 48000, 2, 1.0),
                                              makeMp3(dir_ / "b.mp3", 24000, 1, 1.0)});
    Player player(mixer_);
    ASSERT_TRUE(player.open(file.string()));
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 2.07, 0.1);
}

TEST(PlayerReadError, EndOfFileEndsAtOnce) {
    int consecutive = 0;
    EXPECT_EQ(onReadError(AVERROR_EOF, false, consecutive), ReadErrorAction::End);
    EXPECT_EQ(onReadError(AVERROR(EIO), true, consecutive), ReadErrorAction::End);
}

TEST(PlayerReadError, OtherErrorsAreRetriedThenEnd) {
    int consecutive = 0;
    for (int i = 0; i < kMaxReadErrors; ++i)
        ASSERT_EQ(onReadError(AVERROR(EIO), false, consecutive), ReadErrorAction::Retry) << "error " << i;
    EXPECT_EQ(onReadError(AVERROR(EIO), false, consecutive), ReadErrorAction::End);
}
