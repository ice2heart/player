#pragma once

#include "audio_output.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace oap {

// Base for backends with a blocking write API (PulseAudio simple, ALSA, null):
// a thread renders 10 ms blocks from the mixer and writes them, and the write
// blocking on a full device buffer paces the loop.
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

    virtual bool openDevice() = 0;
    // Blocks until the block is queued. Returns false on an unrecoverable error.
    virtual bool write(const float* interleaved, size_t frames) = 0;
    virtual void closeDevice() = 0;

private:
    void run() {
        std::vector<float> block(kBlockFrames * AudioStream::kChannels);
        while (running_) {
            mixer_->render(block.data(), kBlockFrames);
            if (!write(block.data(), kBlockFrames)) {
                std::fprintf(stderr, "audio: output failed, stopping\n");
                return;
            }
        }
    }

    Mixer* mixer_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace oap
