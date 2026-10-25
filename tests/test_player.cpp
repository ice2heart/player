// Tests for the FFmpeg Player on real files. WAV files are written here; the
// tests that need MP3 make them with the ffmpeg command and are skipped without it.

#include "media/player.h"
#include "temp_dir.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <arpa/inet.h>
#include <optional>
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
#include <mutex>
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
// that has stopped answering. With HangUp, the server instead closes the
// connection and stops listening after the first reply: a server that went away.
// After holdReplies(), requests get their reply only once release() is called:
// a slow server.
class StalledServer {
public:
    enum AfterReply { Hold, HangUp };
    explicit StalledServer(std::string reply = {}, AfterReply after = Hold) : reply_(std::move(reply)), after_(after) {
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
    void holdReplies() { held_ = true; }
    void release() { held_ = false; }

private:
    struct Client {
        int fd;
        std::string request; // until its end has come
        bool waiting = false; // its request has come, its reply is held back
    };

    void run() {
        while (!stop_) {
            for (Client& c : clients_)
                if (c.waiting && !held_ && c.fd >= 0) answer(c);
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
        if (held_) client.waiting = true;
        else answer(client);
    }

    void answer(Client& client) {
        client.waiting = false;
        for (size_t sent = 0; sent < reply_.size();) {
            ssize_t k = send(client.fd, reply_.data() + sent, reply_.size() - sent, 0);
            if (k <= 0) break;
            sent += size_t(k);
        }
        if (after_ == HangUp) { // a reconnect is refused
            ::close(client.fd);
            client.fd = -1;
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
        }
    }

    std::string reply_;
    AfterReply after_;
    int fd_ = -1;
    int port_ = 0;
    std::vector<Client> clients_; // the server thread's
    std::atomic<int> requests_{0};
    std::atomic<bool> held_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// A stream opens in the background: waits until the open is done, and says
// whether it worked.
bool openAndWait(Player& player, const std::string& url) {
    if (!player.open(url)) return false;
    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (player.opening() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    EXPECT_FALSE(player.opening()) << "the open never ended";
    return player.isOpen();
}

// Waits until `done` is true, or `limit` has passed.
bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return done();
}

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
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, url)); });
    EXPECT_NE(log.find("player: cannot open"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

TEST_F(PlayerTest, AUrlWithoutAudioIsNotPrinted) {
    std::ofstream(dir_ / "secret-token.srt") << "1\n00:00:01,000 --> 00:00:02,000\nHello\n\n";
    Player player(mixer_);
    std::string url = "file://" + (dir_ / "secret-token.srt").string();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, url)); });
    EXPECT_NE(log.find("player: no audio stream"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

// ---------------------------------------------------------------- streams

// The server is gone: the open fails at once, not after reconnect attempts.
TEST_F(PlayerTest, AStreamOnAPortWithNothingListeningFailsAtOnce) {
    Player player(mixer_);
    std::string url = "http://127.0.0.1:" + std::to_string(unusedPort()) + "/rest/stream.view?u=me&t=secret-token";
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, url)); });
    EXPECT_LT(secondsSince(start), 2.0) << "even one retry would take 4 s";
    EXPECT_FALSE(player.isOpen());
    EXPECT_NE(log.find("player: cannot open a stream"), std::string::npos) << log;
    EXPECT_EQ(log.find("secret-token"), std::string::npos) << log;
}

// A server that takes the request and never answers is given up at the open
// timeout (shortened here; it is 10 s), so "Loading" does not go on forever.
TEST_F(PlayerTest, AStreamThatNeverAnswersIsGivenUpAtTheOpenTimeout) {
    StalledServer server;
    Player player(mixer_, std::chrono::milliseconds(300));
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        EXPECT_FALSE(openAndWait(player, server.url("/rest/stream.view?u=me&t=secret-token")));
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
    std::string log =
        stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, server.url("/tone.wav"))); });
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
    stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, url)); });
    EXPECT_LT(secondsSince(start), kSlowMachine);
}

// The open runs on the decoder thread: open() returns at once, and the caller
// (the UI) goes on while the server keeps it waiting.
TEST_F(PlayerTest, OpeningAStreamThatDoesNotAnswerDoesNotBlockTheCaller) {
    StalledServer server;
    Player player(mixer_, 8s); // within ctest's 10 s, so a blocking open fails rather than times out
    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(player.open(server.url("/rest/stream.view?u=me&t=secret-token")));
    EXPECT_LT(secondsSince(start), 1.0);
    EXPECT_TRUE(player.opening());
    EXPECT_FALSE(player.isOpen());
    EXPECT_DOUBLE_EQ(player.duration(), 0);
    player.play(); // nothing to play yet: ignored
    EXPECT_FALSE(player.playing());
    EXPECT_TRUE(waitFor([&] { return server.requests() == 1; })) << "the open goes on meanwhile";
    EXPECT_TRUE(player.opening());
}

