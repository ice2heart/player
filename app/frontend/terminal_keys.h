#pragma once

#include "presses.h"

#include <string_view>
#include <vector>

namespace oap {

// A terminal (for example over SSH) sends bytes for key presses but never releases,
// so each key becomes a whole press for PressSequencer:
//
//   Space play/pause   arrows Back/Forward/Up/Down   Enter OK   Esc/Backspace Home
//   t Talk   p Power   h hold switch (toggle)
//   Alt+key, or l then key: long press     Shift+Left/Right, or d then Left/Right: deep press
struct TerminalKey {
    PressStep press;         // valid unless toggleHold
    bool toggleHold = false;
};

class TerminalKeyDecoder {
public:
    // `bytes` is one read() from the terminal. An escape sequence arrives in one read,
    // so a lone Esc at the end of the chunk is the Esc key itself.
    std::vector<TerminalKey> decode(std::string_view bytes);

private:
    bool nextLong_ = false; // after 'l'
    bool nextDeep_ = false; // after 'd'
};

} // namespace oap
