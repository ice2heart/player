#pragma once

#include "presses.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace oap {

// A terminal (for example over SSH) sends bytes for key presses but never releases,
// so each key becomes a whole press for PressSequencer:
//
//   Space play/pause   arrows Back/Forward/Up/Down   Enter OK   Esc/Backspace Home
//   t Talk   p Power   h hold switch (toggle)   + (or =) / - volume up / down   o Options
//   (a terminal never releases a key, so a volume key steps once and has no long press)
//   Alt+key, or l then key: long press     Shift+Left/Right, or d then Left/Right: deep press
struct TerminalKey {
    PressStep press;         // valid unless toggleHold
    bool toggleHold = false;
};

class TerminalKeyDecoder {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    // How long a lone Esc waits for the rest of an escape sequence before it
    // counts as the Esc key. Over a slow link a sequence can arrive in pieces.
    static constexpr std::chrono::milliseconds kEscTimeout{200};

    // `bytes` is one read() from the terminal at time `now`. An escape sequence
    // cut off at the end of the read is kept for the next read.
    std::vector<TerminalKey> decode(std::string_view bytes, TimePoint now);
    // Call every tick: once no bytes have come for kEscTimeout, a waiting lone
    // Esc becomes Home and an unfinished sequence is dropped.
    std::vector<TerminalKey> poll(TimePoint now);

    // Older form without time: every read is taken as complete, so a lone Esc at
    // its end is Home at once and a sequence split across reads is lost.
    std::vector<TerminalKey> decode(std::string_view bytes);

private:
    void parse(std::vector<TerminalKey>& keys, bool final);
    size_t parseOne(std::string_view in, std::vector<TerminalKey>& keys, bool final);
    void emit(std::vector<TerminalKey>& keys, Button b, bool longPress, bool deep);

    std::string pending_;   // bytes not decoded yet: an unfinished escape sequence
    TimePoint pendingSince_{};
    bool nextLong_ = false; // after 'l'
    bool nextDeep_ = false; // after 'd'
};

} // namespace oap
