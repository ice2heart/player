#pragma once

#include "core/services.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace oap {

// LibraryScanner on its own thread. Jobs run one after the other in the order of scan().
// The worker walks the roots with scanLibrary(), so it lists the same files as the menus,
// skips the cache folder, and reads the tags (through the TagReader, which must outlive
// the scanner) only of files that are not in `previous` with the same size and file time.
// A file that is not a regular file is never opened (a FIFO would block the read).
//
// cancel() never waits: a scan that has not started is dropped, one that runs stops after
// the file it is reading, and its callback never runs. The destructor does the same and then
// joins the thread, so it waits for one tag read at most (a file on a hung mount can delay it).
class ThreadedScanner : public LibraryScanner {
public:
    explicit ThreadedScanner(TagReader& tags);
    ~ThreadedScanner() override;
    ThreadedScanner(const ThreadedScanner&) = delete;
    ThreadedScanner& operator=(const ThreadedScanner&) = delete;

    RequestId scan(ScanRequest request, Done done) override;
    void cancel(RequestId id) override;
    void poll() override;

private:
    struct Job {
        RequestId id;
        ScanRequest request;
    };
    struct Result {
        RequestId id;
        std::vector<IndexItem> items;
    };

    void work();
    bool cancelled(RequestId id);

    TagReader& tags_;
    RequestId lastId_ = 0;                   // UI thread
    std::vector<std::pair<RequestId, Done>> callbacks_; // UI thread: scans that may still be delivered

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::vector<Result> results_; // for the next poll()
    RequestId running_ = 0;       // the job being scanned, 0: none
    bool runningCancelled_ = false;
    bool stop_ = false;
    std::thread worker_; // last: starts when everything above exists
};

} // namespace oap
