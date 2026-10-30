#include "core/settings.h"
#include "fakes.h"
#include "frontend/presses.h"
#include "frontend/terminal_keys.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <string>

using namespace oap;
using namespace oap::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Steps as text: "OK, Forward long, Back deep, wait 3000, quit".
std::string describe(const std::vector<PressStep>& steps) {
    std::string out;
    for (const auto& s : steps) {
        std::string one;
        if (s.quit) one = "quit";
        else if (!s.button) one = "wait " + std::to_string(s.waitMs);
        else one = std::string(buttonName(*s.button)) + (s.longPress ? " long" : "") + (s.deep ? " deep" : "");
        out += (out.empty() ? "" : ", ") + one;
    }
    return out;
}

} // namespace

TEST(ParseScript, ReadsButtonsWaitsAndQuit) {
    EXPECT_EQ(describe(parseScript("down ok wait:3000 right:long left:deep play talk power home up quit")),
              "Down, OK, wait 3000, Forward long, Back deep, Play/Pause, Talk, Power, Home, Up, quit");
}

TEST(ParseScript, VolumeAndOptionsSteps) {
    EXPECT_EQ(describe(parseScript("volup voldown:long options")), "Volume up, Volume down long, Options");
}

TEST(ParseScript, DeepOnlyForKeysWithADeepStage) {
    EXPECT_EQ(describe(parseScript("up:deep right:deep")), "Forward deep");
}

TEST(ParseScript, SkipsUnknownStepsAndWaitsWithoutANumber) {
    EXPECT_EQ(describe(parseScript("wait wait: wait:abc wait:12x wait:-5 jump ok wait:0 wait:250")),
              "OK, wait 0, wait 250");
}

// Drives a real App with fake services, ticking every 10 ms like a front end.
class PressSequencerTest : public testing::Test {
protected:
    void SetUp() override {
        dir = testTempDir();
        fs::remove_all(dir);
        app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(config, server, downloader), indexStore, scanner, config, state, clock, nullptr}, dir / "books", dir / "music");
        app->start();
        t0 = clock.now();
    }
    void TearDown() override { fs::remove_all(dir); }

    // Runs for `length` and returns when buttons went down and up, in ms from the
    // start: "0 OK down, 80 OK up". Stops early when the sequencer quits.
    std::string run(PressSequencer& seq, std::chrono::milliseconds length) {
        std::string log;
        std::array<bool, kButtonCount> was{};
        for (auto end = clock.now() + length; clock.now() < end; clock.advance(10ms)) {
            bool more = seq.tick(*app, &held, clock.now());
            for (size_t b = 0; b < kButtonCount; ++b) {
                bool now = held.held(Button(b));
                if (now == was[b]) continue;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - t0).count();
                log += (log.empty() ? "" : ", ") + std::to_string(ms) + " " + buttonName(Button(b)) +
                       (now ? " down" : " up");
                was[b] = now;
            }
            app->tick();
            if (!more) {
                log += (log.empty() ? "" : ", ") +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - t0).count()) +
                       " quit";
                break;
            }
        }
        return log;
    }

    int usage(Button b, Gesture g) const { return app->buttonUsage()[size_t(b)][size_t(g)]; }

    fs::path dir;
    FakeSpeech speech;
    FakePlayer player;
    FakeSounds sounds;
    FakeVolume volume;
    FakeMusicServer server;
    FakeDownloader downloader;
    FakeLibraryStore indexStore;
    FakeLibraryScanner scanner;
    FakeClock clock;
    Settings config;
    Settings state;
    std::unique_ptr<App> app;
    HeldButtons held;
    FakeClock::TimePoint t0;
};

