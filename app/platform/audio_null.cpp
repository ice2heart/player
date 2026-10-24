// Consumes the mix in real time and discards it, so the player runs (and
// positions advance) on machines without a sound card.

#include "audio_thread.h"

#include <chrono>

namespace oap {

namespace {

class NullOutput : public ThreadedAudioOutput {
public:
    ~NullOutput() override { stopThread(); }

protected:
    bool openDevice() override {
        next_ = std::chrono::steady_clock::now();
        return true;
    }

    bool write(const float*, size_t frames) override {
        next_ += std::chrono::microseconds(frames * 1'000'000 / Mixer::kSampleRate);
        std::this_thread::sleep_until(next_);
        return true;
    }

    void closeDevice() override {}

private:
    std::chrono::steady_clock::time_point next_;
};

} // namespace

std::unique_ptr<AudioOutput> makeNullOutput() { return std::make_unique<NullOutput>(); }

} // namespace oap
