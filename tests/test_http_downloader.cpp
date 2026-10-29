// HttpDownloader against sockets on 127.0.0.1 only: a tiny server that answers
// as the test says (whole, in two halves with a pause, cut short, with an error
// status) and logs what it was asked. No internet needed.

#include "net/http_downloader.h"
#include "net/subsonic.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using oap::Downloader;
using oap::HttpDownloader;
using oap::Subsonic;
using Result = Downloader::Result;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

constexpr auto kPollStep = 5ms;
constexpr auto kPatience = 5s;
constexpr auto kQuiet = 150ms; // room for something wrong to happen

int boundSocket(int& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t length = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    port = ntohs(addr.sin_port);
    return fd;
}

int unusedPort() {
    int port = 0;
    close(boundSocket(port));
    return port;
}

// Status line and headers for a body of `length` bytes.
std::string head(int status, std::size_t length, const std::string& type = "audio/flac",
                 const std::string& extra = "") {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(length) + "\r\n" + extra + "Connection: close\r\n\r\n";
}

// What the server sends for one request: `first`, then, after release() when
// `hold` is set, `second`; then it closes the connection.
struct Answer {
    std::string first, second;
    bool hold = false;
};

Answer whole(const std::string& body, int status = 200, const std::string& type = "audio/flac") {
    return {head(status, body.size(), type) + body, "", false};
}

class TestServer {
public:
    using Handler = std::function<Answer(const std::string& request)>;
    explicit TestServer(Handler handler) : handler_(std::move(handler)) {
        listener_ = boundSocket(port_);
        listen(listener_, 8);
        thread_ = std::thread([this] { acceptLoop(); });
    }
    ~TestServer() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        thread_.join();
        for (auto& t : connections_) t.join();
        close(listener_);
    }
    // The base for Subsonic::Config::url.
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    std::vector<std::string> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::size_t requestCount() { return requests().size(); }

private:
    bool stopping() {
        std::lock_guard lock(mutex_);
        return stop_;
    }
    void acceptLoop() {
        while (!stopping()) {
            pollfd fd{listener_, POLLIN, 0};
            if (::poll(&fd, 1, 20) <= 0) continue;
            int connection = accept(listener_, nullptr, nullptr);
            if (connection >= 0) connections_.emplace_back([this, connection] { serve(connection); });
        }
    }
    void serve(int connection) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && !stopping()) {
            pollfd fd{connection, POLLIN, 0};
            if (::poll(&fd, 1, 20) <= 0) continue;
            auto n = read(connection, buffer, sizeof buffer);
            if (n <= 0) break;
            request.append(buffer, std::size_t(n));
        }
        if (request.find("\r\n\r\n") != std::string::npos) {
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(request);
            }
            Answer answer = handler_(request);
            (void)!write(connection, answer.first.data(), answer.first.size());
            if (answer.hold) {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] { return released_ || stop_; });
            }
            if (!stopping()) (void)!write(connection, answer.second.data(), answer.second.size());
        }
        close(connection);
    }

    Handler handler_;
    int listener_ = -1;
    int port_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_ = false;
    bool stop_ = false;
    std::vector<std::string> requests_;
    std::thread thread_;
    std::vector<std::thread> connections_;
};

std::string requestLine(const std::string& request) { return request.substr(0, request.find("\r\n")); }
// The start of "Range: bytes=N-", or "" without a Range header.
std::string rangeOf(const std::string& request) {
    auto at = request.find("Range: bytes=");
    if (at == std::string::npos) return "";
    at += 13;
    return request.substr(at, request.find("\r\n", at) - at);
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}
void spit(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << contents;
}

Subsonic::Config configFor(const std::string& base) {
    Subsonic::Config c;
    c.url = base;
    c.user = "ice";
    c.password = "sesame";
    return c;
}

// What the callbacks of one download got.
struct Outcome {
    int doneCalls = 0;
    Result result = Result::Ok;
    long long bytes = -1, total = -1;
    int progressCalls = 0;
    Downloader::Progress progress() {
        return [this](long long b, long long t) { ++progressCalls, bytes = b, total = t; };
    }
    Downloader::Done done() {
        return [this](Result r) { ++doneCalls, result = r; };
    }
};