// Home or another track while a stream opens: close() returns at once, the open
// is over, and it reports nothing (nobody waits for it any more).
TEST_F(PlayerTest, CloseDropsAnOpenStillRunning) {
    StalledServer server;
    Player player(mixer_);
    ASSERT_TRUE(player.open(server.url("/tone.wav")));
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; })); // the open waits for the answer
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        auto start = std::chrono::steady_clock::now();
        player.close();
        EXPECT_LT(secondsSince(start), 1.0);
        EXPECT_FALSE(player.opening());
        std::this_thread::sleep_for(300ms); // FFmpeg has seen the interrupt by now
    });
    EXPECT_FALSE(player.opening());
    EXPECT_FALSE(player.isOpen());
    EXPECT_EQ(log.find("cannot open"), std::string::npos) << log;
}

TEST_F(PlayerTest, ANewOpenDropsAStreamStillOpening) {
    StalledServer server;
    auto file = writeWav(dir_ / "tone.wav", 44100, 1, 1.5);
    Player player(mixer_);
    ASSERT_TRUE(player.open(server.url("/tone.wav")));
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; })); // the open waits for the answer
    ASSERT_TRUE(player.open(file.string()));
    EXPECT_TRUE(player.isOpen()) << "a local file opens at once";
    EXPECT_FALSE(player.opening());
    std::this_thread::sleep_for(500ms); // the dropped open has given up by now
    EXPECT_TRUE(player.isOpen()) << "the dropped open did not close the new file";
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 1.5, 0.1);
}

// Plays until the player has finished or failed, or `limit` has passed.
void playOut(Player& player, Mixer& mixer, std::chrono::seconds limit) {
    std::vector<float> buf(1024 * AudioStream::kChannels);
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!player.finished() && !player.failed() && std::chrono::steady_clock::now() < deadline)
        if (mixer.content().read(buf.data(), 1024) == 0) std::this_thread::sleep_for(1ms);
}

// Wi-Fi off or the server gone mid-track: once the retries give up (after a cap,
// shortened here), the stream has failed, at the place it got to. It has not ended.
TEST_F(PlayerTest, AStreamThatBreaksOffFailsInsteadOfFinishing) {
    StalledServer server(partialWavReply(20, 6), StalledServer::HangUp);
    Player player(mixer_, kStreamOpenTimeout, 500ms);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    stderrOf(dir_ / "stderr.txt", [&] { playOut(player, mixer_, 9s); });
    EXPECT_TRUE(player.failed());
    EXPECT_FALSE(player.finished());
    EXPECT_NEAR(player.position(), 6.0, 0.1);
}

TEST_F(PlayerTest, AWholeStreamFinishesAndDoesNotFail) {
    StalledServer server(partialWavReply(6, 6), StalledServer::HangUp);
    Player player(mixer_);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    EXPECT_NEAR(player.duration(), 6.0, 0.01) << "what the open learnt reaches the caller";
    player.play();
    playOut(player, mixer_, 9s);
    EXPECT_TRUE(player.finished());
    EXPECT_FALSE(player.failed());
}

// A music server for one file: it answers Range requests (each connection on its
// own thread, since FFmpeg opens the next before it closes the last). Until
// goDown(), a reply that would pass byte `cutAt` sends up to it and then waits,
// as a slow network would. goDown() is the Wi-Fi gone: those connections are
// closed, and for `downFor` every new one is closed at once; then the server
// answers in full again.
class FlakyServer {
public:
    FlakyServer(std::string body, size_t cutAt, std::string type = "audio/wav")
        : body_(std::move(body)), cutAt_(cutAt), type_(std::move(type)) {
        fd_ = boundSocket(port_);
        if (fd_ >= 0 && listen(fd_, 8) == 0) thread_ = std::thread([this] { run(); });
        else ADD_FAILURE() << "cannot listen on a loopback socket";
    }
    ~FlakyServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard lock(mutex_);
            for (int fd : open_) shutdown(fd, SHUT_RDWR); // ends a send() the player no longer reads
        }
        for (std::thread& t : clients_) t.join();
        if (fd_ >= 0) ::close(fd_);
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    void goDown(std::chrono::milliseconds downFor) {
        std::lock_guard lock(mutex_);
        downAt_ = std::chrono::steady_clock::now();
        downFor_ = downFor;
    }
    // When each connection was refused while the server was down.
    std::vector<std::chrono::steady_clock::time_point> refused() const {
        std::lock_guard lock(mutex_);
        return refused_;
    }
    // Replies cut short at `cutAt`, so far.
    int held() const { return held_; }

