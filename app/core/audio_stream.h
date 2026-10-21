#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace oap {

// Thread-safe FIFO of interleaved stereo float frames.
// flush() bumps an epoch so a producer that started before the flush cannot
// sneak stale audio in afterwards (used for seeking and cancelling speech).
class AudioStream {
public:
    static constexpr int kChannels = 2;

    explicit AudioStream(size_t capacityFrames);

    uint64_t epoch() const;
    uint64_t flush();
    size_t buffered() const;

    // Returns frames written (may be fewer than count when full), or -1 if epoch is stale.
    long write(const float* frames, size_t count, uint64_t epoch);
    // Blocks until everything is written. Returns false if the epoch went stale or abort() returned true.
    bool writeAll(const float* frames, size_t count, uint64_t epoch, const std::function<bool()>& abort);
    size_t read(float* out, size_t count);

private:
    mutable std::mutex mutex_;
    std::vector<float> buffer_;
    size_t capacity_;
    size_t head_ = 0;
    size_t size_ = 0;
    uint64_t epoch_ = 0;
};

} // namespace oap
