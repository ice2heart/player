#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oap {

// Asynchronous HTTP GET, as Subsonic needs it. Tests stand in a fake.
// URLs carry credentials: implementations never log or print them.
class Http {
public:
    using Id = std::uint64_t;
    struct Response {
        bool reached = false; // false: no answer (refused, timed out, DNS, TLS, too large)
        long status = 0;      // HTTP status when reached
        std::string body;
    };
    // Runs on the client's own thread, never on the caller's.
    using Handler = std::function<void(Response)>;

    virtual ~Http() = default;
    virtual void get(Id id, std::string url, Handler handler) = 0;
    // Stop request `id` and drop its handler. Best effort: an answer that is
    // already in may still reach the handler, so callers that must not see it
    // (Subsonic) check for themselves.
    virtual void cancel(Id id) = 0;
};

// libcurl on one worker thread (curl multi, so several requests run at once).
// The destructor aborts what is in flight and returns at once: it never waits
// for a stuck socket. Handlers of requests that did not finish are not called.
class HttpClient : public Http {
public:
    struct Options {
        std::chrono::seconds connectTimeout{10};
        std::chrono::seconds readTimeout{10}; // no byte received for this long ends the request
        std::chrono::milliseconds totalTimeout{60'000}; // even a server that trickles is cut off here
        std::size_t maxBody = 16 * 1024 * 1024; // larger answers fail (a wrong URL to a big file)
    };

    HttpClient();
    explicit HttpClient(Options options);
    ~HttpClient() override;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void get(Id id, std::string url, Handler handler) override;
    void cancel(Id id) override;

private:
    struct Job {
        Id id = 0;
        std::string url;
        Handler handler;
    };
    void run();

    Options options_;
    void* multi_ = nullptr; // CURLM*, kept out of the header

    std::mutex mutex_; // guards the three below
    std::deque<Job> queue_;
    std::vector<Id> cancels_;
    bool stop_ = false;

    std::thread worker_;
};

} // namespace oap
