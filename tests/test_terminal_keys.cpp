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

TEST(TerminalKeys, VolumeAndOptionsKeys) {
    EXPECT_EQ(decode("+"), "Volume up");
    EXPECT_EQ(decode("="), "Volume up");
    EXPECT_EQ(decode("-"), "Volume down");
    EXPECT_EQ(decode("o"), "Options");
    EXPECT_EQ(decode("\x1b+"), "Volume up") << "held long, a volume key would repeat";
    EXPECT_EQ(decode("l-"), "Volume down");
    EXPECT_EQ(decode("lo"), "Options long");
    EXPECT_EQ(decode("d+"), "Volume up") << "volume keys have no deep stage";
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

TEST(TerminalKeys, ShiftAltArrowHoldsTheDeepStageElseHoldsNormally) {
    EXPECT_EQ(decode("\x1b[1;4C"), "Forward long deep");
    EXPECT_EQ(decode("\x1b[1;4D"), "Back long deep");
    EXPECT_EQ(decode("\x1b[1;4A"), "Up long");
}

TEST(TerminalKeys, TruncatedSequenceIsDropped) {
    EXPECT_EQ(decode("\x1b["), "");
    EXPECT_EQ(decode("\x1b[1;"), "");
    EXPECT_EQ(decode("\x1b[1;2"), "");
}

TEST(TerminalKeys, PrefixesLAndDTogether) {
    EXPECT_EQ(decode("ld\x1b[C"), "Forward long deep"); // both modifiers apply
    EXPECT_EQ(decode("dl\x1b[D"), "Back long deep");
    EXPECT_EQ(decode("dl\x1b[A"), "Up long");
    EXPECT_EQ(decode("ld "), "Play/Pause long");
}

// Reads with times, as over SSH, where a sequence can arrive in pieces.
class TerminalKeysTimed : public testing::Test {
protected:
    std::string read(std::string_view bytes, std::chrono::milliseconds at) {
        std::string out;
        for (const auto& k : d.decode(bytes, t0 + at)) out += (out.empty() ? "" : ", ") + describe(k);
        return out;
    }
    std::string poll(std::chrono::milliseconds at) {
        std::string out;
        for (const auto& k : d.poll(t0 + at)) out += (out.empty() ? "" : ", ") + describe(k);
        return out;
    }

    TerminalKeyDecoder d;
    TerminalKeyDecoder::TimePoint t0{std::chrono::hours(1)};
};

using std::chrono::milliseconds;
constexpr auto kTimeout = TerminalKeyDecoder::kEscTimeout;

TEST_F(TerminalKeysTimed, LoneEscIsHomeOnlyAfterTheTimeout) {
    EXPECT_EQ(read("\r\x1b", milliseconds(0)), "OK");
    EXPECT_EQ(poll(kTimeout - milliseconds(1)), "");
    EXPECT_EQ(poll(kTimeout), "Home");
    EXPECT_EQ(poll(kTimeout * 2), ""); // only once
}

TEST_F(TerminalKeysTimed, EscThenTheRestOfAnArrowInTheNextRead) {
    EXPECT_EQ(read("\x1b", milliseconds(0)), "");
    EXPECT_EQ(poll(milliseconds(50)), "");
    EXPECT_EQ(read("[A", milliseconds(60)), "Up");
    EXPECT_EQ(poll(milliseconds(60) + kTimeout), ""); // no Home after it
}

TEST_F(TerminalKeysTimed, SequenceCutAfterTheBracketOrTheModifier) {
    EXPECT_EQ(read("\x1b[", milliseconds(0)), "");
    EXPECT_EQ(read("B", milliseconds(10)), "Down");
    EXPECT_EQ(read(" \x1b[1;", milliseconds(20)), "Play/Pause");
    EXPECT_EQ(read("2C", milliseconds(30)), "Forward deep");
}

TEST_F(TerminalKeysTimed, AnUnfinishedSequenceIsDroppedAfterTheTimeout) {
    EXPECT_EQ(read("\x1b[1;", milliseconds(0)), "");
    EXPECT_EQ(poll(kTimeout), "");
    EXPECT_EQ(read("C", kTimeout + milliseconds(10)), ""); // a plain C, not the end of the arrow
}

TEST_F(TerminalKeysTimed, EscLeftWaitingTooLongIsHomeEvenWithoutPoll) {
    EXPECT_EQ(read("\x1b", milliseconds(0)), "");
    EXPECT_EQ(read("t", kTimeout), "Home, Talk");
}

TEST_F(TerminalKeysTimed, EscBeforeAnArrowIsAltArrowAsInRxvtAndTmux) {
    EXPECT_EQ(read("\x1b\x1b[D", milliseconds(0)), "Back long");
    EXPECT_EQ(read("\x1b", milliseconds(10)), "");
    EXPECT_EQ(read("\x1b", milliseconds(20)), "");
    EXPECT_EQ(read("[C", milliseconds(30)), "Forward long");
}

TEST_F(TerminalKeysTimed, EscPressedTwiceIsHomeTwice) {
    EXPECT_EQ(read("\x1b\x1b", milliseconds(0)), "");
    EXPECT_EQ(poll(kTimeout), "Home, Home");
    EXPECT_EQ(read("\x1b\x1b ", kTimeout * 2), "Home, Play/Pause long");
}
