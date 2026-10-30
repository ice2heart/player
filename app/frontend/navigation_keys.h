#pragma once

#include "gamepad.h"

namespace oap {

// SDL-free keyboard navigation. Both Shift keys share the deep modifier;
// changing it while an arrow is down emits only that arrow's stage transition.
enum class NavigationKey { Back, Forward, LeftShift, RightShift };

class NavigationKeys {
public:
    std::vector<ButtonChange> set(NavigationKey key, bool down) {
        switch (key) {
        case NavigationKey::Back: return mapper_.set(PadControl::DpadLeft, down);
        case NavigationKey::Forward: return mapper_.set(PadControl::DpadRight, down);
        case NavigationKey::LeftShift: leftShift_ = down; break;
        case NavigationKey::RightShift: rightShift_ = down; break;
        }
        return mapper_.set(PadControl::L1, leftShift_ || rightShift_);
    }

    std::vector<ButtonChange> releaseAll() {
        leftShift_ = rightShift_ = false;
        return mapper_.releaseAll();
    }

private:
    GamepadMapper mapper_;
    bool leftShift_ = false;
    bool rightShift_ = false;
};

} // namespace oap
