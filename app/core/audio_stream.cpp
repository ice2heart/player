#include "audio_stream.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace oap {

AudioStream::AudioStream(size_t capacityFrames)
    : buffer_(capacityFrames * kChannels), capacity_(capacityFrames) {}

uint64_t AudioStream::epoch() const {
    std::lock_guard lock(mutex_);
    return epoch_;
}

uint64_t AudioStream::flush() {
    std::lock_guard lock(mutex_);
    head_ = 0;
    size_ = 0;
    return ++epoch_;
}

size_t AudioStream::buffered() const {
    std::lock_guard lock(mutex_);
    return size_;
}

long AudioStream::write(const float* frames, size_t count, uint64_t epoch) {
    std::lock_guard lock(mutex_);
    if (epoch != epoch_) return -1;
    size_t n = std::min(count, capacity_ - size_);
    size_t tail = (head_ + size_) % capacity_;
    size_t first = std::min(n, capacity_ - tail);
    std::memcpy(&buffer_[tail * kChannels], frames, first * kChannels * sizeof(float));
    std::memcpy(&buffer_[0], frames + first * kChannels, (n - first) * kChannels * sizeof(float));
    size_ += n;
    return static_cast<long>(n);
}

bool AudioStream::writeAll(const float* frames, size_t count, uint64_t epoch,
                           const std::function<bool()>& abort) {
    while (count > 0) {
        if (abort && abort()) return false;
        long n = write(frames, count, epoch);
        if (n < 0) return false;
        frames += n * kChannels;
        count -= static_cast<size_t>(n);
        if (count > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

size_t AudioStream::read(float* out, size_t count) {
    std::lock_guard lock(mutex_);
    size_t n = std::min(count, size_);
    size_t first = std::min(n, capacity_ - head_);
    std::memcpy(out, &buffer_[head_ * kChannels], first * kChannels * sizeof(float));
    std::memcpy(out + first * kChannels, &buffer_[0], (n - first) * kChannels * sizeof(float));
    head_ = (head_ + n) % capacity_;
    size_ -= n;
    return n;
}

} // namespace oap
