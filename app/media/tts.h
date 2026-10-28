#pragma once

#include "core/mixer.h"
#include "core/services.h"
#include "core/text.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct SwrContext;

namespace oap {

// eSpeak NG on a worker thread. Speech goes into the mixer's speech stream;
// stop() cancels both queued and currently playing speech immediately.
class Tts : public Speech {
public:
    explicit Tts(Mixer& mixer);
    ~Tts() override;

    bool start(const std::string& voice);
    void say(const std::string& text) override;
    void stop() override;
    bool speaking() const override;
    void setRate(int wordsPerMinute) override { rate_ = wordsPerMinute; }

private:
    struct Utterance {
        std::string text;
        uint64_t epoch;
    };

    void run();
    int onSamples(const short* samples, int count);
    static int synthCallback(short* samples, int count, void* events);

    Mixer& mixer_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Utterance> queue_;
    bool busy_ = false;
    std::atomic<bool> quit_{false};
    std::atomic<int> rate_{220};
    uint64_t currentEpoch_ = 0; // worker thread only
    SwrContext* swr_ = nullptr;
    VoiceSet voices_; // set in start(), read only by the worker
    int espeakRate_ = 22050;
    std::vector<float> converted_;
};

} // namespace oap
