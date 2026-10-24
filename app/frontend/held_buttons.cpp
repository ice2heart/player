#include "held_buttons.h"

namespace oap {

bool HeldButtons::press(Button b) {
    return ++count_[static_cast<size_t>(b)] == 1;
}

bool HeldButtons::release(Button b) {
    int& n = count_[static_cast<size_t>(b)];
    if (n == 0) return false;
    return --n == 0;
}

} // namespace oap
