#include "frontend/terminal_keys.h"

#include <gtest/gtest.h>

#include <string>

using namespace oap;

namespace {

// One decoded key as text: "Ok", "Back long", "Forward deep", "hold".
std::string describe(const TerminalKey& k) {
    if (k.toggleHold) return "hold";
    std::string s = buttonName(*k.press.button);
    if (k.press.longPress) s += " long";
    if (k.press.deep) s += " deep";
    return s;
}

std::string decode(TerminalKeyDecoder& d, std::string_view bytes) {
    std::string out;
    for (const auto& k : d.decode(bytes)) out += (out.empty() ? "" : ", ") + describe(k);
    return out;
}

std::string decode(std::string_view bytes) {
    TerminalKeyDecoder d;
    return decode(d, bytes);
}

} // namespace

TEST(TerminalKeys, PlainKeysAreShortPresses) {
    EXPECT_EQ(decode(" "), "Play/Pause");
    EXPECT_EQ(decode("\r"), "OK");
    EXPECT_EQ(decode("\n"), "OK");
    EXPECT_EQ(decode("\x7f"), "Home");
    EXPECT_EQ(decode("t"), "Talk");
    EXPECT_EQ(decode("p"), "Power");
}

TEST(TerminalKeys, ArrowsMapToDirectionButtons) {
    EXPECT_EQ(decode("\x1b[A"), "Up");
    EXPECT_EQ(decode("\x1b[B"), "Down");
    EXPECT_EQ(decode("\x1b[C"), "Forward");
    EXPECT_EQ(decode("\x1b[D"), "Back");
    EXPECT_EQ(decode("\x1bOC"), "Forward"); // application cursor mode
}

TEST(TerminalKeys, SeveralKeysInOneRead) {
    EXPECT_EQ(decode("\x1b[B\x1b[B\r "), "Down, Down, OK, Play/Pause");
}

TEST(TerminalKeys, LoneEscIsHomeButEscStartingASequenceIsNot) {
    EXPECT_EQ(decode("\x1b"), "Home");
    EXPECT_EQ(decode("\r\x1b"), "OK, Home");
    EXPECT_EQ(decode("\x1b[A\x1b"), "Up, Home");
}

TEST(TerminalKeys, ShiftLeftRightIsADeepPress) {
    EXPECT_EQ(decode("\x1b[1;2C"), "Forward deep");
    EXPECT_EQ(decode("\x1b[1;2D"), "Back deep");
    EXPECT_EQ(decode("\x1b[1;2A"), "Up"); // Up has no deep stage
}

TEST(TerminalKeys, AltKeyIsALongPress) {
    EXPECT_EQ(decode("\x1b[1;3D"), "Back long");
    EXPECT_EQ(decode("\x1b "), "Play/Pause long");
    EXPECT_EQ(decode("\x1bp"), "Power long");
    EXPECT_EQ(decode("\x1b\x7f"), "Home long");
}

TEST(TerminalKeys, PrefixLMakesOnlyTheNextKeyLong) {
    TerminalKeyDecoder d;
    EXPECT_EQ(decode(d, "l"), ""); // the prefix may arrive in its own read
    EXPECT_EQ(decode(d, " "), "Play/Pause long");
    EXPECT_EQ(decode(d, " "), "Play/Pause");
}

TEST(TerminalKeys, PrefixDMakesLeftRightDeep) {
    TerminalKeyDecoder d;
    EXPECT_EQ(decode(d, "d\x1b[C"), "Forward deep");
    EXPECT_EQ(decode(d, "\x1b[C"), "Forward");
    EXPECT_EQ(decode(d, "d\x1b[A\x1b[A"), "Up, Up");
}

TEST(TerminalKeys, HTogglesHoldAndOtherBytesAreIgnored) {
    EXPECT_EQ(decode("h"), "hold");
    EXPECT_EQ(decode("xyz"), "");
    EXPECT_EQ(decode("\x1b[Z"), ""); // Shift+Tab
}