TEST_F(PressSequencerTest, ShortPressesWaitsAndGapsFollowTheScript) {
    PressSequencer seq(700ms);
    seq.push(parseScript("ok wait:500 down quit"));
    // OK held 80 ms, 700 ms gap, 500 ms wait, Down held 80 ms, 700 ms gap, quit.
    EXPECT_EQ(run(seq, 5s), "0 OK down, 80 OK up, 1280 Down down, 1360 Down up, 2060 quit");
    EXPECT_EQ(usage(Button::Ok, Gesture::Short), 1);
    EXPECT_EQ(usage(Button::Down, Gesture::Short), 1);
}

TEST_F(PressSequencerTest, LongPressIsHeldLongEnoughForALongGesture) {
    PressSequencer seq(700ms);
    seq.push(parseScript("right:long"));
    EXPECT_EQ(run(seq, 3s), "0 Forward down, 1600 Forward up");
    EXPECT_EQ(usage(Button::Forward, Gesture::Long), 1);
    EXPECT_EQ(usage(Button::Forward, Gesture::Short), 0);
}

TEST_F(PressSequencerTest, DeepPressHoldsBothStages) {
    PressSequencer seq(700ms);
    seq.push(parseScript("left:deep"));
    EXPECT_EQ(run(seq, 1s), "0 Back down, 0 Back deep down, 80 Back up, 80 Back deep up");
    EXPECT_EQ(usage(Button::Back, Gesture::Deep), 1);
    EXPECT_EQ(usage(Button::Back, Gesture::Short), 0);
}

TEST_F(PressSequencerTest, AStepWithNeitherButtonNorWaitIsSkipped) {
    PressSequencer seq(700ms);
    seq.push(PressStep{});
    seq.push(parseScript("wait:abc ok"));
    EXPECT_EQ(run(seq, 1s), "10 OK down, 90 OK up");
    EXPECT_EQ(usage(Button::Ok, Gesture::Short), 1);
}

TEST_F(PressSequencerTest, AButtonHeldElsewhereStaysDownForApp) {
    held.press(Button::Ok); // a pad holds OK
    app->keyDown(Button::Ok);
    PressSequencer seq(700ms);
    seq.push(parseScript("ok"));
    run(seq, 200ms);
    EXPECT_TRUE(held.held(Button::Ok));
    EXPECT_EQ(usage(Button::Ok, Gesture::Short), 0); // App has not seen a release yet
    if (held.release(Button::Ok)) app->keyUp(Button::Ok);
    EXPECT_EQ(usage(Button::Ok, Gesture::Short), 1);
}

TEST(ParseScript, CombinesDeepAndLongAndRejectsUnknownModifiers) {
    EXPECT_EQ(describe(parseScript("right:deep:long left:long:deep right:nope ok:deep right:long:typo quit:long ok")),
              "Forward long deep, Back long deep, OK");
}

TEST_F(PressSequencerTest, TerminalAndScriptDeepHoldsGiveEquivalentMenuGestures) {
    PressSequencer seq(0ms);
    TerminalKeyDecoder decoder;
    auto terminal = decoder.decode("ld\x1b[C");
    ASSERT_EQ(terminal.size(), 1u);
    seq.push(terminal[0].press);
    EXPECT_EQ(run(seq, 2s), "0 Forward down, 0 Forward deep down, 1600 Forward up, 1600 Forward deep up");
    EXPECT_EQ(app->screens().back().items[app->screens().back().index].label, "Help");
    EXPECT_EQ(usage(Button::Forward, Gesture::Deep), 1);
    EXPECT_EQ(usage(Button::Forward, Gesture::Long), 1);
    EXPECT_EQ(usage(Button::Forward, Gesture::Short), 0);
    // Back uses the same lifecycle through the script parser; it ends at Library.
    seq.push(parseScript("left:deep:long"));
    run(seq, 2s);
    EXPECT_EQ(app->screens().back().items[app->screens().back().index].label, "Library");
    EXPECT_EQ(usage(Button::Back, Gesture::Deep), 1);
    EXPECT_EQ(usage(Button::Back, Gesture::Long), 1);
    EXPECT_EQ(usage(Button::Back, Gesture::Short), 0);
}
