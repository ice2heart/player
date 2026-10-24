#include "terminal_keys.h"

#include <optional>

namespace oap {

namespace {

constexpr char kEsc = '\x1b';

std::optional<Button> plainKey(char c) {
    switch (c) {
    case ' ': return Button::PlayPause;
    case '\r':
    case '\n': return Button::Ok;
    case '\x7f':
    case '\b': return Button::Home;
    case 't': return Button::Talk;
    case 'p': return Button::Power;
    default: return std::nullopt;
    }
}

std::optional<Button> arrowKey(char c) {
    switch (c) {
    case 'A': return Button::Up;
    case 'B': return Button::Down;
    case 'C': return Button::Forward;
    case 'D': return Button::Back;
    default: return std::nullopt;
    }
}

} // namespace

std::vector<TerminalKey> TerminalKeyDecoder::decode(std::string_view in) {
    std::vector<TerminalKey> keys;
    auto emit = [&](Button b, bool longPress, bool deep) {
        TerminalKey k;
        k.press.button = b;
        deep = (deep || nextDeep_) && deepOf(b);
        k.press.deep = deep;
        k.press.longPress = !deep && (longPress || nextLong_);
        nextLong_ = nextDeep_ = false;
        keys.push_back(k);
    };

    size_t i = 0;
    while (i < in.size()) {
        char c = in[i++];
        if (c == kEsc) {
            if (i == in.size()) { // lone Esc
                emit(Button::Home, false, false);
                break;
            }
            char n = in[i];
            if (n == '[' || n == 'O') { // CSI / SS3: arrows, maybe with a modifier
                size_t j = i + 1;
                int modifier = 1;
                if (in.substr(j, 2) == "1;" && j + 2 < in.size()) { // ESC [ 1 ; <mod> <letter>
                    modifier = in[j + 2] - '0';
                    j += 3;
                }
                if (j < in.size()) {
                    if (auto b = arrowKey(in[j])) {
                        // xterm modifiers: 2 Shift, 3 Alt, 4 Shift+Alt
                        emit(*b, modifier == 3 || modifier == 4, modifier == 2 || modifier == 4);
                    }
                    i = j + 1;
                } else {
                    i = in.size(); // truncated sequence: drop it
                }
                continue;
            }
            // Alt+key arrives as ESC followed by the key.
            if (auto b = plainKey(n)) emit(*b, true, false);
            ++i;
            continue;
        }
        if (c == 'h') {
            TerminalKey k;
            k.toggleHold = true;
            keys.push_back(k);
        } else if (c == 'l') {
            nextLong_ = true;
        } else if (c == 'd') {
            nextDeep_ = true;
        } else if (auto b = plainKey(c)) {
            emit(*b, false, false);
        }
    }
    return keys;
}

} // namespace oap
