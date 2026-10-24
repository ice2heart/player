#include "gamepad.h"

#include "presses.h"

namespace oap {

namespace {

size_t index(PadControl c) { return static_cast<size_t>(c); }

} // namespace

GamepadLayout defaultGamepadLayout() {
    GamepadLayout layout;
    layout.buttons[index(PadControl::DpadUp)] = Button::Up;
    layout.buttons[index(PadControl::DpadDown)] = Button::Down;
    layout.buttons[index(PadControl::DpadLeft)] = Button::Back;
    layout.buttons[index(PadControl::DpadRight)] = Button::Forward;
    layout.buttons[index(PadControl::West)] = Button::PlayPause; // Square
    layout.buttons[index(PadControl::South)] = Button::Ok;       // Cross
    layout.buttons[index(PadControl::East)] = Button::Home;      // Circle
    layout.deep = PadControl::L1;
    return layout;
}

std::vector<ButtonChange> GamepadMapper::set(PadControl control, bool down) {
    controls_[index(control)] = down;
    return update();
}

std::vector<ButtonChange> GamepadMapper::setAxis(PadControl negative, PadControl positive, int value) {
    controls_[index(negative)] = value < 0;
    controls_[index(positive)] = value > 0;
    return update();
}

std::vector<ButtonChange> GamepadMapper::releaseAll() {
    controls_.fill(false);
    return update();
}

std::vector<ButtonChange> GamepadMapper::update() {
    std::array<bool, kButtonCount> want{};
    bool deep = layout_.deep && controls_[index(*layout_.deep)];
    for (size_t c = 0; c < kPadControlCount; ++c) {
        auto b = layout_.buttons[c];
        if (!controls_[c] || !b) continue;
        want[static_cast<size_t>(*b)] = true;
        if (auto d = deepOf(*b); d && deep) want[static_cast<size_t>(*d)] = true;
    }

    // Releases first, then presses. The deep stages come after Back and Forward
    // in Button, so walking down releases them first and walking up presses them last.
    std::vector<ButtonChange> changes;
    for (size_t b = kButtonCount; b-- > 0;)
        if (buttons_[b] && !want[b]) changes.push_back({static_cast<Button>(b), false});
    for (size_t b = 0; b < kButtonCount; ++b)
        if (!buttons_[b] && want[b]) changes.push_back({static_cast<Button>(b), true});
    buttons_ = want;
    return changes;
}

} // namespace oap
