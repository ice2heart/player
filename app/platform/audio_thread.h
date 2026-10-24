#pragma once

#include "audio_output.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace oap {

// Base for backends with a blocking write API (PulseAudio simple, ALSA, null):
// a thread renders 10 ms blocks from the mixer and writes them, and the write
// blocking on a full device buffer paces the loop.
//
// If the device goes away (USB DAC unplugged, sound server restarted), the
// thread closes it, keeps rendering the mix in real time into nothing, so that
// speech and playback still finish, and tries to reopen the device with a
// growing delay until it works or stop() is called.
class ThreadedAudioOutput : public AudioOutput {
public:
    static constexpr size_t kBlockFrames = Mixer::kSampleRate / 100;

    bool start(Mixer& mixer) override {
        if (!openDevice()) return false;
        mixer_ = &mixer;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!thread_.joinable()) return;
        running_ = false;
        thread_.join();
        closeDevice();
    }

protected:
    // Called by subclasses' destructors, so that closeDevice() is still theirs.
    void stopThread() { stop(); }

    // Delays between attempts to reopen a lost device: the first, then doubling up to the last.
    void setRetryDelays(std::chrono::milliseconds first, std::chrono::milliseconds max) {
        retryFirst_ = first;
        retryMax_ = max;
    }

    virtual bool openDevice() = 0;
    // Blocks until the block is queued. Returns false when the device is lost.
    virtual bool write(const float* interleaved, size_t frames) = 0;
    // Must be safe to call when the device is already closed.
    virtual void closeDevice() = 0;

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kBlockTime = std::chrono::microseconds(kBlockFrames * 1'000'000 / Mixer::kSampleRate);

    void run() {
        std::vector<float> block(kBlockFrames * AudioStream::kChannels);
        while (running_) {
            mixer_->render(block.data(), kBlockFrames);
            if (write(block.data(), kBlockFrames)) continue;
            std::fprintf(stderr, "audio: output failed, trying to reopen it\n");
            closeDevice();
            if (!reopen(block)) return; // stopped
            std::fprintf(stderr, "audio: output reopened\n");
        }
    }

    // Discards the mix in real time until the device opens again. Returns false if stopped first.
    bool reopen(std::vector<float>& block) {
        auto delay = retryFirst_;
        auto next = Clock::now();
        auto retryAt = next + delay;
        while (running_) {
            mixer_->render(block.data(), kBlockFrames);
            next += kBlockTime;
            std::this_thread::sleep_until(next);
            if (Clock::now() < retryAt) continue;
            if (openDevice()) return true;
            delay = std::min(delay * 2, retryMax_);
            next = Clock::now(); // opening may have taken a while: don't race to catch up
            retryAt = next + delay;
        }
        return false;
    }

    Mixer* mixer_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::chrono::milliseconds retryFirst_{250};
    std::chrono::milliseconds retryMax_{4000};
};

} // namespace oap
