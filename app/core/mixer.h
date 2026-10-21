#pragma once

#include "audio_stream.h"

#include <atomic>
#include <vector>

namespace oap {

// Mixes the three sources the device ever plays: content (book/podcast),
// speech (TTS) and effects (earcons). Content is ducked while speech plays.
// render() is called from the platform audio thread.
class Mixer {
public:
    static constexpr int kSampleRate = 48000;

    Mixer();

    AudioStream& content() { return content_; }
    AudioStream& speech() { return speech_; }
    AudioStream& effects() { return effects_; }

    void setContentPaused(bool paused) { contentPaused_ = paused; }
    // 0..1, mapped to a perceptual curve.
    void setVolume(float volume) { volume_ = volume; }

    void render(float* out, size_t frames);

private:
    AudioStream content_{kSampleRate * 2};
    AudioStream speech_{kSampleRate * 4};
    AudioStream effects_{kSampleRate};
    std::atomic<bool> contentPaused_{true};
    std::atomic<float> volume_{0.7f};
    float contentGain_ = 1.0f; // audio thread only
    std::vector<float> scratch_;
};

} // namespace oap
