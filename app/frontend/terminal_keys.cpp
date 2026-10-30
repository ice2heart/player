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
    case '+':
    case '=': return Button::VolumeUp;
    case '-': return Button::VolumeDown;
    case 'o': return Button::Options;
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

bool isParameter(char c) { return (c >= '0' && c <= '9') || c == ';'; }

} // namespace

std::vector<TerminalKey> TerminalKeyDecoder::decode(std::string_view bytes, TimePoint now) {
    std::vector<TerminalKey> keys = poll(now); // bytes left waiting too long do not join these
    pending_.append(bytes);
    pendingSince_ = now;
    parse(keys, false);
    return keys;
}

std::vector<TerminalKey> TerminalKeyDecoder::poll(TimePoint now) {
    std::vector<TerminalKey> keys;
    if (!pending_.empty() && now - pendingSince_ >= kEscTimeout) parse(keys, true);
    return keys;
}

std::vector<TerminalKey> TerminalKeyDecoder::decode(std::string_view bytes) {
    std::vector<TerminalKey> keys;
    pending_.append(bytes);
    parse(keys, true);
    return keys;
}

void TerminalKeyDecoder::emit(std::vector<TerminalKey>& keys, Button b, bool longPress, bool deep) {
    TerminalKey k;
    k.press.button = b;
    deep = (deep || nextDeep_) && deepOf(b);
    // The volume keys repeat while held, so a long hold would swing the volume by half.
    if (b == Button::VolumeUp || b == Button::VolumeDown) longPress = nextLong_ = false;
    k.press.deep = deep;
    k.press.longPress = longPress || nextLong_;
    nextLong_ = nextDeep_ = false;
    keys.push_back(k);
}

// Decodes pending_, keeping an unfinished escape sequence at its end for the
// next read. If `final`, nothing more is coming and every byte is used up.
void TerminalKeyDecoder::parse(std::vector<TerminalKey>& keys, bool final) {
    std::string_view in = pending_;
    size_t i = 0;
    while (i < in.size()) {
        size_t used = parseOne(in.substr(i), keys, final);
        if (!used) break; // wait for the rest of the sequence
        i += used;
    }
    pending_.erase(0, i);
}

// Decodes the key at the start of `in` and returns how many bytes it took, or 0
// if `in` ends inside an escape sequence and more bytes may follow.
size_t TerminalKeyDecoder::parseOne(std::string_view in, std::vector<TerminalKey>& keys, bool final) {
    char c = in[0];
    if (c != kEsc) {
        if (c == 'h') {
            TerminalKey k;
            k.toggleHold = true;
            keys.push_back(k);
        } else if (c == 'l') {
            nextLong_ = true;
        } else if (c == 'd') {
            nextDeep_ = true;
        } else if (auto b = plainKey(c)) {
            emit(keys, *b, false, false);
        }
        return 1;
    }

    if (in.size() == 1) { // Esc alone so far
        if (!final) return 0;
        emit(keys, Button::Home, false, false);
        return 1;
    }
    // rxvt and tmux send Alt+arrow as Esc followed by the arrow's own sequence.
    bool alt = false;
    size_t start = 1;
    if (in[1] == kEsc) {
        if (in.size() == 2 && !final) return 0;
        if (in.size() == 2 || (in[2] != '[' && in[2] != 'O')) { // Esc twice: the first is a key of its own
            emit(keys, Button::Home, false, false);
            return 1;
        }
        alt = true;
        start = 2;
    }

    if (in[start] == '[' || in[start] == 'O') { // CSI / SS3: arrows, maybe with a modifier
        size_t j = start + 1;
        while (j < in.size() && isParameter(in[j])) ++j;
        if (j == in.size()) return final ? in.size() : 0; // cut off: wait for the rest, or drop it
        // ESC [ 1 ; <modifier> <letter>: xterm modifiers 2 Shift, 3 Alt, 4 Shift+Alt (more add Ctrl)
        int modifier = 1;
        std::string_view params = in.substr(start + 1, j - start - 1);
        if (auto semi = params.rfind(';'); semi != std::string_view::npos) {
            modifier = 0;
            for (char d : params.substr(semi + 1)) modifier = modifier * 10 + (d - '0');
        }
        bool shift = modifier >= 2 && ((modifier - 1) & 1);
        bool altKey = modifier >= 2 && ((modifier - 1) & 2);
        if (auto b = arrowKey(in[j])) emit(keys, *b, alt || altKey, shift);
        return j + 1;
    }
    // Alt+key arrives as Esc followed by the key.
    if (auto b = plainKey(in[1])) emit(keys, *b, true, false);
    return 2;
}

} // namespace oap
