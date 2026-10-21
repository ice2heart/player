#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace oap {

enum class Button { PlayPause, Back, Forward, Up, Down, Ok, Home, Talk, Power, Count };
enum class Gesture { Short, Long };

constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);

struct ButtonEvent {
    Button button;
    Gesture gesture;
};

const char* buttonName(Button button);

// Turns raw press/release into short and long presses. A long press fires
// while the button is still held, so the user gets feedback without letting go.
class GestureDetector {
public:
    using Clock = std::chrono::steady_clock;

    void setLongPress(std::chrono::milliseconds duration) { longPress_ = duration; }
    void press(Button button, Clock::time_point now);
    std::optional<ButtonEvent> release(Button button, Clock::time_point now);
    std::vector<ButtonEvent> poll(Clock::time_point now);

private:
    struct State {
        bool down = false;
        bool longSent = false;
        Clock::time_point since;
    };
    std::array<State, kButtonCount> states_{};
    std::chrono::milliseconds longPress_{600};
};

} // namespace oap
