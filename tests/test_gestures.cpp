#include "core/buttons.h"

#include <gtest/gtest.h>

using namespace oap;
using namespace std::chrono_literals;

namespace {
const GestureDetector::Clock::time_point t0{std::chrono::hours(1)};
void expectGesture(std::optional<ButtonEvent> event, Gesture expected) {
    ASSERT_TRUE(event);
    EXPECT_EQ(event->gesture, expected);
}
}

TEST(GestureDetector, QuickReleaseIsShortPress) {
    GestureDetector g;
    g.press(Button::Ok, t0);
    EXPECT_TRUE(g.poll(t0 + 100ms).empty());
    auto ev = g.release(Button::Ok, t0 + 150ms);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->button, Button::Ok);
    EXPECT_EQ(ev->gesture, Gesture::Short);
}

TEST(GestureDetector, LongPressFiresWhileHeldAndOnlyOnce) {
    GestureDetector g;
    g.press(Button::Up, t0);
    EXPECT_TRUE(g.poll(t0 + 599ms).empty());
    auto events = g.poll(t0 + 600ms);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].button, Button::Up);
    EXPECT_EQ(events[0].gesture, Gesture::Long);
    EXPECT_TRUE(g.poll(t0 + 2s).empty());
    EXPECT_FALSE(g.release(Button::Up, t0 + 2s)) << "release after a long press must not add a short press";
}

TEST(GestureDetector, LongPressDurationIsConfigurable) {
    GestureDetector g;
    g.setLongPress(300ms);
    g.press(Button::Home, t0);
    EXPECT_EQ(g.poll(t0 + 300ms).size(), 1u);
}

TEST(GestureDetector, ReleaseWithoutPressIsIgnored) {
    GestureDetector g;
    EXPECT_FALSE(g.release(Button::Down, t0));
}

TEST(GestureDetector, ButtonsAreIndependent) {
    GestureDetector g;
    g.press(Button::Back, t0);
    g.press(Button::Forward, t0 + 500ms);
    auto events = g.poll(t0 + 700ms);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].button, Button::Back);
    auto ev = g.release(Button::Forward, t0 + 800ms);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->gesture, Gesture::Short);
}

TEST(GestureDetector, DeepThenHoldUsesTheInitialDeadlineAndEndsWithoutShort) {
    GestureDetector g;
    EXPECT_FALSE(g.press(Button::Back, t0));
    auto deep = g.press(Button::BackDeep, t0 + 50ms);
    ASSERT_TRUE(deep);
    EXPECT_EQ(deep->button, Button::Back);
    EXPECT_EQ(deep->gesture, Gesture::Deep);
    EXPECT_TRUE(g.poll(t0 + 599ms).empty());
    auto held = g.poll(t0 + 600ms);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].button, Button::Back);
    EXPECT_EQ(held[0].gesture, Gesture::Long);
    EXPECT_TRUE(g.poll(t0 + 2s).empty());
    auto light = g.release(Button::BackDeep, t0 + 2s);
    ASSERT_TRUE(light);
    EXPECT_EQ(light->gesture, Gesture::Light);
    auto end = g.release(Button::Back, t0 + 2s);
    ASSERT_TRUE(end);
    EXPECT_EQ(end->gesture, Gesture::Release);
    EXPECT_FALSE(g.release(Button::Back, t0 + 2s));
}

TEST(GestureDetector, PressAfterADeepPressIsOrdinary) {
    GestureDetector g;
    g.press(Button::Forward, t0);
    g.press(Button::ForwardDeep, t0 + 50ms);
    g.release(Button::ForwardDeep, t0 + 60ms);
    g.release(Button::Forward, t0 + 70ms);
    g.press(Button::Forward, t0 + 1s);
    auto ev = g.release(Button::Forward, t0 + 1100ms);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->gesture, Gesture::Short);
}

