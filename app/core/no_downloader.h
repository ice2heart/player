#pragma once

#include "core/services.h"

#include <utility>
#include <vector>

namespace oap {

// The Downloader of a front end that has no server to fetch from: configured()
// is false, and every download finishes with NotConfigured from the next poll(),
// like NoMusicServer. Nothing is written.
class NoDownloader : public Downloader {
public:
    bool configured() const override { return false; }
    RequestId download(const std::string& trackId, const std::string& dest, long long expectedSize, Progress progress,
                      Done done, int bitrate = 0) override;
    void cancel(RequestId id) override;
    void poll() override;

private:
    RequestId lastId_ = 0;
    std::vector<std::pair<RequestId, Done>> ready_;   // for the next poll()
    std::vector<std::pair<RequestId, Done>> running_; // the batch poll() is running
};

} // namespace oap