class HttpDownloaderTest : public testing::Test {
protected:
    void SetUp() override {
        dir = oap::test::testTempDir();
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override {
        downloader.reset(); // before the server and the folder go
        fs::remove_all(dir);
    }
    void use(TestServer& server, HttpDownloader::Options options = {}) {
        downloader = std::make_unique<HttpDownloader>(
            Subsonic::downloadUrls(configFor(server.base()), [] { return std::string("c19b2d"); }), options);
    }
    template <class F> bool pump(F condition, std::chrono::milliseconds patience = kPatience) {
        auto deadline = Clock::now() + patience;
        while (!condition()) {
            if (Clock::now() > deadline) return false;
            downloader->poll();
            std::this_thread::sleep_for(kPollStep);
        }
        return true;
    }
    template <class F> static bool waitFor(F condition, std::chrono::milliseconds patience = kPatience) {
        auto deadline = Clock::now() + patience;
        while (!condition()) {
            if (Clock::now() > deadline) return false;
            std::this_thread::sleep_for(kPollStep);
        }
        return true;
    }
    // Runs one download to its result.
    Result fetch(const std::string& trackId, const fs::path& dest, long long expectedSize = 0) {
        Outcome outcome;
        downloader->download(trackId, dest.string(), expectedSize, outcome.progress(), outcome.done());
        EXPECT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
        return outcome.result;
    }

    fs::path dir;
    std::unique_ptr<HttpDownloader> downloader;
};

} // namespace

TEST_F(HttpDownloaderTest, AsksForTheDownloadUrlWithTheSameAuthAsAStream) {
    TestServer server([](const std::string&) { return whole("song"); });
    use(server);
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Ok);
    ASSERT_EQ(server.requestCount(), 1u);
    EXPECT_EQ(requestLine(server.requests()[0]),
              "GET /rest/download?u=ice&t=26719a1196d2a940705a59634eb18eab&s=c19b2d&v=1.16.1&c=oap&f=json&id=t1 "
              "HTTP/1.1");
    EXPECT_EQ(rangeOf(server.requests()[0]), "") << "a new file asks for all of it";
}

TEST_F(HttpDownloaderTest, ATranscodeRestartsItsPartWithoutRangeAndIgnoresOriginalSize) {
    TestServer server([](const std::string&) { return Answer{"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\n\r\nnew mp3", {}, false}; });
    use(server);
    fs::path dest = dir / "t1.mp3";
    spit(dest.string() + ".part", "stale partial transcode");
    Outcome outcome;
    downloader->download("t1", dest.string(), 1000000, outcome.progress(), outcome.done(), 128);
    ASSERT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
    EXPECT_EQ(outcome.result, Result::Ok);
    ASSERT_EQ(server.requestCount(), 1u);
    EXPECT_EQ(rangeOf(server.requests()[0]), "");
    EXPECT_NE(requestLine(server.requests()[0]).find("/rest/stream?"), std::string::npos);
    EXPECT_NE(requestLine(server.requests()[0]).find("&maxBitRate=128"), std::string::npos);
    EXPECT_EQ(slurp(dest), "new mp3");
}

TEST_F(HttpDownloaderTest, AFinishedFileIsRenamedFromThePartAndFoldersAreCreated) {
    TestServer server([](const std::string&) { return whole("the whole song"); });
    use(server);
    fs::path dest = dir / "server" / "al1" / "01.flac";
    EXPECT_EQ(fetch("t1", dest), Result::Ok);
    EXPECT_EQ(slurp(dest), "the whole song");
    EXPECT_FALSE(fs::exists(fs::path(dest.string() + ".part")));
}

