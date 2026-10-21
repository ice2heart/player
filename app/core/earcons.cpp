#include "earcons.h"

#include <cmath>
#include <initializer_list>
#include <vector>

namespace oap {

namespace {
struct Tone {
    float freq; // 0 = silence
    float ms;
};

std::vector<float> synth(std::initializer_list<Tone> tones) {
    constexpr float kAmp = 0.25f;
    constexpr float kFadeMs = 3.0f;
    std::vector<float> out;
    for (const Tone& t : tones) {
        size_t n = static_cast<size_t>(t.ms * Mixer::kSampleRate / 1000.0f);
        size_t fade = static_cast<size_t>(kFadeMs * Mixer::kSampleRate / 1000.0f);
        for (size_t i = 0; i < n; ++i) {
            float env = 1.0f;
            if (i < fade) env = float(i) / fade;
            else if (n - i < fade) env = float(n - i) / fade;
            float s = t.freq > 0 ? kAmp * env * std::sin(2.0f * float(M_PI) * t.freq * i / Mixer::kSampleRate) : 0.0f;
            out.push_back(s);
            out.push_back(s);
        }
    }
    return out;
}

std::vector<float> render(Earcon e) {
    switch (e) {
    case Earcon::Move:   return synth({{1200, 20}});
    case Earcon::Wrap:   return synth({{1500, 25}, {0, 20}, {1500, 25}, {0, 20}, {1500, 25}});
    case Earcon::Edge:   return synth({{300, 70}});
    case Earcon::Open:   return synth({{600, 35}, {900, 45}});
    case Earcon::Close:  return synth({{900, 35}, {600, 45}});
    case Earcon::Error:  return synth({{250, 90}, {0, 40}, {250, 90}});
    case Earcon::Volume: return synth({{800, 40}});
    case Earcon::Play:   return synth({{700, 30}, {1050, 40}});
    case Earcon::Boot:   return synth({{523, 70}, {659, 70}, {784, 70}, {1047, 120}});
    case Earcon::Sleep:  return synth({{784, 80}, {659, 80}, {523, 140}});
    case Earcon::Wake:   return synth({{523, 60}, {659, 60}, {784, 90}});
    }
    return {};
}
} // namespace

void Earcons::play(Earcon earcon) {
    auto samples = render(earcon);
    // A new earcon cuts the previous one off; they are only feedback.
    uint64_t epoch = mixer_.effects().flush();
    mixer_.effects().write(samples.data(), samples.size() / AudioStream::kChannels, epoch);
}

} // namespace oap