private:
    bool down() const { return downAt_ && std::chrono::steady_clock::now() - *downAt_ < downFor_; }

    void run() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 10) <= 0) continue;
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            std::lock_guard lock(mutex_);
            if (down()) {
                refused_.push_back(std::chrono::steady_clock::now());
                ::close(c);
                continue;
            }
            open_.push_back(c);
            clients_.emplace_back([this, c] { serve(c); });
        }
    }

    void serve(int fd) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) return finish(fd);
            request.append(buf, size_t(n));
        }
        size_t start = 0;
        size_t range = request.find("Range: bytes=");
        if (range != std::string::npos) start = std::stoul(request.substr(range + 13));
        start = std::min(start, body_.size());
        std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Type: " + type_ + "\r\nAccept-Ranges: bytes\r\n" +
                           "Content-Length: " + std::to_string(body_.size() - start) + "\r\nContent-Range: bytes " +
                           std::to_string(start) + "-" + std::to_string(body_.size() - 1) + "/" +
                           std::to_string(body_.size()) + "\r\n\r\n";
        bool hold;
        {
            std::lock_guard lock(mutex_);
            hold = !downAt_ && start < cutAt_ && cutAt_ < body_.size();
        }
        size_t end = hold ? cutAt_ : body_.size();
        if (sendAll(fd, head) && sendAll(fd, body_.substr(start, end - start)) && hold) {
            ++held_;
            while (!stop_) { // until the network goes
                {
                    std::lock_guard lock(mutex_);
                    if (downAt_) break;
                }
                std::this_thread::sleep_for(5ms);
            }
        }
        finish(fd);
    }

    static std::string chunks(const std::string& data) {
        constexpr size_t kChunk = 4096;
        std::string out;
        for (size_t at = 0; at < data.size(); at += kChunk) {
            std::string part = data.substr(at, kChunk);
            char size[16];
            std::snprintf(size, sizeof size, "%zx\r\n", part.size());
            out += size + part + "\r\n";
        }
        return out;
    }

    static bool sendAll(int fd, const std::string& data) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        for (size_t sent = 0; sent < data.size();) {
            ssize_t k = send(fd, data.data() + sent, data.size() - sent, flags);
            if (k <= 0) return false;
            sent += size_t(k);
        }
        return true;
    }

    void finish(int fd) {
        std::lock_guard lock(mutex_);
        open_.erase(std::find(open_.begin(), open_.end(), fd));
        ::close(fd);
    }

    std::string body_;
    size_t cutAt_;
    std::string type_;
    int fd_ = -1;
    int port_ = 0;
    mutable std::mutex mutex_; // for the five below
    std::optional<std::chrono::steady_clock::time_point> downAt_;
    std::chrono::milliseconds downFor_{0};
    std::vector<std::chrono::steady_clock::time_point> refused_;
    std::vector<int> open_;
    std::vector<std::thread> clients_; // only the accept thread adds, before the destructor joins
    std::atomic<int> held_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::string wavString(double seconds) {
    auto wav = wavBytes(8000, 1, seconds);
    return std::string(wav.data(), wav.size());
}
size_t wavOffset(double seconds) { return 44 + size_t(8000 * seconds) * 2; }

// FFmpeg reads 5 s of audio to open a stream: the dropouts below come after that.
constexpr double kCutAt = 8;

// Plays the stream until it has come up to `cutAt` and the player waits for
// more, then takes the server down for `downFor`. Returns the frames heard by then.
size_t playUntilHeld(Mixer& mixer, FlakyServer& server, std::chrono::milliseconds downFor) {
    std::vector<float> buf(1024 * AudioStream::kChannels);
    size_t heard = 0;
    // Some of what came: the rest is queued, and the decoder waits on the socket.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (heard < Mixer::kSampleRate && std::chrono::steady_clock::now() < deadline) {
        size_t got = mixer.content().read(buf.data(), 1024);
        heard += got;
        if (got == 0) std::this_thread::sleep_for(1ms);
    }
    EXPECT_GE(heard, size_t(Mixer::kSampleRate)) << "the stream did not play";
    EXPECT_TRUE(waitFor([&] { return server.held() > 0; })) << "the reply was cut short at cutAt";
    std::this_thread::sleep_for(200ms);
    server.goDown(downFor);
    return heard;
}

// Hears everything queued, as fast as it comes, until the player has run dry
// (buffering()) and nothing more comes for 100 ms: the decoder has written all
// it had. Returns false if that did not happen within 5 s. Fast, so that the
// playing time before a test pauses is short even on a busy machine.
bool drainUntilDry(Player& player, Mixer& mixer) {
    std::vector<float> buf(1024 * AudioStream::kChannels);
    auto drain = [&] {
        size_t got = 0;
        for (size_t n; (n = mixer.content().read(buf.data(), 1024)) > 0;) got += n;
        return got;
    };
    return waitFor([&] {
        drain();
        if (!player.buffering()) return false;
        std::this_thread::sleep_for(100ms);
        return drain() == 0 && player.buffering();
    });
}

// A short dropout mid-track: what came plays, the player runs dry (buffering()),
// retries, and the track goes on where it stopped, with nothing lost or repeated.
TEST_F(PlayerTest, AStreamThatBreaksOffBrieflyGoesOnWhereItStopped) {
    FlakyServer server(wavString(12), wavOffset(kCutAt));
    Player player(mixer_);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    std::vector<float> buf(1024 * AudioStream::kChannels);
    bool wasDry = false, dryWhilePaused = false;
    size_t heard = 0;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        heard = playUntilHeld(mixer_, server, 1500ms);
        auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!player.finished() && !player.failed() && std::chrono::steady_clock::now() < deadline) {
            size_t got = mixer_.content().read(buf.data(), 1024);
            heard += got;
            if (got == 0 && player.buffering() && !wasDry) {
                wasDry = true;
                player.pause();
                dryWhilePaused = player.buffering();
                player.play();
            }
            if (got == 0) std::this_thread::sleep_for(1ms);
        }
    });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_FALSE(player.failed());
    EXPECT_FALSE(server.refused().empty()) << "the server was down when the player first came back";
    EXPECT_TRUE(wasDry) << "the player ran dry while it waited";
    EXPECT_FALSE(dryWhilePaused) << "a paused player is not buffering";
    EXPECT_FALSE(player.buffering()) << "a finished stream is not buffering";
    EXPECT_NEAR(seconds(heard), 12.0, 0.02) << log;
    EXPECT_NE(log.find("connection is back"), std::string::npos) << log;
}