TEST_F(HttpDownloaderTest, ThePartGrowsAndDestAppearsOnlyWhenTheFileIsComplete) {
    std::string first = "12345", second = "67890";
    TestServer server([&](const std::string&) {
        return Answer{head(200, first.size() + second.size()) + first, second, /*hold=*/true};
    });
    use(server);
    fs::path dest = dir / "t1.flac", part = dir / "t1.flac.part";
    Outcome outcome;
    downloader->download("t1", dest.string(), 0, outcome.progress(), outcome.done());
    ASSERT_TRUE(waitFor([&] { return fs::exists(part) && fs::file_size(part) == first.size(); }));
    EXPECT_FALSE(fs::exists(dest)) << "half a file is not the file";
    ASSERT_TRUE(pump([&] { return outcome.progressCalls > 0; }));
    EXPECT_EQ(outcome.bytes, 5);
    EXPECT_EQ(outcome.total, 10);
    EXPECT_EQ(outcome.doneCalls, 0);
    server.release();
    ASSERT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
    EXPECT_EQ(outcome.result, Result::Ok);
    EXPECT_EQ(slurp(dest), "1234567890");
    EXPECT_FALSE(fs::exists(part));
}

TEST_F(HttpDownloaderTest, TheResultIsReportedOnlyFromPoll) {
    TestServer server([](const std::string&) { return whole("song"); });
    use(server);
    Outcome outcome;
    downloader->download("t1", (dir / "t1.flac").string(), 0, nullptr, outcome.done());
    ASSERT_TRUE(waitFor([&] { return fs::exists(dir / "t1.flac"); }));
    std::this_thread::sleep_for(kQuiet);
    EXPECT_EQ(outcome.doneCalls, 0);
    ASSERT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
    downloader->poll();
    EXPECT_EQ(outcome.doneCalls, 1);
}

TEST_F(HttpDownloaderTest, APartialPartIsContinuedWithARangeRequest) {
    TestServer server([](const std::string& request) {
        return rangeOf(request) == "4-" ? Answer{head(206, 6, "audio/flac", "Content-Range: bytes 4-9/10\r\n") + "efghij", "", false}
                                        : whole("abcdefghij");
    });
    use(server);
    fs::path dest = dir / "t1.flac";
    spit(dir / "t1.flac.part", "abcd");
    Outcome outcome;
    downloader->download("t1", dest.string(), 0, outcome.progress(), outcome.done());
    ASSERT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
    EXPECT_EQ(outcome.result, Result::Ok);
    ASSERT_EQ(server.requestCount(), 1u);
    EXPECT_EQ(rangeOf(server.requests()[0]), "4-");
    EXPECT_EQ(slurp(dest), "abcdefghij");
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part"));
}

TEST_F(HttpDownloaderTest, ProgressOfAContinuedPartCountsTheBytesItAlreadyHad) {
    TestServer server([](const std::string&) {
        return Answer{head(206, 6, "audio/flac", "Content-Range: bytes 4-9/10\r\n") + "ef", "ghij", /*hold=*/true};
    });
    use(server);
    spit(dir / "t1.flac.part", "abcd");
    Outcome outcome;
    downloader->download("t1", (dir / "t1.flac").string(), 0, outcome.progress(), outcome.done());
    ASSERT_TRUE(pump([&] { return outcome.progressCalls > 0; }));
    EXPECT_EQ(outcome.bytes, 6);
    EXPECT_EQ(outcome.total, 10);
    server.release();
    ASSERT_TRUE(pump([&] { return outcome.doneCalls > 0; }));
    EXPECT_EQ(slurp(dir / "t1.flac"), "abcdefghij");
}

TEST_F(HttpDownloaderTest, AServerThatIgnoresRangeAndAnswers200StartsTheFileOver) {
    TestServer server([](const std::string&) { return whole("abcdefghij"); });
    use(server);
    spit(dir / "t1.flac.part", "abcd");
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Ok);
    EXPECT_EQ(rangeOf(server.requests()[0]), "4-");
    EXPECT_EQ(slurp(dir / "t1.flac"), "abcdefghij") << "not abcd + abcdefghij";
}