TEST(GestureDetector, DeepStageBeforeLightKeepsOneDeadlineAndEndsOnLastContact) {
    GestureDetector g;
    auto deep = g.press(Button::ForwardDeep, t0);
    ASSERT_TRUE(deep);
    EXPECT_EQ(deep->button, Button::Forward);
    EXPECT_EQ(deep->gesture, Gesture::Deep);
    EXPECT_FALSE(g.press(Button::Forward, t0 + 100ms));
    auto held = g.poll(t0 + 600ms);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].gesture, Gesture::Long);
    EXPECT_FALSE(g.release(Button::Forward, t0 + 700ms));
    auto end = g.release(Button::ForwardDeep, t0 + 800ms);
    ASSERT_TRUE(end);
    EXPECT_EQ(end->button, Button::Forward);
    EXPECT_EQ(end->gesture, Gesture::Release);
}

TEST(GestureDetector, LightStageBounceWhileDeepDoesNotResetTheHold) {
    GestureDetector g;
    g.press(Button::Forward, t0);
    ASSERT_TRUE(g.press(Button::ForwardDeep, t0 + 50ms));
    EXPECT_FALSE(g.release(Button::Forward, t0 + 60ms));
    EXPECT_FALSE(g.press(Button::Forward, t0 + 65ms));
    auto held = g.poll(t0 + 600ms);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].gesture, Gesture::Long);
    expectGesture(g.release(Button::ForwardDeep, t0 + 700ms), Gesture::Light);
    expectGesture(g.release(Button::Forward, t0 + 800ms), Gesture::Release);
}

TEST(GestureDetector, DeepAfterLightHoldAndEasingAreReportedEachTime) {
    GestureDetector g;
    g.press(Button::Back, t0);
    ASSERT_EQ(g.poll(t0 + 600ms).size(), 1u);
    for (int i = 0; i < 3; ++i) {
        auto deep = g.press(Button::BackDeep, t0 + 700ms + 200ms * i);
        ASSERT_TRUE(deep);
        EXPECT_EQ(deep->gesture, Gesture::Deep);
        auto light = g.release(Button::BackDeep, t0 + 800ms + 200ms * i);
        ASSERT_TRUE(light);
        EXPECT_EQ(light->button, Button::Back);
        EXPECT_EQ(light->gesture, Gesture::Light);
        EXPECT_TRUE(g.poll(t0 + 800ms + 200ms * i).empty());
    }
    expectGesture(g.release(Button::Back, t0 + 2s), Gesture::Release);
}

TEST(GestureDetector, DuplicateDownDoesNotRestartDeadlinesOrVolume) {
    GestureDetector g;
    g.press(Button::Forward, t0);
    EXPECT_FALSE(g.press(Button::Forward, t0 + 500ms));
    ASSERT_EQ(g.poll(t0 + 600ms).size(), 1u);
    EXPECT_FALSE(g.press(Button::Forward, t0 + 700ms));
    EXPECT_TRUE(g.poll(t0 + 2s).empty());
    expectGesture(g.release(Button::Forward, t0 + 2s), Gesture::Release);
    ASSERT_TRUE(g.press(Button::VolumeUp, t0 + 3s));
    EXPECT_FALSE(g.press(Button::VolumeUp, t0 + 3300ms));
    auto repeat = g.poll(t0 + 3400ms);
    ASSERT_EQ(repeat.size(), 1u);
    EXPECT_EQ(repeat[0].gesture, Gesture::Repeat);
}

TEST(GestureDetector, CancelSuppressesBothContactsUntilFinalRelease) {
    GestureDetector g;
    g.press(Button::Forward, t0);
    g.press(Button::ForwardDeep, t0 + 50ms);
    auto cancelled = g.cancel();
    ASSERT_EQ(cancelled.size(), 1u);
    EXPECT_EQ(cancelled[0].button, Button::Forward);
    EXPECT_EQ(cancelled[0].gesture, Gesture::Cancel);
    EXPECT_TRUE(g.cancel().empty());
    EXPECT_TRUE(g.poll(t0 + 2s).empty());
    EXPECT_FALSE(g.release(Button::Forward, t0 + 2s));
    EXPECT_FALSE(g.press(Button::Forward, t0 + 2100ms));
    EXPECT_FALSE(g.release(Button::ForwardDeep, t0 + 2200ms));
    EXPECT_FALSE(g.release(Button::Forward, t0 + 2300ms));
    g.press(Button::Forward, t0 + 3s);
    ASSERT_TRUE(g.release(Button::Forward, t0 + 3100ms));
    ASSERT_TRUE(g.press(Button::ForwardDeep, t0 + 4s));
    EXPECT_EQ(g.cancel().size(), 1u);
    EXPECT_FALSE(g.release(Button::ForwardDeep, t0 + 4100ms));
}

