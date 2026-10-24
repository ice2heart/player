// HttpClient against sockets on 127.0.0.1 only: a port nobody listens on, a tiny
// server that answers, and one that takes the request and never answers. Also
// Subsonic end to end over the real client. No internet needed.

#include "net/http_client.h"
#include "net/subsonic.h"
#include "subsonic_fixtures.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using oap::Http;
using oap::HttpClient;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kPollStep = 5ms;
constexpr auto kPatience = 5s; // longest a test waits for something that should happen

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

// A port with nothing listening: connecting to it is refused at once.
int unusedPort() {
    int port = 0;
    close(boundSocket(port));
    return port;
}

std::string httpResponse(int status, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// Takes one connection on 127.0.0.1 and reads the request. Then it sends
// `response` at once, or, when `hold` is set, only after release() (never, if
// release() is not called). With `trickle` it sends one byte of it every
// kTrickleStep, a server that is alive but far too slow.
constexpr auto kTrickleStep = 50ms;
class LocalServer {
public:
    explicit LocalServer(std::string response, bool hold = false, bool trickle = false)
        : response_(std::move(response)), trickle_(trickle), released_(!hold) {
        listener_ = boundSocket(port_);
        listen(listener_, 4);
        thread_ = std::thread([this] { serve(); });
    }
    ~LocalServer() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        thread_.join();
        close(listener_);
    }
    std::string url(const std::string& path = "/") const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    bool requestSeen() const { return requestSeen_; }
    std::string requestLine() {
        std::lock_guard lock(mutex_);
        return requestLine_;
    }

private:
    bool stopping() {
        std::lock_guard lock(mutex_);
        return stop_;
    }
    void serve() {
        int connection = -1;
        while (connection < 0 && !stopping()) {
            pollfd fd{listener_, POLLIN, 0};
            if (::poll(&fd, 1, 20) > 0) connection = accept(listener_, nullptr, nullptr);
        }
        if (connection < 0) return;
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && !stopping()) {
            pollfd fd{connection, POLLIN, 0};
            if (::poll(&fd, 1, 20) <= 0) continue;
            auto n = read(connection, buffer, sizeof buffer);
            if (n <= 0) break;
            request.append(buffer, std::size_t(n));
        }
        {
            std::unique_lock lock(mutex_);
            requestLine_ = request.substr(0, request.find("\r\n"));
            requestSeen_ = true;
            changed_.wait(lock, [this] { return released_ || stop_; });
            if (released_ && !stop_) {
                lock.unlock();
                if (!trickle_) (void)!write(connection, response_.data(), response_.size());
                for (std::size_t i = 0; trickle_ && i < response_.size() && !stopping(); ++i) {
                    if (write(connection, &response_[i], 1) != 1) break;
                    std::this_thread::sleep_for(kTrickleStep);
                }
            }
        }
        close(connection);
    }

    std::string response_;
    bool trickle_;
    int listener_ = -1;
    int port_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_;
    bool stop_ = false;
    std::atomic<bool> requestSeen_{false};
    std::string requestLine_;
    std::thread thread_;
};

// What a handler got, filled on the client's thread.
struct Outcome {
    std::mutex mutex;
    std::optional<Http::Response> response;
    Http::Handler handler() {
        return [this](Http::Response r) {
            std::lock_guard lock(mutex);
            response = std::move(r);
        };
    }
    bool done() {
        std::lock_guard lock(mutex);
        return response.has_value();
    }
};

template <class F> bool waitFor(F condition, std::chrono::milliseconds patience = kPatience) {
    auto deadline = Clock::now() + patience;
    while (!condition()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(kPollStep);
    }
    return true;
}

} // namespace

TEST(HttpClient, ARefusedConnectionIsNotReached) {
    HttpClient client;
    Outcome outcome;
    client.get(1, "http://127.0.0.1:" + std::to_string(unusedPort()) + "/", outcome.handler());
    ASSERT_TRUE(waitFor([&] { return outcome.done(); }));
    EXPECT_FALSE(outcome.response->reached);
}

TEST(HttpClient, ReturnsStatusAndBody) {
    LocalServer server(httpResponse(404, "{\"x\":1}"));
    HttpClient client;
    Outcome outcome;
    client.get(1, server.url("/rest/ping?a=1"), outcome.handler());
    ASSERT_TRUE(waitFor([&] { return outcome.done(); }));
    EXPECT_TRUE(outcome.response->reached);
    EXPECT_EQ(outcome.response->status, 404);
    EXPECT_EQ(outcome.response->body, "{\"x\":1}");
    EXPECT_EQ(server.requestLine(), "GET /rest/ping?a=1 HTTP/1.1");
}