TEST_F(HttpDownloaderTest, APartThatIsNotAPrefixGets416AndTheFileStartsOver) {
    TestServer server([](const std::string& request) {
        return rangeOf(request).empty() ? whole("abcdefghij")
                                        : Answer{head(416, 0, "text/plain", "Content-Range: bytes */10\r\n"), "", false};
    });
    use(server);
    spit(dir / "t1.flac.part", std::string(20, 'x')); // longer than the file
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Ok);
    ASSERT_EQ(server.requestCount(), 2u);
    EXPECT_EQ(rangeOf(server.requests()[0]), "20-");
    EXPECT_EQ(rangeOf(server.requests()[1]), "");
    EXPECT_EQ(slurp(dir / "t1.flac"), "abcdefghij");
}

TEST_F(HttpDownloaderTest, ABrokenConnectionKeepsThePartAndTheNextTryContinuesIt) {
    std::atomic<int> calls{0};
    TestServer server([&](const std::string& request) {
        if (calls++ == 0) return Answer{head(200, 10) + "12345", "", false}; // cut after half
        EXPECT_EQ(rangeOf(request), "5-");
        return Answer{head(206, 5, "audio/flac", "Content-Range: bytes 5-9/10\r\n") + "67890", "", false};
    });
    use(server);
    fs::path dest = dir / "t1.flac";
    EXPECT_EQ(fetch("t1", dest), Result::Unreachable);
    EXPECT_FALSE(fs::exists(dest));
    EXPECT_EQ(slurp(dir / "t1.flac.part"), "12345");
    EXPECT_EQ(fetch("t1", dest), Result::Ok);
    EXPECT_EQ(slurp(dest), "1234567890");
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part"));
}

TEST_F(HttpDownloaderTest, AServerThatCannotBeReachedIsUnreachableAndWritesNothing) {
    Subsonic::Config config = configFor("http://127.0.0.1:" + std::to_string(unusedPort()));
    downloader = std::make_unique<HttpDownloader>(Subsonic::downloadUrls(config));
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Unreachable);
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part"));
}

TEST_F(HttpDownloaderTest, AServerThatStopsSendingTimesOutAndTheBytesThatCameAreKept) {
    TestServer server([](const std::string&) { return Answer{head(200, 10) + "1", "234567890", /*hold=*/true}; });
    HttpDownloader::Options options;
    options.readTimeout = 1s;
    use(server, options);
    auto start = Clock::now();
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Unreachable);
    EXPECT_LT(Clock::now() - start, 4s);
    EXPECT_EQ(slurp(dir / "t1.flac.part"), "1");
}

TEST_F(HttpDownloaderTest, AnHttpErrorIsAServerErrorAndLeavesThePartAsItWas) {
    TestServer server([](const std::string&) { return whole("not found", 404, "application/octet-stream"); });
    use(server);
    spit(dir / "t1.flac.part", "abcd");
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Server);
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
    EXPECT_EQ(slurp(dir / "t1.flac.part"), "abcd");
}

TEST_F(HttpDownloaderTest, ASubsonicErrorDocumentWithStatus200IsAServerErrorNotAnAudioFile) {
    TestServer server([](const std::string&) {
        return whole("{\"subsonic-response\":{\"status\":\"failed\"}}", 200, "application/json");
    });
    use(server);
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Server);
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part"));
}

TEST_F(HttpDownloaderTest, AnUnwritableDestinationIsADiskError) {
    TestServer server([](const std::string&) { return whole("song"); });
    use(server);
    spit(dir / "blocker", "a file where a folder should be");
    EXPECT_EQ(fetch("t1", dir / "blocker" / "al1" / "t1.flac"), Result::Disk);
}

TEST_F(HttpDownloaderTest, ADestinationWhosePartCannotBeOpenedIsADiskError) {
    TestServer server([](const std::string&) { return whole("song"); });
    use(server);
    fs::create_directories(dir / "t1.flac.part"); // a folder where the part should go
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Disk);
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
}