// The server does not come back: the player retries, each time waiting twice as
// long, and gives up after the cap (shortened here), at the place it got to.
TEST_F(PlayerTest, AStreamThatStaysAwayIsRetriedWithBackoffThenFails) {
    FlakyServer server(wavString(20), wavOffset(kCutAt));
    Player player(mixer_, kStreamOpenTimeout, 2500ms);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    std::chrono::steady_clock::time_point downAt;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        playUntilHeld(mixer_, server, std::chrono::hours(1));
        downAt = std::chrono::steady_clock::now();
        playOut(player, mixer_, 8s);
    });
    double took = secondsSince(downAt);
    EXPECT_TRUE(player.failed()) << log;
    EXPECT_FALSE(player.finished());
    EXPECT_FALSE(player.buffering()) << "a failed stream is not buffering";
    EXPECT_NEAR(player.position(), kCutAt, 0.1);
    EXPECT_GE(took, 2.5) << "retried until the cap";
    EXPECT_LT(took, 2.5 + kSlowMachine);
    // FFmpeg reconnects once at once; then Player retries after 1 s and after 2 s more.
    auto refused = server.refused();
    ASSERT_EQ(refused.size(), 3u) << log;
    double first = std::chrono::duration<double>(refused[1] - refused[0]).count();
    double second = std::chrono::duration<double>(refused[2] - refused[1]).count();
    // A wait is never shorter; on a busy machine it can be a good deal longer.
    EXPECT_GE(first, 0.95);
    EXPECT_LT(first, 1.0 + kSlowMachine / 5);
    EXPECT_GE(second, 1.95) << "each wait is twice the last";
    EXPECT_LT(second, 2.0 + kSlowMachine / 5);
    EXPECT_NE(log.find("giving up"), std::string::npos) << log;
}

// Paused, nobody waits for the stream: the time it is paused does not count
// towards the cap, so Play after a long pause still finds it retrying.
TEST_F(PlayerTest, APausedStreamDoesNotGiveUpItsRetries) {
    FlakyServer server(wavString(20), wavOffset(kCutAt));
    Player player(mixer_, kStreamOpenTimeout, 2500ms);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        playUntilHeld(mixer_, server, std::chrono::hours(1));
        EXPECT_TRUE(drainUntilDry(player, mixer_)) << "it ran dry";
        player.pause();
        std::this_thread::sleep_for(4s); // well past the cap
        player.play();
    });
    EXPECT_TRUE(player.buffering()) << "still retrying" << log;
    EXPECT_FALSE(player.failed());
    EXPECT_EQ(log.find("giving up"), std::string::npos) << log;
}

// Only the time it plays counts, to the moment: a pause that covers one retry
// and ends in the wait for the next leaves that wait's paused part out too.
// The retries start at 0, 1 and 3 s after the loss. Paused from just after the
// loss to 1.9 s, the stream has played about 1.1 s at the third, which is under
// the cap; counted from the second (paused) retry it would be 2 s, at the cap.
TEST_F(PlayerTest, APauseInTheWaitBeforeARetryIsLeftOutOfTheCap) {
    FlakyServer server(wavString(20), wavOffset(kCutAt));
    Player player(mixer_, kStreamOpenTimeout, 2000ms);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        playUntilHeld(mixer_, server, std::chrono::hours(1));
        EXPECT_TRUE(drainUntilDry(player, mixer_)) << "it ran dry";
        player.pause();
        std::this_thread::sleep_for(1900ms);
        player.play();
        std::this_thread::sleep_for(1600ms); // past the third retry (3 s), before the fourth (7 s)
    });
    EXPECT_FALSE(player.failed()) << log;
    EXPECT_TRUE(player.buffering()) << "still retrying" << log;
    EXPECT_EQ(log.find("giving up"), std::string::npos) << log;
}

// A seek during a dropout that works (here from what FFmpeg still holds) is the
// user listening on: the retries after it get the full time again.
TEST_F(PlayerTest, ASeekDuringADropoutGivesTheRetriesTheFullTimeAgain) {
    FlakyServer server(wavString(20), wavOffset(kCutAt));
    Player player(mixer_, kStreamOpenTimeout, 2500ms);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
    player.play();
    std::vector<float> buf(1024 * AudioStream::kChannels);
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        playUntilHeld(mixer_, server, std::chrono::hours(1));
        EXPECT_TRUE(drainUntilDry(player, mixer_)) << "it ran dry";
        // Past the cap: the retries would give up at the next error, were it not for the seek.
        std::this_thread::sleep_for(2700ms);
        player.seek(kCutAt - 1);
        auto until = std::chrono::steady_clock::now() + 500ms; // what the seek gave is heard
        while (std::chrono::steady_clock::now() < until && !player.failed())
            if (mixer_.content().read(buf.data(), 1024) == 0) std::this_thread::sleep_for(1ms);
    });
    EXPECT_EQ(log.find("cannot seek"), std::string::npos) << "the seek must work for this test" << log;
    EXPECT_TRUE(player.buffering()) << "still retrying" << log;
    EXPECT_FALSE(player.failed());
}