TEST(GestureDetector, CancelSilencesShortsAndRepeatsButCanPreservePower) {
    GestureDetector g;
    g.press(Button::Ok, t0);
    g.press(Button::Power, t0);
    g.press(Button::VolumeDown, t0);
    EXPECT_TRUE(g.cancel(true).empty());
    auto held = g.poll(t0 + 1s);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].button, Button::Power);
    EXPECT_EQ(held[0].gesture, Gesture::Long);
    EXPECT_FALSE(g.release(Button::Ok, t0 + 1s));
    EXPECT_FALSE(g.release(Button::VolumeDown, t0 + 1s));
    EXPECT_FALSE(g.release(Button::Power, t0 + 1s));
}

// ---------------------------------------------------------------- repeating keys

TEST(GestureDetector, VolumeKeyStepsAtOnceAndReleaseSaysNothing) {
    GestureDetector g;
    auto ev = g.press(Button::VolumeUp, t0);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->button, Button::VolumeUp);
    EXPECT_EQ(ev->gesture, Gesture::Short);
    EXPECT_FALSE(g.release(Button::VolumeUp, t0 + 100ms));
    EXPECT_TRUE(g.poll(t0 + 2s).empty()) << "nothing repeats once the key is up";
}

TEST(GestureDetector, HeldVolumeKeyRepeatsEveryIntervalAfterTheDelay) {
    GestureDetector g;
    g.press(Button::VolumeDown, t0);
    EXPECT_TRUE(g.poll(t0 + GestureDetector::kRepeatDelay - 1ms).empty()) << "a slow tap does not repeat";
    auto first = g.poll(t0 + GestureDetector::kRepeatDelay);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].button, Button::VolumeDown);
    EXPECT_EQ(first[0].gesture, Gesture::Repeat);
    auto at = t0 + GestureDetector::kRepeatDelay;
    EXPECT_TRUE(g.poll(at + GestureDetector::kRepeatInterval - 1ms).empty());
    EXPECT_EQ(g.poll(at + GestureDetector::kRepeatInterval).size(), 1u);
    EXPECT_EQ(g.poll(at + 2 * GestureDetector::kRepeatInterval).size(), 1u);
}

TEST(GestureDetector, LettingGoOfAVolumeKeyStopsTheRepeat) {
    GestureDetector g;
    g.press(Button::VolumeUp, t0);
    EXPECT_EQ(g.poll(t0 + 500ms).size(), 1u);
    g.release(Button::VolumeUp, t0 + 520ms);
    EXPECT_TRUE(g.poll(t0 + 700ms).empty());
    EXPECT_TRUE(g.poll(t0 + 2s).empty());
}

TEST(GestureDetector, VolumeKeyNeverBecomesALongPress) {
    GestureDetector g;
    g.press(Button::VolumeUp, t0);
    for (auto t = t0; t < t0 + 3s; t += 50ms)
        for (const auto& ev : g.poll(t)) EXPECT_EQ(ev.gesture, Gesture::Repeat);
}

TEST(GestureDetector, OtherKeysDoNotRepeat) {
    GestureDetector g;
    EXPECT_FALSE(g.press(Button::Options, t0));
    EXPECT_FALSE(g.press(Button::Up, t0));
    auto events = g.poll(t0 + 500ms); // before a long press
    EXPECT_TRUE(events.empty());
    for (const auto& ev : g.poll(t0 + 3s)) EXPECT_EQ(ev.gesture, Gesture::Long);
}

TEST(GestureDetector, TwoVolumeKeysRepeatIndependently) {
    GestureDetector g;
    g.press(Button::VolumeUp, t0);
    g.press(Button::VolumeDown, t0 + 200ms);
    auto events = g.poll(t0 + 400ms);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].button, Button::VolumeUp);
    events = g.poll(t0 + 600ms);
    ASSERT_EQ(events.size(), 2u);
}
