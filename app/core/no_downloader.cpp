#include "core/no_downloader.h"

#include <algorithm>

namespace oap {

Downloader::RequestId NoDownloader::download(const std::string&, const std::string&, long long, Progress,
                                          Done done, int) {
    ready_.emplace_back(++lastId_, std::move(done));
    return lastId_;
}

void NoDownloader::cancel(RequestId id) {
    std::erase_if(ready_, [id](const auto& r) { return r.first == id; });
    std::erase_if(running_, [id](const auto& r) { return r.first == id; });
}

// A callback may queue a new download (finished on the next poll) or cancel one
// still to run in this batch, so the batch is a member cancel() can reach.
void NoDownloader::poll() {
    running_ = std::move(ready_);
    ready_.clear();
    while (!running_.empty()) {
        Done done = std::move(running_.front().second);
        running_.erase(running_.begin());
        if (done) done(Result::NotConfigured);
    }
}

} // namespace oap