// Read-ahead: the next track's stream is preloaded while a track plays. The
// server here answers once and then goes away, so the stream can only play
// from the connection the preload made.
TEST_F(PlayerTest, APreloadedStreamOpensAtOnceWithoutANewConnection) {
    StalledServer server(partialWavReply(6, 6), StalledServer::HangUp);
    auto file = writeWav(dir_ / "tone.wav", 44100, 1, 1.5);
    Player player(mixer_);
    std::string url = server.url("/rest/stream.view?id=2&t=secret-token");
    player.preload(url);
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; }));
    player.preload(url); // App asks again at every tick: it goes on with the same one
    // What plays meanwhile is not disturbed, and an open of another path keeps the preload.
    ASSERT_TRUE(player.open(file.string()));
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 1.5, 0.01);
    ASSERT_TRUE(player.finished());
    std::this_thread::sleep_for(1s); // the preload has opened by now (it reads a few kB from loopback)
    auto start = std::chrono::steady_clock::now();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_TRUE(player.open(url, 2)); });
    EXPECT_LT(secondsSince(start), 1.0);
    EXPECT_TRUE(player.isOpen()) << "open at once" << log;
    EXPECT_FALSE(player.opening());
    EXPECT_NEAR(player.duration(), 6.0, 0.01);
    EXPECT_NEAR(player.position(), 2.0, 0.01) << "where open() asked for";
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 4.0, 0.01);
    EXPECT_TRUE(player.finished());
    EXPECT_FALSE(player.failed());
    EXPECT_EQ(server.requests(), 1) << "no second connection";
}

// The track ends before the preload has opened: open() goes on with it.
TEST_F(PlayerTest, APreloadStillRunningIsTakenOverByOpen) {
    StalledServer server(partialWavReply(6, 6), StalledServer::HangUp);
    server.holdReplies();
    Player player(mixer_);
    std::string url = server.url("/tone.wav");
    player.preload(url);
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; }));
    ASSERT_TRUE(player.open(url, 2));
    EXPECT_TRUE(player.opening());
    EXPECT_FALSE(player.isOpen());
    server.release();
    EXPECT_TRUE(waitFor([&] { return !player.opening(); }));
    ASSERT_TRUE(player.isOpen());
    EXPECT_NEAR(player.position(), 2.0, 0.01) << "where open() asked for";
    player.play();
    playOut(player, mixer_, 9s);
    EXPECT_TRUE(player.finished());
    EXPECT_EQ(server.requests(), 1) << "no second connection";
}

// Taken over while it waits, the preload then gives up: open() goes on with a
// new open, as if the preload had failed before it.
TEST_F(PlayerTest, APreloadThatFailsAfterOpenTookItOverIsOpenedAgain) {
    StalledServer server(partialWavReply(6, 6), StalledServer::HangUp);
    server.holdReplies();
    Player player(mixer_, 500ms);
    std::string url = server.url("/tone.wav");
    player.preload(url);
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; }));
    ASSERT_TRUE(player.open(url));
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        EXPECT_TRUE(waitFor([&] { return server.requests() == 2; })) << "opened again after the timeout";
        server.release();
        EXPECT_TRUE(waitFor([&] { return !player.opening(); }));
    });
    ASSERT_TRUE(player.isOpen()) << log;
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 6.0, 0.01);
}

// The preload found the server down: open() opens the stream again, as without read-ahead.
TEST_F(PlayerTest, AFailedPreloadFallsBackToANormalOpen) {
    FlakyServer server(wavString(3), SIZE_MAX);
    Player player(mixer_);
    std::string url = server.url("/tone.wav");
    server.goDown(800ms);
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        player.preload(url);
        EXPECT_TRUE(waitFor([&] { return !server.refused().empty(); }));
        std::this_thread::sleep_for(1s); // the preload has failed, and the server is back
        EXPECT_TRUE(openAndWait(player, url));
    });
    ASSERT_TRUE(player.isOpen()) << log;
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 3.0, 0.01);
    EXPECT_TRUE(player.finished());
}

TEST_F(PlayerTest, ACancelledPreloadIsNotUsed) {
    StalledServer server(partialWavReply(6, 6), StalledServer::HangUp);
    Player player(mixer_);
    std::string url = server.url("/tone.wav");
    player.preload(url);
    ASSERT_TRUE(waitFor([&] { return server.requests() == 1; }));
    std::this_thread::sleep_for(500ms); // it has opened
    player.cancelPreload();
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, url)); });
    EXPECT_FALSE(player.isOpen()) << "the server has gone: only the dropped preload could have played";
}