TEST_F(HttpDownloaderTest, ACancelledDownloadNeverCallsBackAndTheTransferStops) {
    TestServer server([](const std::string&) { return Answer{head(200, 10) + "12345", "67890", /*hold=*/true}; });
    use(server);
    Outcome cancelled, next;
    auto id = downloader->download("t1", (dir / "t1.flac").string(), 0, cancelled.progress(), cancelled.done());
    ASSERT_TRUE(waitFor([&] { return fs::exists(dir / "t1.flac.part") && fs::file_size(dir / "t1.flac.part") == 5; }));
    downloader->cancel(id);
    server.release(); // the rest comes now; without the cancel the file would finish
    std::this_thread::sleep_for(kQuiet);
    downloader->poll();
    EXPECT_EQ(cancelled.doneCalls, 0);
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
    EXPECT_EQ(slurp(dir / "t1.flac.part"), "12345") << "the transfer stopped and the part is kept";
    // The worker is free again.
    downloader->download("t2", (dir / "t2.flac").string(), 0, nullptr, next.done());
    ASSERT_TRUE(pump([&] { return next.doneCalls > 0; }));
    EXPECT_EQ(cancelled.doneCalls, 0);
}

namespace {
// Every file and folder below `root`, as relative names.
std::set<std::string> listing(const fs::path& root) {
    std::set<std::string> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        names.insert(fs::relative(it->path(), root).string());
    return names;
}
} // namespace

// A worker that is slow to get to its next file operation, as a busy disk makes it: the test holds
// it before (`holdingFiles` false) or inside the operation until it says go.
struct FileStepGate {
    explicit FileStepGate(bool inside) : inside(inside) {}
    HttpDownloader::Options options() {
        HttpDownloader::Options o;
        o.onFileStep = [this](bool holding) {
            if (holding != inside) return;
            std::unique_lock lock(mutex);
            if (++reached == 1) {
                changed.notify_all();
                changed.wait(lock, [this] { return go; });
            }
        };
        return o;
    }
    bool waitForWorker() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, kPatience, [this] { return reached > 0; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            go = true;
        }
        changed.notify_all();
    }
    bool inside;
    std::mutex mutex;
    std::condition_variable changed;
    int reached = 0;
    bool go = false;
};

TEST_F(HttpDownloaderTest, ADownloadCancelledWhileTheWorkerIsAboutToCreateItsFolderNeverCreatesIt) {
    TestServer server([](const std::string&) { return whole("song"); });
    FileStepGate gate(/*inside=*/false);
    use(server, gate.options());
    auto id = downloader->download("t1", (dir / "album" / "t1.flac").string(), 0, nullptr, nullptr);
    ASSERT_TRUE(gate.waitForWorker());
    downloader->cancel(id); // the caller deletes the folder now, or has no use for it
    gate.release();
    Outcome next; // the worker is done with the cancelled job when it finishes this one
    downloader->download("t2", (dir / "t2.flac").string(), 0, nullptr, next.done());
    ASSERT_TRUE(pump([&] { return next.doneCalls > 0; }));
    EXPECT_FALSE(fs::exists(dir / "album")) << "no folder after cancel() returned";
    EXPECT_EQ(server.requestCount(), 1u) << "and no request for the cancelled one";
}

TEST_F(HttpDownloaderTest, ADownloadCancelledWhileTheWorkerIsAboutToOpenItsPartNeverOpensIt) {
    TestServer server([](const std::string&) { return Answer{head(200, 10), "1234567890", /*hold=*/true}; });
    FileStepGate gate(/*inside=*/false);
    HttpDownloader::Options options = gate.options();
    auto inner = options.onFileStep;
    std::atomic<int> steps{0};
    options.onFileStep = [&](bool holding) { // let the folder be made, hold the second step: the open of the part
        if (!holding && steps++ == 0) return;
        inner(holding);
    };
    use(server, options);
    auto id = downloader->download("t1", (dir / "t1.flac").string(), 0, nullptr, nullptr);
    ASSERT_TRUE(waitFor([&] { return server.requestCount() == 1; }));
    server.release(); // the body comes: the worker is about to open the part
    ASSERT_TRUE(gate.waitForWorker());
    downloader->cancel(id);
    gate.release();
    std::this_thread::sleep_for(kQuiet);
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part")) << "nothing is opened after cancel() returned";
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
}

