#include "core/buttons.h"

#include <gtest/gtest.h>

using namespace oap;
using namespace std::chrono_literals;

namespace {
const GestureDetector::Clock::time_point t0{std::chrono::hours(1)};
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

TEST(GestureDetector, DeepStageFiresAtOnceAndSilencesTheFirstStage) {
    GestureDetector g;
    EXPECT_FALSE(g.press(Button::Back, t0));
    auto deep = g.press(Button::BackDeep, t0 + 50ms);
    ASSERT_TRUE(deep);
    EXPECT_EQ(deep->button, Button::Back);
    EXPECT_EQ(deep->gesture, Gesture::Deep);
    EXPECT_TRUE(g.poll(t0 + 2s).empty()) << "no long press of either stage";
    EXPECT_FALSE(g.release(Button::BackDeep, t0 + 2s));
    EXPECT_FALSE(g.release(Button::Back, t0 + 2s));
}

TEST(GestureDetector, FirstStageAloneIsAnOrdinaryPress) {
    GestureDetector g;
    g.press(Button::Forward, t0);
    g.press(Button::ForwardDeep, t0 + 50ms);
    g.release(Button::ForwardDeep, t0 + 60ms);
    g.release(Button::Forward, t0 + 70ms);
    g.press(Button::Forward, t0 + 1s); // the next press is not silenced
    auto ev = g.release(Button::Forward, t0 + 1100ms);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->gesture, Gesture::Short);
}