TEST_F(PlayerTest, APreloadOfAnotherPathReplacesTheLast) {
    StalledServer first(partialWavReply(6, 6), StalledServer::HangUp);
    StalledServer second(partialWavReply(6, 6), StalledServer::HangUp);
    Player player(mixer_);
    player.preload(first.url("/tone.wav"));
    ASSERT_TRUE(waitFor([&] { return first.requests() == 1; }));
    player.preload(second.url("/tone.wav"));
    ASSERT_TRUE(waitFor([&] { return second.requests() == 1; }));
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { EXPECT_FALSE(openAndWait(player, first.url("/tone.wav"))); });
    EXPECT_TRUE(openAndWait(player, second.url("/tone.wav"))) << "the second preload is kept";
    EXPECT_EQ(second.requests(), 1) << "no second connection";
}

// A preload that waits on a server that does not answer is no reason to wait:
// the Player goes away at once (the device powers off).
TEST_F(PlayerTest, DestroyingThePlayerDoesNotWaitForAPreload) {
    StalledServer server;
    auto start = std::chrono::steady_clock::now();
    {
        Player player(mixer_);
        player.preload(server.url("/tone.wav"));
        ASSERT_TRUE(waitFor([&] { return server.requests() == 1; }));
    }
    EXPECT_LT(secondsSince(start), 1.0 + 0.5);
}

TEST_F(PlayerTest, ALocalFileIsNotPreloaded) {
    auto file = writeWav(dir_ / "tone.wav", 44100, 1, 1.5);
    Player player(mixer_);
    player.preload(file.string()); // does nothing: a local file opens at once anyway
    ASSERT_TRUE(player.open(file.string()));
    EXPECT_TRUE(player.isOpen());
    player.play();
    EXPECT_NEAR(seconds(consume(player, mixer_)), 1.5, 0.01);
}

// The decoder thread waits for data that never comes: close() (also run by the
// destructor and by every open()) must not wait for the read timeout.
TEST_F(PlayerTest, CloseDoesNotWaitForAStreamThatStoppedSending) {
    StalledServer server(partialWavReply(20, 6));
    Player player(mixer_);
    ASSERT_TRUE(openAndWait(player, server.url("/tone.wav")));
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

// A transcoding music server for one track (Subsonic stream with format and
// maxBitRate): it makes the audio as it sends it, so a reply has no length and
// no byte ranges ("Accept-Ranges: none", a Range header is ignored). It comes
// in chunks (Transfer-Encoding: chunked, as Navidrome sends it), and FFmpeg
// reports its end as a read error, as it does for Navidrome. With timeOffset=<seconds> in the URL the reply
// starts there, as Navidrome's transcodeOffset does; past the end it holds no
// audio (Navidrome sends a few bytes FFmpeg cannot open), or with HeaderOnly
// just a WAV header. With cutFirstAt, the first
// reply stops there and waits until reset(), which breaks the connection off
// (TCP reset: an error), as a lost network does.
class TranscodeServer {
public:
    enum PastEnd { NoAudio, HeaderOnly };
    struct Request {
        std::string target;   // the path and query of the request line
        long rangeStart = -1; // from a Range header, -1 without one
    };
    explicit TranscodeServer(double seconds, std::optional<double> cutFirstAt = std::nullopt, PastEnd pastEnd = NoAudio)
        : seconds_(seconds), cutFirstAt_(cutFirstAt), pastEnd_(pastEnd) {
        fd_ = boundSocket(port_);
        if (fd_ >= 0 && listen(fd_, 8) == 0) thread_ = std::thread([this] { run(); });
        else ADD_FAILURE() << "cannot listen on a loopback socket";
    }
    ~TranscodeServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard lock(mutex_);
            for (int fd : open_) shutdown(fd, SHUT_RDWR); // ends a send() the player no longer reads
        }
        for (std::thread& t : clients_) t.join();
        if (fd_ >= 0) ::close(fd_);
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::vector<Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    bool held() const { return held_; }
    void reset() { reset_ = true; }

    // What Navidrome sends for a transcode past its end: an ID3v2.4 tag (here all
    // padding, 1200 bytes, the size in 7-bit bytes) and no audio.
    static std::string id3Only() {
        constexpr size_t kTagSize = 1200;
        std::string tag = "ID3\x04";
        tag += std::string(2, '\0'); // revision, flags
        for (int shift : {21, 14, 7, 0}) tag += char((kTagSize >> shift) & 0x7f);
        return tag + std::string(kTagSize, '\0');
    }

    // A WAV header for a stream of unknown length, then `seconds` of tone at 8 kHz mono.
    static std::string streamedWav(double seconds) {
        std::string wav = wavString(seconds);
        for (size_t at : {size_t(4), size_t(40)}) wav.replace(at, 4, "\xff\xff\xff\xff");
        return wav;
    }

private:
    void run() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 10) <= 0) continue;
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            std::lock_guard lock(mutex_);
            open_.push_back(c);
            clients_.emplace_back([this, c] { serve(c); });
        }
    }

    void serve(int fd) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) return finish(fd, false);
            request.append(buf, size_t(n));
        }
        Request r;
        size_t space = request.find(' ');
        r.target = request.substr(space + 1, request.find(' ', space + 1) - space - 1);
        size_t range = request.find("Range: bytes=");
        if (range != std::string::npos) r.rangeStart = std::stol(request.substr(range + 13));
        double from = 0;
        size_t offset = r.target.find("timeOffset=");
        if (offset != std::string::npos) from = std::stod(r.target.substr(offset + 11));
        bool cut;
        {
            std::lock_guard lock(mutex_);
            cut = cutFirstAt_ && requests_.empty();
            requests_.push_back(r);
        }
        std::string body = from < seconds_ || pastEnd_ == HeaderOnly ? streamedWav(std::max(0.0, seconds_ - from)) : id3Only();
        if (cut) body.resize(std::min(body.size(), wavOffset(*cutFirstAt_)));
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nAccept-Ranges: none\r\n"
                           "Connection: close\r\nTransfer-Encoding: chunked\r\n\r\n";
        if (!sendAll(fd, head) || !sendAll(fd, chunks(body))) return finish(fd, false);
        if (!cut) { // the end: the last chunk; the player reads to it, then hangs up
            if (!sendAll(fd, "0\r\n\r\n")) return finish(fd, false);
            shutdown(fd, SHUT_WR);
            while (!stop_) {
                pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 10) > 0 && read(fd, buf, sizeof buf) <= 0) break;
            }
            return finish(fd, false);
        }
        held_ = true;
        while (!stop_ && !reset_) std::this_thread::sleep_for(5ms);
        finish(fd, true);
    }

    static std::string chunks(const std::string& data) {
        constexpr size_t kChunk = 4096;
        std::string out;
        for (size_t at = 0; at < data.size(); at += kChunk) {
            std::string part = data.substr(at, kChunk);
            char size[16];
            std::snprintf(size, sizeof size, "%zx\r\n", part.size());
            out += size + part + "\r\n";
        }
        return out;
    }

    static bool sendAll(int fd, const std::string& data) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        for (size_t sent = 0; sent < data.size();) {
            ssize_t k = send(fd, data.data() + sent, data.size() - sent, flags);
            if (k <= 0) return false;
            sent += size_t(k);
        }
        return true;
    }

    void finish(int fd, bool reset) {
        if (reset) { // close() then sends a TCP reset instead of the end of the stream
            linger hard{1, 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof hard);
        }
        std::lock_guard lock(mutex_);
        open_.erase(std::find(open_.begin(), open_.end(), fd));
        ::close(fd);
    }

    double seconds_;
    std::optional<double> cutFirstAt_;
    PastEnd pastEnd_;
    int fd_ = -1;
    int port_ = 0;
    mutable std::mutex mutex_; // for the three below
    std::vector<Request> requests_;
    std::vector<int> open_;
    std::vector<std::thread> clients_; // only the accept thread adds, before the destructor joins
    std::atomic<bool> held_{false};
    std::atomic<bool> reset_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Plays until the player has finished or failed, or `limit` has passed. Returns the frames heard.
