#pragma once

#include "core/services.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oap {

// The Downloader of services.h over HTTP with libcurl, on its own worker thread
// (apart from HttpClient's, so a long download never delays a list request).
// Tracks download one at a time, in the order they were queued.
//
// The URL of a track comes from `UrlSource` (for Subsonic: Subsonic::downloadUrls()),
// called on the UI thread in download(). URLs hold credentials: nothing here
// logs, prints or reports one, not even in an error. The `.part` is continued
// with a Range request; a server that answers 200 to it (no ranges) or 416
// (the `.part` is not a prefix of its file) gets the file from its start. Only
// audio is kept: an HTTP error, or an error document (a Subsonic error comes as
// text or JSON with status 200) is Result::Server and leaves the `.part` as it was.
// A body that ends early when the server said how long it is, is Unreachable;
// a body of unknown length counts as complete when the connection closes.
//
// The destructor aborts the transfer and returns at once, never waiting for a
// stuck socket; the `.part` stays, and no callback runs.
class HttpDownloader : public Downloader {
public:
    using UrlSource = std::function<std::string(const std::string& trackId)>; // empty: not configured
    struct Options {
        std::chrono::seconds connectTimeout{10};
        std::chrono::seconds readTimeout{10}; // no byte received for this long ends the transfer
    };

    explicit HttpDownloader(UrlSource urls);
    HttpDownloader(UrlSource urls, Options options);
    ~HttpDownloader() override;
    HttpDownloader(const HttpDownloader&) = delete;
    HttpDownloader& operator=(const HttpDownloader&) = delete;

    bool configured() const override { return true; }
    RequestId download(const std::string& trackId, const std::string& dest, Progress progress, Done done) override;
    void cancel(RequestId id) override;
    void poll() override;

private:
    struct Job {
        RequestId id = 0;
        std::string url, dest;
    };
    struct Callbacks {
        Progress progress;
        Done done;
    };
    struct Fetch;
    void run();
    bool shouldStop(); // stop or abort of the current job asked for
    // nullopt: stopped by cancel or the destructor.
    std::optional<Result> transfer(const Job& job);

    UrlSource urls_;
    Options options_;
    void* multi_ = nullptr; // CURLM*, kept out of the header

    RequestId lastId_ = 0;
    std::map<RequestId, Callbacks> waiting_; // UI thread only

    std::mutex mutex_; // guards everything below
    std::condition_variable wake_;
    std::deque<Job> queue_;
    RequestId current_ = 0; // the job being fetched, 0 if none
    bool abort_ = false;    // cancel() of current_
    bool stop_ = false;
    std::map<RequestId, std::pair<long long, long long>> progress_; // latest, until poll()
    std::vector<std::pair<RequestId, Result>> finished_;

    std::thread worker_;
};

} // namespace oap