TEST_F(HttpDownloaderTest, CancelWaitsForTheFileOperationInFlightAndNothingFollowsIt) {
    TestServer server([](const std::string&) { return whole("song"); });
    FileStepGate gate(/*inside=*/true);
    use(server, gate.options());
    auto id = downloader->download("t1", (dir / "album" / "t1.flac").string(), 0, nullptr, nullptr);
    ASSERT_TRUE(gate.waitForWorker()); // inside the creation of the folder
    std::atomic<bool> returned{false};
    std::thread canceller([&] {
        downloader->cancel(id);
        returned = true;
    });
    std::this_thread::sleep_for(kQuiet);
    EXPECT_FALSE(returned) << "the operation is still running";
    gate.release();
    canceller.join();
    EXPECT_TRUE(returned);
    std::set<std::string> atCancel = listing(dir);
    std::this_thread::sleep_for(kQuiet);
    EXPECT_EQ(listing(dir), atCancel) << "nothing is created once cancel() has returned";
    EXPECT_EQ(server.requestCount(), 0u) << "the transfer never began";
}

TEST_F(HttpDownloaderTest, TheLengthTheServerSendsDecidesNotTheExpectedSize) {
    TestServer server([](const std::string&) { return whole("abc"); });
    use(server);
    EXPECT_EQ(fetch("t1", dir / "a.flac", 100), Result::Ok) << "a stale size in the listing";
    EXPECT_EQ(slurp(dir / "a.flac"), "abc");
    EXPECT_EQ(fetch("t1", dir / "b.flac", 2), Result::Ok);
}

TEST_F(HttpDownloaderTest, ABodyOfUnknownLengthIsComparedWithTheExpectedSize) {
    TestServer server([](const std::string&) {
        return Answer{"HTTP/1.1 200 X\r\nContent-Type: audio/flac\r\nConnection: close\r\n\r\nabc", "", false};
    });
    use(server);
    EXPECT_EQ(fetch("t1", dir / "short.flac", 10), Result::Unreachable) << "cut short";
    EXPECT_FALSE(fs::exists(dir / "short.flac"));
    EXPECT_EQ(slurp(dir / "short.flac.part"), "abc") << "kept to be continued";
    EXPECT_EQ(fetch("t2", dir / "exact.flac", 3), Result::Ok);
    EXPECT_EQ(slurp(dir / "exact.flac"), "abc");
    EXPECT_EQ(fetch("t3", dir / "unknown.flac", 0), Result::Ok) << "no size to compare with: accepted";
    EXPECT_EQ(fetch("t4", dir / "stale.flac", 2), Result::Ok) << "longer than listed cannot be a cut one";
    EXPECT_EQ(slurp(dir / "stale.flac"), "abc");
}

TEST_F(HttpDownloaderTest, AContentRangeTotalOfZeroIsNotTakenForTheSize) {
    TestServer server([](const std::string&) {
        return Answer{head(206, 2, "audio/flac", "Content-Range: bytes 4-5/0\r\n") + "ef", "", false};
    });
    use(server);
    spit(dir / "t1.flac.part", "abcd");
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::Ok) << "a total of 0 is nonsense: the Content-Length decides";
    EXPECT_EQ(slurp(dir / "t1.flac"), "abcdef");
}

TEST_F(HttpDownloaderTest, TheTotalOfContentRangeDecidesForAContinuedPart) {
    TestServer server([](const std::string&) {
        return Answer{head(206, 2, "audio/flac", "Content-Range: bytes 4-5/10\r\n") + "ef", "", false};
    });
    use(server);
    spit(dir / "t1.flac.part", "abcd");
    EXPECT_EQ(fetch("t1", dir / "t1.flac", 6), Result::Unreachable) << "6 bytes of 10; the expected 6 does not matter";
    EXPECT_FALSE(fs::exists(dir / "t1.flac"));
    EXPECT_EQ(slurp(dir / "t1.flac.part"), "abcdef");
}