size_t heardUntilTheEnd(Player& player, Mixer& mixer, std::chrono::seconds limit = 9s) {
    std::vector<float> buf(1024 * AudioStream::kChannels);
    size_t heard = 0;
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!player.finished() && !player.failed() && std::chrono::steady_clock::now() < deadline) {
        size_t got = mixer.content().read(buf.data(), 1024);
        heard += got;
        if (got == 0) std::this_thread::sleep_for(1ms);
    }
    return heard;
}

// No byte of a transcode is asked for with Range (FFmpeg's "bytes=0-" asks for all of it).
void expectNoRangeSeek(const TranscodeServer& server) {
    for (const auto& r : server.requests()) EXPECT_LE(r.rangeStart, 0) << r.target;
}

// Waits, hearing nothing, until a seek has opened the transcode again and the
// audio from before it has been dropped.
void afterTheSeek(const TranscodeServer& server) {
    EXPECT_TRUE(waitFor([&] { return server.requests().size() == 2; })) << "opened again";
    std::this_thread::sleep_for(300ms);
}

const char* const kTranscodePath = "/rest/stream?id=7&format=mp3&maxBitRate=128";

// A seek in a transcode opens it again from the whole second (timeOffset) and
// drops the rest up to the place: the server cannot send byte ranges of it.
TEST_F(PlayerTest, ASeekInATranscodeOpensItAgainWithTimeOffsetNotRange) {
    TranscodeServer server(10);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 0, Transcoded{url + "&timeOffset=", 10}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    ASSERT_TRUE(player.isOpen());
    EXPECT_DOUBLE_EQ(player.duration(), 10) << "the length the server lists";
    player.play();
    consume(player, mixer_, Mixer::kSampleRate); // 1 s
    player.seek(6.5);
    EXPECT_DOUBLE_EQ(player.position(), 6.5);
    afterTheSeek(server);
    size_t heard = 0;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { heard = heardUntilTheEnd(player, mixer_); });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_NEAR(seconds(heard), 3.5, 0.05) << "from 6.5 s to the end";
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u) << log;
    EXPECT_EQ(requests[0].target, kTranscodePath) << "the start, without an offset";
    EXPECT_EQ(requests[1].target, std::string(kTranscodePath) + "&timeOffset=6");
    expectNoRangeSeek(server);
    EXPECT_EQ(log.find("cannot seek"), std::string::npos) << log;
}

