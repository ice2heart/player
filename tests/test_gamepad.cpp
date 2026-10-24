#include "frontend/gamepad.h"

#include <gtest/gtest.h>

#include <iterator>
#include <string>

using namespace oap;
using namespace std::chrono_literals;

namespace {

// Changes as text: "Back down, Back deep down".
std::string describe(const std::vector<ButtonChange>& changes) {
    std::string out;
    for (const auto& c : changes)
        out += (out.empty() ? "" : ", ") + std::string(buttonName(c.button)) + (c.down ? " down" : " up");
    return out;
}

std::string press(GamepadMapper& pad, PadControl c) { return describe(pad.set(c, true)); }
std::string release(GamepadMapper& pad, PadControl c) { return describe(pad.set(c, false)); }
std::string hatX(GamepadMapper& pad, int value) {
    return describe(pad.setAxis(PadControl::DpadLeft, PadControl::DpadRight, value));
}

} // namespace

TEST(Gamepad, FaceButtonsMapToPlayOkAndHome) {
    GamepadMapper pad;
    EXPECT_EQ(press(pad, PadControl::West), "Play/Pause down"); // Square
    EXPECT_EQ(release(pad, PadControl::West), "Play/Pause up");
    EXPECT_EQ(press(pad, PadControl::South), "OK down");        // Cross
    EXPECT_EQ(release(pad, PadControl::South), "OK up");
    EXPECT_EQ(press(pad, PadControl::East), "Home down");       // Circle
    EXPECT_EQ(release(pad, PadControl::East), "Home up");
}

TEST(Gamepad, DpadButtonsAreArrows) {
    GamepadMapper pad;
    EXPECT_EQ(press(pad, PadControl::DpadUp), "Up down");
    EXPECT_EQ(press(pad, PadControl::DpadDown), "Down down");
    EXPECT_EQ(press(pad, PadControl::DpadLeft), "Back down");
    EXPECT_EQ(press(pad, PadControl::DpadRight), "Forward down");
}

TEST(Gamepad, DpadHatAxisPressesAndReleasesBackAndForward) {
    GamepadMapper pad;
    EXPECT_EQ(hatX(pad, -1), "Back down");
    EXPECT_EQ(hatX(pad, 1), "Back up, Forward down"); // rolled straight across
    EXPECT_EQ(hatX(pad, 0), "Forward up");
    EXPECT_EQ(hatX(pad, 0), "");
}

TEST(Gamepad, UnmappedControlsDoNothing) {
    GamepadMapper pad;
    EXPECT_EQ(press(pad, PadControl::North), "");
    EXPECT_EQ(press(pad, PadControl::R1), "");
    EXPECT_EQ(press(pad, PadControl::L1), ""); // the deep modifier alone
}

TEST(Gamepad, L1WithLeftPressesTheDeepStageAfterTheLightStage) {
    GamepadMapper pad;
    pad.set(PadControl::L1, true);
    EXPECT_EQ(hatX(pad, -1), "Back down, Back deep down");
    EXPECT_EQ(hatX(pad, 0), "Back deep up, Back up");
    EXPECT_EQ(hatX(pad, 1), "Forward down, Forward deep down");
}

TEST(Gamepad, L1PressedWhileLeftIsHeldDeepensThePress) {
    GamepadMapper pad;
    EXPECT_EQ(hatX(pad, -1), "Back down");
    EXPECT_EQ(press(pad, PadControl::L1), "Back deep down");
    EXPECT_EQ(release(pad, PadControl::L1), "Back deep up");
    EXPECT_EQ(hatX(pad, 0), "Back up");
}

TEST(Gamepad, L1DoesNotDeepenKeysWithoutADeepStage) {
    GamepadMapper pad;
    pad.set(PadControl::L1, true);
    EXPECT_EQ(press(pad, PadControl::DpadUp), "Up down");
    EXPECT_EQ(press(pad, PadControl::South), "OK down");
}

