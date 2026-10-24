#include "frontend/held_buttons.h"

#include <gtest/gtest.h>

using namespace oap;

TEST(HeldButtons, OneSourcePressesAndReleases) {
    HeldButtons held;
    EXPECT_FALSE(held.held(Button::Ok));
    EXPECT_TRUE(held.press(Button::Ok));
    EXPECT_TRUE(held.held(Button::Ok));
    EXPECT_TRUE(held.release(Button::Ok));
    EXPECT_FALSE(held.held(Button::Ok));
}

TEST(HeldButtons, TwoSourcesReleaseOnlyWhenBothLetGo) {
    HeldButtons held;
    EXPECT_TRUE(held.press(Button::Back));   // keyboard: App gets keyDown
    EXPECT_FALSE(held.press(Button::Back));  // pad: already down
    EXPECT_FALSE(held.release(Button::Back)); // keyboard lets go, the pad still holds it
    EXPECT_TRUE(held.held(Button::Back));
    EXPECT_TRUE(held.release(Button::Back)); // pad lets go: App gets keyUp
    EXPECT_FALSE(held.held(Button::Back));
}

TEST(HeldButtons, AReleaseWithoutAPressIsIgnored) {
    HeldButtons held;
    EXPECT_FALSE(held.release(Button::Up)); // a key held before the window had focus
    EXPECT_FALSE(held.held(Button::Up));
    EXPECT_TRUE(held.press(Button::Up));    // the count did not go below zero
    EXPECT_TRUE(held.release(Button::Up));
}
