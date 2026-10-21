#include "core/earcons.h"
#include "core/mixer.h"

#include <gtest/gtest.h>

#include <vector>

using namespace oap;

namespace {
void fill(AudioStream& stream, size_t count, float value) {
    std::vector<float> in(count * AudioStream::kChannels, value);
    stream.write(in.data(), count, stream.epoch());
}

std::vector<float> render(Mixer& m, size_t count) {
    std::vector<float> out(count * AudioStream::kChannels);
    m.render(out.data(), count);
    return out;
}
} // namespace

TEST(Mixer, SilentWhenNothingQueued) {
    Mixer m;
    m.setVolume(1.0f);
    for (float s : render(m, 64)) EXPECT_EQ(s, 0.0f);
}

TEST(Mixer, PausedContentStaysBuffered) {
    Mixer m;
    m.setVolume(1.0f);
    fill(m.content(), 100, 0.5f);
    m.setContentPaused(true);
    render(m, 100);
    EXPECT_EQ(m.content().buffered(), 100u);
    m.setContentPaused(false);
    auto out = render(m, 100);
    EXPECT_EQ(m.content().buffered(), 0u);
    EXPECT_NEAR(out[0], 0.5f, 1e-6);
}

TEST(Mixer, VolumeIsSquared) {
    Mixer m;
    m.setVolume(0.5f);
    fill(m.effects(), 10, 0.8f);
    auto out = render(m, 10);
    EXPECT_NEAR(out[0], 0.8f * 0.25f, 1e-6);
}

TEST(Mixer, SumIsClamped) {
    Mixer m;
    m.setVolume(1.0f);
    fill(m.speech(), 10, 0.9f);
    fill(m.effects(), 10, 0.9f);
    for (float s : render(m, 10)) EXPECT_EQ(s, 1.0f);
}

TEST(Mixer, ContentIsDuckedWhileSpeechIsBuffered) {
    Mixer m;
    m.setVolume(1.0f);
    m.setContentPaused(false);
    fill(m.content(), 4000, 0.5f);
    fill(m.speech(), 4000, 0.0f); // silent speech still ducks
    auto out = render(m, 4000);
    EXPECT_NEAR(out[2 * 3999], 0.5f * 0.3f, 0.01f);
    EXPECT_GT(out[0], 0.45f) << "ducking ramps in, it does not jump";
}

TEST(Earcons, NewEarconReplacesThePrevious) {
    Mixer m;
    Earcons e(m);
    e.play(Earcon::Boot);
    size_t boot = m.effects().buffered();
    EXPECT_GT(boot, 0u);
    e.play(Earcon::Move);
    EXPECT_GT(m.effects().buffered(), 0u);
    EXPECT_LT(m.effects().buffered(), boot);
}
