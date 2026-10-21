#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace oap {

// BackDeep / ForwardDeep are the second stage of two-stage Left / Right keys
// (pressed further in). They are raw inputs only: GestureDetector reports them as
// a Deep gesture on Back / Forward, so App never sees these two buttons.
enum class Button { PlayPause, Back, Forward, Up, Down, Ok, Home, Talk, Power, BackDeep, ForwardDeep, Count };
enum class Gesture { Short, Long, Deep, Count };

constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);
constexpr size_t kGestureCount = static_cast<size_t>(Gesture::Count);

struct ButtonEvent {
    Button button;
    Gesture gesture;
};

const char* buttonName(Button button);

// Turns raw press/release into short, long and deep presses. A long press fires
// while the button is still held, so the user gets feedback without letting go.
// A deep press fires as soon as the second stage closes. The first stage's
// release that follows is then ignored, as after a long press.
class GestureDetector {
public:
    using Clock = std::chrono::steady_clock;

    void setLongPress(std::chrono::milliseconds duration) { longPress_ = duration; }
    std::optional<ButtonEvent> press(Button button, Clock::time_point now); // a Deep event, if any
    std::optional<ButtonEvent> release(Button button, Clock::time_point now);
    std::vector<ButtonEvent> poll(Clock::time_point now);

private:
    struct State {
        bool down = false;
        bool consumed = false; // a long or deep press already fired: the release says nothing
        Clock::time_point since;
    };
    std::array<State, kButtonCount> states_{};
    std::chrono::milliseconds longPress_{600};
};

} // namespace oap