TEST_F(PlayerTest, ATranscodeOpenedAtAPlaceStartsThereWithTimeOffset) {
    TranscodeServer server(10);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 4.5, Transcoded{url + "&timeOffset=", 10}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    ASSERT_TRUE(player.isOpen());
    EXPECT_NEAR(player.position(), 4.5, 0.001);
    player.play();
    size_t heard = heardUntilTheEnd(player, mixer_);
    EXPECT_TRUE(player.finished());
    EXPECT_NEAR(seconds(heard), 5.5, 0.05);
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].target, std::string(kTranscodePath) + "&timeOffset=4");
}

// A server without transcodeOffset: the transcode is opened again from its
// start, and what comes before the place is decoded and dropped.
TEST_F(PlayerTest, ATranscodeWithoutAnOffsetUrlSeeksByOpeningItFromTheStart) {
    TranscodeServer server(10);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 0, Transcoded{"", 10}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    player.play();
    consume(player, mixer_, Mixer::kSampleRate);
    player.seek(6.5);
    afterTheSeek(server);
    size_t heard = 0;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { heard = heardUntilTheEnd(player, mixer_); });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_NEAR(seconds(heard), 3.5, 0.05);
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u) << log;
    EXPECT_EQ(requests[1].target, kTranscodePath);
    expectNoRangeSeek(server);
}

// FFmpeg reports the end of a transcode as a read error. Without a length,
// that is the end; it is not retried.
TEST_F(PlayerTest, ATranscodeOfUnknownLengthFinishesAtItsEnd) {
    TranscodeServer server(3);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 0, Transcoded{url + "&timeOffset=", 0}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    EXPECT_DOUBLE_EQ(player.duration(), 0);
    player.play();
    size_t heard = 0;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { heard = heardUntilTheEnd(player, mixer_); });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_NEAR(seconds(heard), 3.0, 0.05);
    EXPECT_EQ(server.requests().size(), 1u) << log;
    EXPECT_EQ(log.find("retrying"), std::string::npos) << log;
}

// The server lists the track as longer than its transcode is: the error at the
// real end looks like a dropout, but the retry there gets no audio, so the
// track has ended. Navidrome answers past the end with a few bytes that are no
// audio (HeaderOnly: a header the open takes, then nothing).
TEST_F(PlayerTest, ATranscodeShorterThanListedFinishesAfterOneRetry) {
    for (auto pastEnd : {TranscodeServer::NoAudio, TranscodeServer::HeaderOnly}) {
        TranscodeServer server(3, std::nullopt, pastEnd);
        Player player(mixer_);
        std::string url = server.url(kTranscodePath);
        ASSERT_TRUE(player.open(url, 0, Transcoded{url + "&timeOffset=", 8}));
        ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
        player.play();
        size_t heard = 0;
        std::string log = stderrOf(dir_ / "stderr.txt", [&] { heard = heardUntilTheEnd(player, mixer_, 5s); });
        EXPECT_TRUE(player.finished()) << pastEnd << log;
        EXPECT_FALSE(player.failed());
        EXPECT_NEAR(seconds(heard), 3.0, 0.05);
        auto requests = server.requests();
        ASSERT_EQ(requests.size(), 2u) << pastEnd << log;
        EXPECT_EQ(requests[1].target, std::string(kTranscodePath) + "&timeOffset=3");
    }
}

// A skip past the real end of a transcode: the server has no audio there, so the track has ended.
TEST_F(PlayerTest, ASeekPastTheEndOfATranscodeEndsIt) {
    TranscodeServer server(3);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 0, Transcoded{url + "&timeOffset=", 8}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    player.play();
    consume(player, mixer_, Mixer::kSampleRate / 2);
    player.seek(6);
    std::string log = stderrOf(dir_ / "stderr.txt", [&] { heardUntilTheEnd(player, mixer_, 5s); });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_NEAR(player.position(), 6, 0.01);
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u) << log;
    EXPECT_EQ(requests[1].target, std::string(kTranscodePath) + "&timeOffset=6");
}

// A dropout in a transcode: the retry is a seek too, so it opens the transcode
// again where the queued audio ends, and nothing is lost or repeated.
TEST_F(PlayerTest, ATranscodeThatBreaksOffIsOpenedAgainWhereItStopped) {
    TranscodeServer server(12, kCutAt);
    Player player(mixer_);
    std::string url = server.url(kTranscodePath);
    ASSERT_TRUE(player.open(url, 0, Transcoded{url + "&timeOffset=", 12}));
    ASSERT_TRUE(waitFor([&] { return !player.opening(); }, 9s));
    player.play();
    size_t heard = 0;
    std::string log = stderrOf(dir_ / "stderr.txt", [&] {
        heard = consume(player, mixer_, Mixer::kSampleRate); // some of what came; the rest is queued
        EXPECT_TRUE(waitFor([&] { return server.held(); })) << "the reply was cut short";
        std::this_thread::sleep_for(200ms);
        server.reset();
        heard += heardUntilTheEnd(player, mixer_);
    });
    EXPECT_TRUE(player.finished()) << log;
    EXPECT_FALSE(player.failed());
    EXPECT_NEAR(seconds(heard), 12.0, 0.05) << log;
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u) << log;
    EXPECT_NE(requests[1].target.find("&timeOffset="), std::string::npos) << requests[1].target;
    expectNoRangeSeek(server);
    EXPECT_NE(log.find("connection is back"), std::string::npos) << log;
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