TEST_F(HttpDownloaderTest, ADownloadCancelledBeforeItStartedNeverRuns) {
    TestServer server([&](const std::string& request) {
        return request.find("id=t1") != std::string::npos ? Answer{head(200, 4) + "so", "ng", /*hold=*/true}
                                                          : whole("song");
    });
    use(server);
    Outcome first, second, third;
    downloader->download("t1", (dir / "t1.flac").string(), 0, nullptr, first.done()); // holds the worker
    auto queued = downloader->download("t2", (dir / "t2.flac").string(), 0, nullptr, second.done());
    downloader->download("t3", (dir / "t3.flac").string(), 0, nullptr, third.done());
    ASSERT_TRUE(waitFor([&] { return server.requestCount() == 1; }));
    downloader->cancel(queued);
    server.release();
    ASSERT_TRUE(pump([&] { return third.doneCalls > 0; }));
    EXPECT_EQ(first.doneCalls, 1);
    EXPECT_EQ(second.doneCalls, 0);
    EXPECT_FALSE(fs::exists(dir / "t2.flac"));
    EXPECT_EQ(server.requestCount(), 2u) << "t1 and t3 were asked for, t2 never";
}

TEST_F(HttpDownloaderTest, DownloadsRunOneAfterAnotherInTheOrderQueued) {
    TestServer server([](const std::string& request) {
        return request.find("id=t1") != std::string::npos ? Answer{head(200, 4) + "so", "ng", /*hold=*/true}
                                                          : whole("song");
    });
    use(server);
    std::vector<std::string> order;
    auto note = [&](std::string name) { return [&order, name](Result) { order.push_back(name); }; };
    auto start = Clock::now();
    downloader->download("t1", (dir / "1.flac").string(), 0, nullptr, note("t1"));
    downloader->download("t2", (dir / "2.flac").string(), 0, nullptr, note("t2"));
    downloader->download("t3", (dir / "3.flac").string(), 0, nullptr, note("t3"));
    EXPECT_LT(Clock::now() - start, 1s) << "download() does not wait for the transfer";
    ASSERT_TRUE(waitFor([&] { return server.requestCount() == 1; }));
    std::this_thread::sleep_for(kQuiet);
    EXPECT_EQ(server.requestCount(), 1u) << "the second waits for the first";
    server.release();
    ASSERT_TRUE(pump([&] { return order.size() == 3; }));
    EXPECT_EQ(order, (std::vector<std::string>{"t1", "t2", "t3"}));
}

TEST_F(HttpDownloaderTest, TheDestructorDoesNotWaitForAStuckTransfer) {
    TestServer server([](const std::string&) { return Answer{head(200, 10) + "12345", "67890", /*hold=*/true}; });
    use(server);
    Outcome outcome;
    downloader->download("t1", (dir / "t1.flac").string(), 0, nullptr, outcome.done());
    ASSERT_TRUE(waitFor([&] { return server.requestCount() == 1; }));
    auto start = Clock::now();
    downloader.reset();
    EXPECT_LT(Clock::now() - start, 500ms) << "the wake-up ends the poll wait, not its one second timeout";
    EXPECT_EQ(outcome.doneCalls, 0);
}

TEST_F(HttpDownloaderTest, AnEmptyUrlIsNotConfigured) {
    downloader = std::make_unique<HttpDownloader>(Subsonic::downloadUrls(configFor("")));
    EXPECT_EQ(fetch("t1", dir / "t1.flac"), Result::NotConfigured);
    EXPECT_FALSE(fs::exists(dir / "t1.flac.part"));
}

TEST_F(HttpDownloaderTest, ACallbackThatCancelsAnotherDownloadStopsItsCallback) {
    TestServer server([](const std::string&) { return whole("song"); });
    use(server);
    Outcome second;
    Downloader::RequestId secondId = 0;
    int firstCalls = 0;
    // Both finish before one poll() sees them; the first one's callback cancels the second.
    downloader->download("t1", (dir / "1.flac").string(), 0, nullptr, [&](Result) {
        ++firstCalls;
        downloader->cancel(secondId);
    });
    secondId = downloader->download("t2", (dir / "2.flac").string(), 0, nullptr, second.done());
    ASSERT_TRUE(waitFor([&] { return fs::exists(dir / "2.flac"); }));
    std::this_thread::sleep_for(kQuiet); // both results are waiting for the poll
    ASSERT_TRUE(pump([&] { return firstCalls > 0; }));
    downloader->poll();
    EXPECT_EQ(second.doneCalls, 0);
}
