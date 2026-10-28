#pragma once

#include <chrono>
#include <condition_variable>
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
    // Extra request headers, one "Name: value" line each. Values can be secrets
    // (a bearer token): implementations never log or print them, and a request
    // that has headers does not follow redirects. A line break in one fails it.
    using Headers = std::vector<std::string>;

    virtual ~Http() = default;
    virtual void get(Id id, std::string url, Headers headers, Handler handler) = 0;
    // Stop request `id` and drop its handler. Best effort: an answer that is
    // already in may still reach the handler, so callers that must not see it
    // (Subsonic) check for themselves.
    virtual void cancel(Id id) = 0;
    // Before the program exits: send no new request and give the queued and
    // running ones up to `deadline` to finish, then abort the rest. Nothing is
    // sent after it. Blocks the caller; the default (a fake) does nothing.
    virtual void finish(std::chrono::milliseconds deadline) { (void)deadline; }
};

// libcurl on one worker thread (curl multi, so several requests run at once).
// The destructor aborts what is in flight and returns at once: it never waits
// for a stuck socket. finish() is the polite way out: it waits, up to a deadline.
// Handlers of requests that did not finish are not called.
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

    void get(Id id, std::string url, Headers headers, Handler handler) override;
    void cancel(Id id) override;
    // Rejects new get()s (their handlers never run), waits until the queued and
    // running requests are answered (their handlers run) or `deadline` has passed,
    // then aborts what is left and stops the worker. The destructor is still safe after it.
    // Call it once, from one thread, never from a handler.
    void finish(std::chrono::milliseconds deadline) override;

private:
    struct Job {
        Id id = 0;
        std::string url;
        Headers headers;
        Handler handler;
    };
    void run();

    Options options_;
    void* multi_ = nullptr; // CURLM*, kept out of the header

    std::mutex mutex_; // guards the three below
    std::deque<Job> queue_;
    std::vector<Id> cancels_;
    bool stop_ = false;      // abort everything and leave
    bool finishing_ = false; // no new requests; leave when nothing is left
    bool exited_ = false;    // the worker is done
    std::condition_variable exitedChanged_;

    std::thread worker_;
};

} // namespace oap
