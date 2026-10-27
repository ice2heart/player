#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace oap {

// BackDeep / ForwardDeep are the second stage of two-stage Left / Right keys
// (pressed further in). They are raw inputs only: GestureDetector reports them as
// a Deep gesture on Back / Forward, so App never sees these two buttons.
//
// VolumeUp / VolumeDown change the content volume on every screen. Options opens the
// actions of the selected item. VolumeUp / VolumeDown also repeat while held (see
// GestureDetector); they come after the deep stages so that gamepad releases still
// let go of a deep stage before its light stage.
enum class Button { PlayPause, Back, Forward, Up, Down, Ok, Home, Talk, Power, BackDeep, ForwardDeep,
                    VolumeUp, VolumeDown, Options, Count };
// Repeat is only sent for a key that repeats while held (the volume keys), after the
// Short that its press sends at once.
enum class Gesture { Short, Long, Deep, Repeat, Count };

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
// release that follows is then ignored, as after a long press, and so is a light
// press while the deep stage is down (stages out of order, or contact bounce).
//
// A key that repeats (the volume keys) sends a Short as soon as it goes down, so the
// first step is heard at once, then a Repeat every kRepeatInterval once it has been
// held for kRepeatDelay, until it is let go. It has no long press and its release says
// nothing. The delay keeps a slow tap from stepping twice.
class GestureDetector {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kRepeatDelay{400};
    static constexpr std::chrono::milliseconds kRepeatInterval{150};

    void setLongPress(std::chrono::milliseconds duration) { longPress_ = duration; }
    std::optional<ButtonEvent> press(Button button, Clock::time_point now); // a Deep, or a repeating key's Short
    std::optional<ButtonEvent> release(Button button, Clock::time_point now);
    std::vector<ButtonEvent> poll(Clock::time_point now);

private:
    struct State {
        bool down = false;
        bool consumed = false; // a long or deep press already fired: the release says nothing
        bool longFired = false; // a long press fired during this hold
        Clock::time_point since;
        Clock::time_point nextRepeat; // a repeating key: when the next Repeat is due
    };
    std::array<State, kButtonCount> states_{};
    std::chrono::milliseconds longPress_{600};
};

} // namespace oap
