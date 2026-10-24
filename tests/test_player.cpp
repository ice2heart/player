// Tests for the FFmpeg Player on real files. WAV files are written here; the
// tests that need MP3 make them with the ffmpeg command and are skipped without it.

#include "media/player.h"
#include "temp_dir.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <unistd.h>
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

// What `run` writes to stderr (FFmpeg's own messages included).
std::string stderrOf(const fs::path& file, const std::function<void()>& run) {
    FILE* out = std::fopen(file.c_str(), "w");
    if (!out) {
        ADD_FAILURE() << "cannot write " << file;
        return {};
    }
    std::fflush(stderr);
    int saved = dup(2);
    dup2(fileno(out), 2);
    run();
    std::fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::fclose(out);
    std::stringstream text;
    text << std::ifstream(file).rdbuf();
    return text.str();
}

double seconds(size_t frames) { return double(frames) / Mixer::kSampleRate; }

// Time limits leave room for a slow machine (the Pi build runs the tests under
// qemu), yet stay well below the 10 s that a missing timeout or interrupt takes.
constexpr double kSlowMachine = 5.0;

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// A loopback socket bound to a free port.
int boundSocket(int& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ADD_FAILURE() << "cannot bind a loopback socket";
        if (fd >= 0) ::close(fd);
        port = 0;
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

// A port on this machine with nothing listening on it.
int unusedPort() {
    int port = 0;
    int fd = boundSocket(port);
    if (fd >= 0) ::close(fd); // (another program could take the port now, but hardly will)
    return port;
}

// An HTTP server on the loopback interface, so the stream tests need no
// internet. Each connection gets `reply` once its request has arrived and is
// then held open without another byte until the server is destroyed: a server
// that has stopped answering.
class StalledServer {
public:
    explicit StalledServer(std::string reply = {}) : reply_(std::move(reply)) {
        fd_ = boundSocket(port_);
        if (fd_ >= 0 && listen(fd_, 4) == 0) thread_ = std::thread([this] { run(); });
        else ADD_FAILURE() << "cannot listen on a loopback socket";
    }
    ~StalledServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        for (const Client& c : clients_)
            if (c.fd >= 0) ::close(c.fd);
        if (fd_ >= 0) ::close(fd_);
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int requests() const { return requests_; }

private:
    struct Client {
        int fd;
        std::string request; // until its end has come
    };

    void run() {
        while (!stop_) {
            std::vector<pollfd> fds{{fd_, POLLIN, 0}};
            for (const Client& c : clients_) fds.push_back({c.fd, POLLIN, 0}); // poll() skips fd -1
            if (poll(fds.data(), nfds_t(fds.size()), 10) <= 0) continue;
            if (fds[0].revents) {
                int c = accept(fd_, nullptr, nullptr);
                if (c >= 0) clients_.push_back({c, {}});
            }
            for (size_t i = 1; i < fds.size(); ++i) {
                if (fds[i].revents) receive(clients_[i - 1]);
            }
        }
    }

    void receive(Client& client) {
        char buf[4096];
        ssize_t n = read(client.fd, buf, sizeof buf);
        if (n <= 0) { // the player hung up
            ::close(client.fd);
            client.fd = -1;
            return;
        }
        client.request.append(buf, size_t(n));
        if (client.request.find("\r\n\r\n") == std::string::npos) return;
        ++requests_;
        client.request.clear();
        for (size_t sent = 0; sent < reply_.size();) {
            ssize_t k = send(client.fd, reply_.data() + sent, reply_.size() - sent, 0);
            if (k <= 0) break;
            sent += size_t(k);
        }
    }

    std::string reply_;
    int fd_ = -1;
    int port_ = 0;
    std::vector<Client> clients_; // the server thread's
    std::atomic<int> requests_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

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

// A stream URL holds the server login (docs/streaming.md): a failed open must not print it.
TEST_F(PlayerTest, AUrlThatCannotBeOpenedIsNotPrinted) {
    Player player(mixer_);
    std::string url = "file://" + (dir_ / "missing-secret-token.mp3").string();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(player.open(url)); });
    EXPECT_NE(log.find("player: cannot open"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

TEST_F(PlayerTest, AUrlWithoutAudioIsNotPrinted) {
    std::ofstream(dir_ / "secret-token.srt") << "1\n00:00:01,000 --> 00:00:02,000\nHello\n\n";
    Player player(mixer_);
    std::string url = "file://" + (dir_ / "secret-token.srt").string();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(player.open(url)); });
    EXPECT_NE(log.find("player: no audio stream"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

// ---------------------------------------------------------------- streams

// The server is gone: the open fails at once, not after reconnect attempts.
TEST_F(PlayerTest, AStreamOnAPortWithNothingListeningFailsAtOnce) {
    Player player(mixer_);
    std::string url = "http://127.0.0.1:" + std::to_string(unusedPort()) + "/rest/stream.view?u=me&t=secret-token";
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(player.open(url)); });
    EXPECT_LT(secondsSince(start), 2.0) << "even one retry would take 4 s";
    EXPECT_FALSE(player.isOpen());
    EXPECT_NE(log.find("player: cannot open a stream"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

// The open runs on the UI thread: a server that takes the request and never
// answers is given up at the open timeout (shortened here; it is 10 s).
TEST_F(PlayerTest, AStreamThatNeverAnswersIsGivenUpAtTheOpenTimeout) {
    StalledServer server;
    Player player(mixer_, std::chrono::milliseconds(300));
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        EXPECT_FALSE(player.open(server.url("/rest/stream.view?u=me&t=secret-token")));
    });
    double took = secondsSince(start);
    EXPECT_EQ(server.requests(), 1) << "the request reached the server";
    EXPECT_GE(took, 0.29);
    EXPECT_LT(took, kSlowMachine) << "the open must give up at its timeout";
    EXPECT_NE(log.find("no answer in time"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

// A WAV reply with a header for `total` seconds of which only `sent` seconds come.
std::string partialWavReply(double total, double sent) {
    const int rate = 8000;
    auto wav = wavBytes(rate, 1, total);
    size_t bytes = 44 + size_t(rate * sent) * 2;
    return "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " + std::to_string(wav.size()) +
           "\r\n\r\n" + std::string(wav.data(), bytes);
}

// FFmpeg reads 5 s of audio to learn the format. A stream that stops after 1 s
// is cut off by the timeout, and what came in time is not enough: no open.
TEST_F(PlayerTest, AStreamThatStopsWhileOpeningIsGivenUpAtTheOpenTimeout) {
    StalledServer server(partialWavReply(20, 1));
    Player player(mixer_, std::chrono::milliseconds(300));
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(player.open(server.url("/tone.wav"))); });
    EXPECT_LT(secondsSince(start), kSlowMachine);
    EXPECT_FALSE(player.isOpen());
    EXPECT_NE(log.find("no answer in time"), std::string::npos) << log;
}

// Not only http: any URL is given up at the open timeout (FFmpeg can also be
// sent to another protocol), here a raw TCP stream that never sends a byte.
TEST_F(PlayerTest, AnyUrlIsGivenUpAtTheOpenTimeout) {
    StalledServer server;
    Player player(mixer_, std::chrono::milliseconds(300));
    std::string url = server.url("");
    url.replace(0, 4, "tcp");
    auto start = std::chrono::steady_clock::now();
    stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(player.open(url)); });
    EXPECT_LT(secondsSince(start), kSlowMachine);
}

// The decoder thread waits for data that never comes: close() (also run by the
// destructor and by every open()) must not wait for the read timeout.
TEST_F(PlayerTest, CloseDoesNotWaitForAStreamThatStoppedSending) {
    StalledServer server(partialWavReply(20, 6));
    Player player(mixer_);
    ASSERT_TRUE(player.open(server.url("/tone.wav")));
    player.play();
    // Less than what came, so the player does not wait for more yet.
    EXPECT_NEAR(seconds(consume(player, mixer_, 5 * Mixer::kSampleRate)), 5.0, 0.01);
    std::this_thread::sleep_for(200ms); // the rest is queued: the decoder now waits on the socket
    EXPECT_FALSE(player.finished());
    auto start = std::chrono::steady_clock::now();
    player.close();
    EXPECT_LT(secondsSince(start), kSlowMachine);
    EXPECT_FALSE(player.isOpen());
}

// FFmpeg prints some URLs in its own messages, such as HLS and redirect errors.
TEST_F(PlayerTest, FfmpegMessagesShowNoStreamLogin) {
    Player player(mixer_); // installs the log filter
    std::string log = stderrOf(dir_ / "stderr.txt", [] {
        av_log(nullptr, AV_LOG_ERROR, "Opening '%s' for reading\n",
               "https://me:secret-password@music.home:4533/rest/stream.view?u=me&t=secret-token&s=salt");
        av_log(nullptr, AV_LOG_ERROR | AV_LOG_C(134), "a coloured error\n"); // colour bits above the level
        av_log(nullptr, AV_LOG_WARNING, "a warning\n");
    });
    EXPECT_NE(log.find("Opening 'https://music.home:4533/...' for reading"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret"), std::string::npos) << log;
    EXPECT_NE(log.find("a coloured error"), std::string::npos) << log;
    EXPECT_EQ(log.find("a warning"), std::string::npos) << "only errors, as before" << log;
}

TEST(RedactUrls, KeepsSchemeAndHostOnly) {
    EXPECT_EQ(redactUrls("plain text, no link\n"), "plain text, no link\n");
    EXPECT_EQ(redactUrls("Connection to tcp://10.0.0.2:4533 failed"), "Connection to tcp://10.0.0.2:4533 failed");
    EXPECT_EQ(redactUrls("a http://h/x?t=1 b \"https://u:p@h2?t=2\" c"), "a http://h/... b \"https://h2/...\" c");
    EXPECT_EQ(redactUrls("end http://h:1/rest?t=secret"), "end http://h:1/...");
    // Where the host begins is unclear: nothing after the scheme is kept.
    EXPECT_EQ(redactUrls("mail me@home, see http://h/a@b"), "mail me@home, see http://...");
    EXPECT_EQ(redactUrls("'https://me:pa/ss@h/x?t=1'"), "'https://...'");
    EXPECT_EQ(redactUrls("'https://me:pa?ss@h'"), "'https://...'");
}

// Request lines (FFmpeg logs them at debug level) have a query but no scheme.
TEST(RedactUrls, CutsQueriesWithoutAScheme) {
    EXPECT_EQ(redactUrls("request: GET /rest/stream.view?u=me&t=secret HTTP/1.1\n"),
              "request: GET /rest/stream.view?... HTTP/1.1\n");
    EXPECT_EQ(redactUrls("Continue? yes=no"), "Continue? yes=no");
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
