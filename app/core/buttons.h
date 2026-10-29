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
// Light: a deep navigation contact opened while the light contact remains down.
// Release: the last navigation contact opened after a hold/deep gesture.
// Cancel: a navigation gesture was abandoned; its contacts stay suppressed until up.
enum class Gesture { Short, Long, Deep, Repeat, Light, Release, Cancel, Count };

constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);
constexpr size_t kGestureCount = static_cast<size_t>(Gesture::Count);

struct ButtonEvent {
    Button button;
    Gesture gesture;
};

const char* buttonName(Button button);

// Turns raw contacts into gestures. Back/Forward have one lifecycle shared by
// both contacts: Deep is immediate, Long uses the initial press deadline even
// after Deep, Light reports easing, and Release ends the gesture without Short.
// Duplicate downs are ignored; the final contact up ends the lifecycle in either
// release order. Volume keeps its immediate Short and delayed Repeat behavior.
class GestureDetector {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kRepeatDelay{400};
    static constexpr std::chrono::milliseconds kRepeatInterval{150};

    void setLongPress(std::chrono::milliseconds duration) { longPress_ = duration; }
    std::optional<ButtonEvent> press(Button button, Clock::time_point now); // a Deep, or a repeating key's Short
    std::optional<ButtonEvent> release(Button button, Clock::time_point now);
    std::vector<ButtonEvent> poll(Clock::time_point now);
    // Suppress currently held contacts until up. Hold locking preserves Power.
    std::vector<ButtonEvent> cancel(bool preservePower = false);

private:
    struct State {
        bool down = false;
        bool consumed = false; // suppress Short after a long/deep action
        bool longFired = false; // Long fires once, including after Deep
        bool cancelled = false; // ignored until the last contact is released
        Clock::time_point since;
        Clock::time_point nextRepeat; // a repeating key: when the next Repeat is due
    };
    std::array<State, kButtonCount> states_{};
    std::chrono::milliseconds longPress_{600};
};

} // namespace oap
