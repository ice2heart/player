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
// Complete means the length the server says (Content-Length; for a continued
// `.part` the total of Content-Range): a body that ends before it is Unreachable.
// A server that says no length is compared with the `expectedSize` given to
// download(), if that is not 0; a shorter body is Unreachable and the `.part` stays
// (continued by the next try). A longer one is accepted: it cannot be a cut one, so
// the size the listing gave is stale.
//
// cancel() returns only after the worker has left any file operation it was in
// (creating the folders, opening or writing the `.part`, the rename): it waits
// for one write at most. From then on that job never creates or changes a file or
// folder, so the caller may delete the folder at once.
//
// The destructor aborts the transfer and returns at once, never waiting for a
// stuck socket; the `.part` stays, and no callback runs.
class HttpDownloader : public Downloader {
public:
    using UrlSource = std::function<std::string(const std::string& trackId)>; // empty: not configured
    struct Options {
        std::chrono::seconds connectTimeout{10};
        std::chrono::seconds readTimeout{10}; // no byte received for this long ends the transfer
        // For tests only: called on the worker thread around every file operation, with false before
        // it asks for the right to touch files and with true once it has it (and is going to).
        std::function<void(bool holdingFiles)> onFileStep;
    };

    explicit HttpDownloader(UrlSource urls);
    HttpDownloader(UrlSource urls, Options options);
    ~HttpDownloader() override;
    HttpDownloader(const HttpDownloader&) = delete;
    HttpDownloader& operator=(const HttpDownloader&) = delete;

    bool configured() const override { return true; }
    RequestId download(const std::string& trackId, const std::string& dest, long long expectedSize, Progress progress,
                       Done done) override;
    void cancel(RequestId id) override;
    void poll() override;

private:
    struct Job {
        RequestId id = 0;
        std::string url, dest;
        long long expectedSize = 0;
    };
    struct Callbacks {
        Progress progress;
        Done done;
    };
    struct Fetch;
    void run();
    bool shouldStop(); // stop or abort of the current job asked for
    // Runs one file operation unless the job was cancelled (or the downloader is going away); false then.
    bool fileStep(const std::function<void()>& operation);
    // nullopt: stopped by cancel or the destructor.
    std::optional<Result> transfer(const Job& job);

    UrlSource urls_;
    Options options_;
    void* multi_ = nullptr; // CURLM*, kept out of the header

    RequestId lastId_ = 0;
    std::map<RequestId, Callbacks> waiting_; // UI thread only

    // Held by the worker for each file operation (which first checks shouldStop()), and
    // taken by cancel() once after it set abort_: when it has it, no operation is running
    // and none will start. Taken before mutex_, never while holding it.
    std::mutex fileMutex_;

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
