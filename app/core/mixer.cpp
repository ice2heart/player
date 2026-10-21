#include "mixer.h"

#include <algorithm>

namespace oap {

namespace {
constexpr size_t kChunk = 1024;
constexpr float kDuckGain = 0.3f;
constexpr float kDuckRamp = 0.002f; // per frame, ~10 ms time constant

void mixIn(AudioStream& stream, float* out, float* scratch, size_t frames) {
    size_t got = stream.read(scratch, frames);
    for (size_t i = 0; i < got * AudioStream::kChannels; ++i) out[i] += scratch[i];
}
} // namespace

Mixer::Mixer() : scratch_(kChunk * AudioStream::kChannels) {}

void Mixer::render(float* out, size_t frames) {
    while (frames > 0) {
        size_t n = std::min(frames, kChunk);
        std::fill(out, out + n * AudioStream::kChannels, 0.0f);

        float target = speech_.buffered() > 0 ? kDuckGain : 1.0f;
        if (!contentPaused_) {
            size_t got = content_.read(scratch_.data(), n);
            for (size_t i = 0; i < got; ++i) {
                contentGain_ += (target - contentGain_) * kDuckRamp;
                out[2 * i] += scratch_[2 * i] * contentGain_;
                out[2 * i + 1] += scratch_[2 * i + 1] * contentGain_;
            }
        }
        mixIn(speech_, out, scratch_.data(), n);
        mixIn(effects_, out, scratch_.data(), n);

        float v = volume_;
        float gain = v * v;
        for (size_t i = 0; i < n * AudioStream::kChannels; ++i)
            out[i] = std::clamp(out[i] * gain, -1.0f, 1.0f);

        out += n * AudioStream::kChannels;
        frames -= n;
    }
}

} // namespace oap
