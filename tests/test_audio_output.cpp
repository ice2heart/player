#include "platform/audio_output.h"
#include "platform/audio_thread.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace oap;
using namespace std::chrono_literals;

namespace {

void fill(AudioStream& stream, size_t count, float value) {
    std::vector<float> in(count * AudioStream::kChannels, value);
    stream.write(in.data(), count, stream.epoch());
}

// Polls until done() or the timeout. Real threads, so real time.
bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 3000ms) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A device that can be unplugged: while lost, writes and opens fail.
class FakeDevice : public ThreadedAudioOutput {
public:
    FakeDevice() { setRetryDelays(5ms, 20ms); }
    ~FakeDevice() override { stopThread(); }

    std::atomic<bool> lost{false};
    std::atomic<int> opens{0};
    std::atomic<int> doubleOpens{0}; // opened while already open
    std::atomic<int> writes{0};      // successful writes
    std::atomic<bool> heardSound{false};

protected:
    bool openDevice() override {
        if (lost) return false;
        if (open_) ++doubleOpens;
        open_ = true;
        ++opens;
        return true;
    }

    bool write(const float* data, size_t frames) override {
        if (lost) return false;
        for (size_t i = 0; i < frames * AudioStream::kChannels; ++i)
            if (data[i] != 0.0f) heardSound = true;
        ++writes;
        std::this_thread::sleep_for(1ms);
        return true;
    }

    void closeDevice() override { open_ = false; }

private:
    std::atomic<bool> open_{false};
};

} // namespace

TEST(AudioOutput, NullBackendConsumesTheMix) {
    Mixer mixer;
    auto output = AudioOutput::open("null", mixer);
    ASSERT_NE(output, nullptr);
    fill(mixer.effects(), Mixer::kSampleRate / 20, 0.5f); // 50 ms
    EXPECT_TRUE(waitFor([&] { return mixer.effects().buffered() == 0; }));
    output->stop();
}

TEST(AudioOutput, ArgumentAfterColonStillPicksTheBackend) {
    Mixer mixer;
    auto output = AudioOutput::open("null:whatever", mixer);
    EXPECT_NE(output, nullptr);
}

TEST(AudioOutput, UnknownBackendIsRefused) {
    Mixer mixer;
    EXPECT_EQ(AudioOutput::open("nosuch", mixer), nullptr);
    EXPECT_EQ(AudioOutput::open("nul", mixer), nullptr);
}

TEST(AudioOutput, AutoNeverPicksNull) {
    if (AudioOutput::backends() != std::vector<std::string>{"auto", "null"})
        GTEST_SKIP() << "device backends are built in; auto would open a real device";
    Mixer mixer;
    EXPECT_EQ(AudioOutput::open("auto", mixer), nullptr);
}

TEST(AudioOutput, BackendsListsAutoAndNull) {
    auto names = AudioOutput::backends();
    ASSERT_FALSE(names.empty());
    EXPECT_EQ(names.front(), "auto");
    EXPECT_EQ(names.back(), "null");
}

TEST(ThreadedAudioOutput, KeepsDrainingTheMixWhileTheDeviceIsLost) {
    Mixer mixer;
    FakeDevice device;
    ASSERT_TRUE(device.start(mixer));
    ASSERT_TRUE(waitFor([&] { return device.writes > 0; }));

    device.lost = true;
    fill(mixer.speech(), Mixer::kSampleRate / 5, 0.5f); // 200 ms of speech nobody can hear
    EXPECT_TRUE(waitFor([&] { return mixer.speech().buffered() == 0; }))
        << "speech still buffered: Tts::speaking() would never end";
    device.stop();
}

TEST(ThreadedAudioOutput, LostDeviceIsDiscardedInRealTime) {
    Mixer mixer;
    FakeDevice device;
    ASSERT_TRUE(device.start(mixer));
    ASSERT_TRUE(waitFor([&] { return device.writes > 0; }));

    device.lost = true;
    fill(mixer.speech(), Mixer::kSampleRate, 0.5f); // 1 s
    std::this_thread::sleep_for(300ms);
    // Not thrown away at once: speech takes as long as when it is heard.
    EXPECT_GT(mixer.speech().buffered(), size_t(Mixer::kSampleRate / 2));
    device.stop();
}

TEST(ThreadedAudioOutput, ReopensTheDeviceWhenItComesBack) {
    Mixer mixer;
    FakeDevice device;
    ASSERT_TRUE(device.start(mixer));
    ASSERT_TRUE(waitFor([&] { return device.writes > 0; }));

    device.lost = true;
    std::this_thread::sleep_for(100ms); // several failed attempts to reopen
    device.lost = false;
    ASSERT_TRUE(waitFor([&] { return device.opens == 2; })) << "never reopened";

    fill(mixer.effects(), Mixer::kSampleRate / 10, 0.5f);
    EXPECT_TRUE(waitFor([&] { return device.heardSound.load(); })) << "output did not resume";
    EXPECT_EQ(device.doubleOpens, 0) << "reopened without closing the lost device";
    device.stop();
}
