#pragma once

#include "core/buttons.h"

#include <array>
#include <optional>
#include <vector>

namespace oap {

// Controls on a game controller, named by position as Linux names them: South
// is Cross on a PlayStation pad and A on an Xbox pad, West is Square or X.
enum class PadControl {
    DpadUp, DpadDown, DpadLeft, DpadRight,
    South, East, West, North,
    L1, R1, L2, R2, Select, Start, Mode, ThumbL, ThumbR,
    Count
};

constexpr size_t kPadControlCount = static_cast<size_t>(PadControl::Count);

// Which control presses which device button.
struct GamepadLayout {
    std::array<std::optional<Button>, kPadControlCount> buttons{};
    // Held together with a control mapped to Back or Forward, presses that key's
    // deep stage too (docs/navigation.md, two-stage keys).
    std::optional<PadControl> deep;
};

// PlayStation layout. Edit this to remap:
//   D-pad   Up/Down/Back/Forward      Square  Play/Pause
//   Cross   OK                        Circle  Home
//   Triangle Options                  R1 / R2  Volume up / down
//   L1 + D-pad Left/Right             deep press (previous / next chapter)
GamepadLayout defaultGamepadLayout();

struct ButtonChange {
    Button button;
    bool down;
};

// Turns gamepad controls into device-button presses and releases for
// App::keyDown / keyUp. A deep stage goes down after its light stage and comes
// up before it, as on a real two-stage key.
class GamepadMapper {
public:
    GamepadMapper() : GamepadMapper(defaultGamepadLayout()) {}
    explicit GamepadMapper(GamepadLayout layout) : layout_(layout) {}

    std::vector<ButtonChange> set(PadControl control, bool down);
    // A D-pad reported as an axis (hat): -1 presses `negative`, 1 `positive`, 0 neither.
    std::vector<ButtonChange> setAxis(PadControl negative, PadControl positive, int value);
    // Lets go of everything, for a controller that disconnects mid-press.
    std::vector<ButtonChange> releaseAll();

private:
    std::vector<ButtonChange> update();

    GamepadLayout layout_;
    std::array<bool, kPadControlCount> controls_{};
    std::array<bool, kButtonCount> buttons_{};
};

} // namespace oap