TEST(HttpClient, AServerThatStopsAnsweringTimesOut) {
    LocalServer server("", /*hold=*/true);
    HttpClient::Options options;
    options.readTimeout = 1s;
    HttpClient client(options);
    Outcome outcome;
    auto start = Clock::now();
    client.get(1, server.url(), outcome.handler());
    ASSERT_TRUE(waitFor([&] { return outcome.done(); }));
    EXPECT_FALSE(outcome.response->reached);
    EXPECT_LT(Clock::now() - start, 4s);
}

TEST(HttpClient, AnAnswerThatTricklesInTooSlowlyTimesOut) {
    // A byte every 50 ms passes any "no data for N seconds" check; the total limit ends it.
    LocalServer server(httpResponse(200, std::string(1000, 'x')), /*hold=*/false, /*trickle=*/true);
    HttpClient::Options options;
    options.totalTimeout = 500ms;
    HttpClient client(options);
    Outcome outcome;
    client.get(1, server.url(), outcome.handler());
    ASSERT_TRUE(waitFor([&] { return outcome.done(); }));
    EXPECT_FALSE(outcome.response->reached);
}

TEST(HttpClient, ATooLargeAnswerFails) {
    LocalServer server(httpResponse(200, std::string(100, 'x')));
    HttpClient::Options options;
    options.maxBody = 50;
    HttpClient client(options);
    Outcome outcome;
    client.get(1, server.url(), outcome.handler());
    ASSERT_TRUE(waitFor([&] { return outcome.done(); }));
    EXPECT_FALSE(outcome.response->reached);
}

TEST(HttpClient, ACancelledRequestsHandlerDoesNotRun) {
    LocalServer held(httpResponse(200, "late"), /*hold=*/true);
    LocalServer quick(httpResponse(200, "quick"));
    HttpClient client;
    Outcome cancelled, other;
    client.get(1, held.url(), cancelled.handler());
    ASSERT_TRUE(waitFor([&] { return held.requestSeen(); }));
    client.cancel(1);
    held.release(); // the answer comes now; without the cancel the handler would get it
    client.get(2, quick.url(), other.handler());
    ASSERT_TRUE(waitFor([&] { return other.done(); }));
    std::this_thread::sleep_for(100ms); // room for a wrong late handler call
    EXPECT_FALSE(cancelled.done());
}

TEST(HttpClient, ANewRequestWithTheIdOfOneInFlightReplacesIt) {
    LocalServer held(httpResponse(200, "old"), /*hold=*/true);
    LocalServer quick(httpResponse(200, "new"));
    HttpClient client;
    Outcome first, second;
    client.get(1, held.url(), first.handler());
    ASSERT_TRUE(waitFor([&] { return held.requestSeen(); }));
    client.get(1, quick.url(), second.handler());
    ASSERT_TRUE(waitFor([&] { return second.done(); }));
    EXPECT_EQ(second.response->body, "new");
    held.release(); // the old request's answer comes now and must go nowhere
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(first.done());
}

TEST(HttpClient, DestructorDoesNotWaitForAStuckRequest) {
    LocalServer server("", /*hold=*/true); // takes the request, never answers
    Outcome outcome;
    auto client = std::make_unique<HttpClient>();
    client->get(1, server.url(), outcome.handler());
    ASSERT_TRUE(waitFor([&] { return server.requestSeen(); }));
    auto start = Clock::now();
    client.reset();
    EXPECT_LT(Clock::now() - start, 500ms);
    EXPECT_FALSE(outcome.done());
}

TEST(SubsonicOverHttp, PingThroughALocalServer) {
    LocalServer server(httpResponse(200, oap::test::subsonicFixture("ping.json")));
    oap::Subsonic::Config config;
    config.url = server.url("/music");
    config.user = "ice";
    config.password = "sesame";
    oap::Subsonic subsonic(config);
    int calls = 0;
    oap::MusicServer::ServerInfo info;
    subsonic.ping([&](oap::MusicServer::Error error, oap::MusicServer::ServerInfo i) {
        ++calls;
        EXPECT_EQ(error, oap::MusicServer::Error::None);
        info = i;
    });
    ASSERT_TRUE(waitFor([&] {
        subsonic.poll(); // the UI thread's loop
        return calls > 0;
    }));
    EXPECT_EQ(info.type, "navidrome");
    EXPECT_EQ(info.version, "0.64.2");
    auto line = server.requestLine();
    EXPECT_EQ(line.rfind("GET /music/rest/ping?u=ice&t=", 0), 0u) << line;
    EXPECT_EQ(line.find("sesame"), std::string::npos);
}