TEST(Gamepad, L1WithLeftIsADeepGestureAndTheReleaseSaysNothing) {
    GamepadMapper pad;
    GestureDetector g;
    auto t0 = GestureDetector::Clock::time_point{};
    std::vector<ButtonEvent> events;
    auto feed = [&](const std::vector<ButtonChange>& changes, GestureDetector::Clock::time_point t) {
        for (const auto& c : changes) {
            auto ev = c.down ? g.press(c.button, t) : g.release(c.button, t);
            if (ev) events.push_back(*ev);
        }
    };
    feed(pad.set(PadControl::L1, true), t0);
    feed(pad.setAxis(PadControl::DpadLeft, PadControl::DpadRight, -1), t0 + 10ms);
    feed(pad.setAxis(PadControl::DpadLeft, PadControl::DpadRight, 0), t0 + 100ms);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].button, Button::Back);
    EXPECT_EQ(events[0].gesture, Gesture::Deep);
}

TEST(Gamepad, ReleaseAllLetsGoOfEveryHeldButton) {
    GamepadMapper pad;
    pad.set(PadControl::L1, true);
    pad.setAxis(PadControl::DpadLeft, PadControl::DpadRight, 1);
    pad.set(PadControl::South, true);
    EXPECT_EQ(describe(pad.releaseAll()), "Forward deep up, OK up, Forward up");
    EXPECT_EQ(describe(pad.releaseAll()), "");
}

TEST(Gamepad, ALayoutCanBeRemapped) {
    GamepadLayout layout;
    layout.buttons[size_t(PadControl::South)] = Button::PlayPause;
    layout.buttons[size_t(PadControl::DpadRight)] = Button::Forward;
    layout.deep = PadControl::R1;
    GamepadMapper pad(layout);
    EXPECT_EQ(press(pad, PadControl::South), "Play/Pause down");
    EXPECT_EQ(press(pad, PadControl::West), "");
    pad.set(PadControl::R1, true);
    EXPECT_EQ(press(pad, PadControl::DpadRight), "Forward down, Forward deep down");
}

TEST(Gamepad, DpadHatYAxisPressesAndReleasesUpAndDown) {
    GamepadMapper pad;
    auto hatY = [&](int value) { return describe(pad.setAxis(PadControl::DpadUp, PadControl::DpadDown, value)); };
    EXPECT_EQ(hatY(-1), "Up down");
    EXPECT_EQ(hatY(1), "Up up, Down down");
    EXPECT_EQ(hatY(0), "Down up");
    EXPECT_EQ(hatY(0), "");
}

TEST(Gamepad, BothHatAxesAtOnceHoldTwoButtons) {
    GamepadMapper pad;
    EXPECT_EQ(hatX(pad, 1), "Forward down");
    EXPECT_EQ(describe(pad.setAxis(PadControl::DpadUp, PadControl::DpadDown, 1)), "Down down"); // diagonal
    EXPECT_EQ(hatX(pad, 0), "Forward up");
    EXPECT_EQ(describe(pad.releaseAll()), "Down up");
}

TEST(Gamepad, DefaultLayoutMapsOnlyTheDocumentedControls) {
    const char* names[] = {"DpadUp", "DpadDown", "DpadLeft", "DpadRight", "South", "East", "West", "North",
                           "L1", "R1", "L2", "R2", "Select", "Start", "Mode", "ThumbL", "ThumbR"};
    static_assert(std::size(names) == kPadControlCount);
    std::string mapped;
    for (size_t c = 0; c < kPadControlCount; ++c) {
        GamepadMapper pad;
        std::string change = press(pad, PadControl(c));
        if (!change.empty()) mapped += (mapped.empty() ? "" : ", ") + std::string(names[c]) + ": " + change;
    }
    EXPECT_EQ(mapped, "DpadUp: Up down, DpadDown: Down down, DpadLeft: Back down, DpadRight: Forward down, "
                      "South: OK down, East: Home down, West: Play/Pause down");
}
