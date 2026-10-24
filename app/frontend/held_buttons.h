#pragma once

#include "core/buttons.h"

#include <array>

namespace oap {

// Counts the sources (keyboard keys, each game controller, on-screen buttons,
// scripted presses) that hold each device button, so that App sees one press
// from the first source going down to the last one coming up. Without it a
// keyboard key and a pad both holding Left would end the press at the first release.
class HeldButtons {
public:
    // True for the first holder: the caller then calls App::keyDown.
    bool press(Button b);
    // True when the last holder lets go: the caller then calls App::keyUp.
    // A release without a press (a key held before the window had focus) is ignored.
    bool release(Button b);
    bool held(Button b) const { return count_[static_cast<size_t>(b)] > 0; }

private:
    std::array<int, kButtonCount> count_{};
};

} // namespace oap
