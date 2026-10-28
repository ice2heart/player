#include "core/text.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using oap::spokenDuration;
using oap::Script;
using oap::scriptRuns;

TEST(SpokenDuration, SecondsOnly) {
    EXPECT_EQ(spokenDuration(45), "45 seconds");
    EXPECT_EQ(spokenDuration(1), "1 second");
}

TEST(SpokenDuration, ZeroAndNegativeSayZeroSeconds) {
    EXPECT_EQ(spokenDuration(0), "0 seconds");
    EXPECT_EQ(spokenDuration(-12), "0 seconds");
}

TEST(SpokenDuration, MinutesAndSeconds) {
    EXPECT_EQ(spokenDuration(200), "3 minutes 20 seconds");
    EXPECT_EQ(spokenDuration(60), "1 minute");
}

TEST(SpokenDuration, SecondsDroppedOverAnHour) {
    EXPECT_EQ(spokenDuration(3600 + 4 * 60 + 59), "1 hour 4 minutes");
    EXPECT_EQ(spokenDuration(2 * 3600), "2 hours");
}

TEST(SpokenDuration, RoundsToNearestSecond) {
    EXPECT_EQ(spokenDuration(59.6), "1 minute");
}

TEST(ParseIsoTime, ReadsATimeWithFractionAndZ) {
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T18:01:44.120Z"), 1792519304);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T18:01:44Z"), 1792519304);
    EXPECT_EQ(oap::parseIsoTime("1970-01-01T00:00:00Z"), 0 + 0) << "the epoch is 0, which also means unknown";
}

TEST(ParseIsoTime, HandlesLeapDaysAndZoneOffsets) {
    EXPECT_EQ(oap::parseIsoTime("2024-02-29T23:59:59Z"), 1709251199);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T20:01:44+02:00"), 1792519304);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T13:31:44-0430"), 1792519304);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T18:01:44"), 1792519304) << "no zone: UTC";
}

TEST(ParseIsoTime, ADateAloneIsMidnight) {
    EXPECT_EQ(oap::parseIsoTime("2026-10-20"), 1792519304 - (18 * 3600 + 60 + 44));
}

TEST(ParseIsoTime, SomethingElseIsZero) {
    EXPECT_EQ(oap::parseIsoTime(""), 0);
    EXPECT_EQ(oap::parseIsoTime("yesterday"), 0);
    EXPECT_EQ(oap::parseIsoTime("2026-13-20T18:01:44Z"), 0);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T25:01:44Z"), 0);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T24:00:00Z"), 0);
    EXPECT_EQ(oap::parseIsoTime("2026-00-20T18:01:44Z"), 0);
    EXPECT_EQ(oap::parseIsoTime("2026-10-20T18:01:44Zjunk"), 0);
}

TEST(FoldCase, LowersAsciiLatin1ExtendedAGreekAndCyrillicButKeepsAccents) {
    EXPECT_EQ(oap::foldCase('A'), U'a');
    EXPECT_EQ(oap::foldCase(U'É'), U'é') << "the accent stays";
    EXPECT_NE(oap::foldCase(U'É'), U'e');
    EXPECT_EQ(oap::foldCase(U'×'), U'×');
    EXPECT_EQ(oap::foldCase(U'Ł'), U'ł');
    EXPECT_EQ(oap::foldCase(U'Ź'), U'ź');
    EXPECT_EQ(oap::foldCase(U'Σ'), U'σ');
    EXPECT_EQ(oap::foldCase(U'Ά'), U'ά');
    EXPECT_EQ(oap::foldCase(U'Я'), U'я');
    EXPECT_EQ(oap::foldCase(U'Ѐ'), U'ѐ');
    EXPECT_EQ(oap::foldCase(U'漢'), U'漢') << "other scripts are left alone";
}

TEST(FoldCase, YoIsYeTonosCapitalsFoldAndFinalSigmaIsSigma) {
    EXPECT_EQ(oap::foldCase(U'Ё'), U'е');
    EXPECT_EQ(oap::foldCase(U'ё'), U'е');
    EXPECT_EQ(oap::foldCase(U'Ό'), U'ό');
    EXPECT_EQ(oap::foldCase(U'Ύ'), U'ύ');
    EXPECT_EQ(oap::foldCase(U'Ώ'), U'ώ');
    EXPECT_EQ(oap::foldCase(U'ς'), U'σ');
}

TEST(Utf8, CodePointsRoundTripAndMalformedBytesStandForThemselves) {
    for (char32_t c : {U'a', U'é', U'Ж', U'漢', char32_t(0x1F600)}) {
        std::string s;
        oap::appendUtf8(s, c);
        size_t i = 0;
        EXPECT_EQ(oap::nextCodePoint(s, i), c);
        EXPECT_EQ(i, s.size());
    }
    std::string bad = "\xC3";
    size_t i = 0;
    EXPECT_EQ(oap::nextCodePoint(bad, i), char32_t(0xC3)) << "truncated sequence";
    EXPECT_EQ(i, 1u);
    std::string bad2 = "\xC3(";
    i = 0;
    EXPECT_EQ(oap::nextCodePoint(bad2, i), char32_t(0xC3)) << "bad continuation byte";
    EXPECT_EQ(i, 1u);
}

namespace {
using Runs = std::vector<std::pair<Script, std::string>>;
Runs runsOf(const std::string& s) {
    Runs out;
    for (auto& r : scriptRuns(s)) out.emplace_back(r.script, r.text);
    return out;
}
} // namespace

TEST(ScriptRuns, EmptyTextHasNoRuns) { EXPECT_TRUE(scriptRuns("").empty()); }

TEST(ScriptRuns, PlainEnglishIsOneLatinRun) {
    EXPECT_EQ(runsOf("Hello, world 42!"), (Runs{{Script::Latin, "Hello, world 42!"}}));
    EXPECT_EQ(runsOf("Zażółć Été"), (Runs{{Script::Latin, "Zażółć Été"}}));
    EXPECT_EQ(runsOf("ǆ"), (Runs{{Script::Latin, "ǆ"}}));
    EXPECT_EQ(runsOf("Кино×Кино"), (Runs{{Script::Cyrillic, "Кино×Кино"}}));
    EXPECT_EQ(runsOf("Кино ԁ"), (Runs{{Script::Cyrillic, "Кино ԁ"}}));
}

TEST(ScriptRuns, ACyrillicNameInAnEnglishSentenceIsCutOut) {
    EXPECT_EQ(runsOf("Artist Агата Кристи, 3 of 10"),
              (Runs{{Script::Latin, "Artist "}, {Script::Cyrillic, "Агата Кристи"}, {Script::Latin, ", 3 of 10"}}));
}

TEST(ScriptRuns, SpacesBetweenWordsOfOneScriptStayInTheRun) {
    EXPECT_EQ(runsOf("Кино и Ко"), (Runs{{Script::Cyrillic, "Кино и Ко"}}));
    EXPECT_EQ(runsOf("Кино - «Звезда», 2"),
              (Runs{{Script::Cyrillic, "Кино - «Звезда"}, {Script::Latin, "», 2"}}));
}

TEST(ScriptRuns, ADigitAfterAWordGoesToTheLatinRun) {
    EXPECT_EQ(runsOf("Кино 1989"), (Runs{{Script::Cyrillic, "Кино"}, {Script::Latin, " 1989"}}));
}

TEST(ScriptRuns, LeadingPunctuationJoinsTheLatinRun) {
    EXPECT_EQ(runsOf("\"Кино\""), (Runs{{Script::Latin, "\""}, {Script::Cyrillic, "Кино"}, {Script::Latin, "\""}}));
}

TEST(ScriptRuns, PunctuationBetweenTwoScriptsIsLatin) {
    EXPECT_EQ(runsOf("Кино Ελλάδα"),
              (Runs{{Script::Cyrillic, "Кино"}, {Script::Latin, " "}, {Script::Greek, "Ελλάδα"}}));
}

TEST(ScriptRuns, GreekAndHangulAndHebrewAndArabicHaveTheirOwnRuns) {
    EXPECT_EQ(runsOf("Μίκης Θεοδωράκης"), (Runs{{Script::Greek, "Μίκης Θεοδωράκης"}}));
    EXPECT_EQ(runsOf("방탄소년단"), (Runs{{Script::Hangul, "방탄소년단"}}));
    EXPECT_EQ(runsOf("שלום"), (Runs{{Script::Hebrew, "שלום"}}));
    EXPECT_EQ(runsOf("مرحبا"), (Runs{{Script::Arabic, "مرحبا"}}));
    EXPECT_EQ(runsOf("a\xE0\xA4\x85"), (Runs{{Script::Latin, "a"}, {Script::Other, "\xE0\xA4\x85"}}));
}

TEST(ScriptRuns, KanaWithHanIsOneJapaneseRun) {
    EXPECT_EQ(runsOf("宇多田ヒカル"), (Runs{{Script::Japanese, "宇多田ヒカル"}}));
    EXPECT_EQ(runsOf("ひらがな"), (Runs{{Script::Japanese, "ひらがな"}}));
    EXPECT_EQ(runsOf("東京ガール"), (Runs{{Script::Japanese, "東京ガール"}}));
    EXPECT_EQ(runsOf("ヒカル宇多田"), (Runs{{Script::Japanese, "ヒカル宇多田"}}));
}

TEST(ScriptRuns, HanAloneIsHan) {
    EXPECT_EQ(runsOf("周杰伦"), (Runs{{Script::Han, "周杰伦"}}));
    EXPECT_EQ(runsOf("周 杰伦"), (Runs{{Script::Han, "周 杰伦"}}));
    EXPECT_EQ(runsOf("龍"), (Runs{{Script::Han, "龍"}}));
}

TEST(ScriptRuns, PunctuationBetweenHanAndKanaStaysInTheJapaneseRun) {
    EXPECT_EQ(runsOf("ひらがな、漢字"), (Runs{{Script::Japanese, "ひらがな、漢字"}}));
    EXPECT_EQ(runsOf("ヒカル・宇多田"), (Runs{{Script::Japanese, "ヒカル・宇多田"}}));
    EXPECT_EQ(runsOf("周 ヒ"), (Runs{{Script::Japanese, "周 ヒ"}}));
    EXPECT_EQ(runsOf("人々は"), (Runs{{Script::Japanese, "人々は"}}));
    EXPECT_EQ(runsOf("𠀋ヒ"), (Runs{{Script::Japanese, "𠀋ヒ"}}));
}

TEST(ScriptRuns, JapanesePunctuationAtTheEdgesGoesToTheLatinRun) {
    EXPECT_EQ(runsOf("「宇多田」"), (Runs{{Script::Latin, "「"}, {Script::Han, "宇多田"}, {Script::Latin, "」"}}));
    EXPECT_EQ(runsOf("Ａ宇"), (Runs{{Script::Latin, "Ａ"}, {Script::Han, "宇"}}));
    EXPECT_EQ(runsOf("宇\xEF\xBF\xA5"), (Runs{{Script::Han, "宇"}, {Script::Latin, "\xEF\xBF\xA5"}}));
    EXPECT_EQ(runsOf("宇㈱"), (Runs{{Script::Han, "宇"}, {Script::Latin, "㈱"}}));
    EXPECT_EQ(runsOf("宇\xEF\xB9\x8F"), (Runs{{Script::Han, "宇"}, {Script::Latin, "\xEF\xB9\x8F"}}));
}

TEST(ScriptRuns, DigitsAndEmojiAreNotLetters) {
    EXPECT_EQ(runsOf("Кино ٣ Кино"), (Runs{{Script::Cyrillic, "Кино ٣ Кино"}}));
    EXPECT_EQ(runsOf("Кино 😀 Кино"), (Runs{{Script::Cyrillic, "Кино 😀 Кино"}}));
}

TEST(ScriptRuns, SymbolsAndDashesDoNotBreakARun) {
    EXPECT_EQ(runsOf("Кино — Кино"), (Runs{{Script::Cyrillic, "Кино — Кино"}}));
    EXPECT_EQ(runsOf("Кино → Кино"), (Runs{{Script::Cyrillic, "Кино → Кино"}}));
    EXPECT_EQ(runsOf("Кино・Кино"), (Runs{{Script::Cyrillic, "Кино・Кино"}}));
}

TEST(ScriptRuns, IterationMarksAndHalfwidthKanaAreLetters) {
    EXPECT_EQ(runsOf("宇々"), (Runs{{Script::Han, "宇々"}}));
    EXPECT_EQ(runsOf("宇〇"), (Runs{{Script::Han, "宇〇"}}));
    EXPECT_EQ(runsOf("ﾋｶﾙ"), (Runs{{Script::Japanese, "ﾋｶﾙ"}}));
}

TEST(ScriptRuns, ALigatureIsLatin) { EXPECT_EQ(runsOf("Кино ﬁnal"), (Runs{{Script::Cyrillic, "Кино"}, {Script::Latin, " ﬁnal"}})); }

TEST(ScriptRuns, AMarkStaysWithItsLetter) {
    EXPECT_EQ(runsOf("Cafe\xCC\x81 Кино"), (Runs{{Script::Latin, "Cafe\xCC\x81 "}, {Script::Cyrillic, "Кино"}}));
    EXPECT_EQ(runsOf("Кино\xCC\x81 Кино"), (Runs{{Script::Cyrillic, "Кино\xCC\x81 Кино"}}));
    EXPECT_EQ(runsOf("宇\xEF\xB8\x80ヒ"), (Runs{{Script::Japanese, "宇\xEF\xB8\x80ヒ"}}));
    EXPECT_EQ(runsOf("宇\xF3\xA0\x84\x80ヒ"), (Runs{{Script::Japanese, "宇\xF3\xA0\x84\x80ヒ"}}));
    EXPECT_EQ(runsOf("\xCC\x81Кино"), (Runs{{Script::Latin, "\xCC\x81"}, {Script::Cyrillic, "Кино"}}));
    EXPECT_EQ(runsOf("Кино \xCC\x81 Кино"), (Runs{{Script::Cyrillic, "Кино \xCC\x81 Кино"}}));
    EXPECT_EQ(runsOf("Кʰ"), (Runs{{Script::Cyrillic, "Кʰ"}}));
}

TEST(ScriptRuns, ABrokenByteDoesNotCrashAndStaysInItsRun) {
    EXPECT_EQ(runsOf("Ки\xFFно"), (Runs{{Script::Cyrillic, "Ки\xFFно"}}));
    EXPECT_EQ(runsOf("ab\xE9" "cd"), (Runs{{Script::Latin, "ab\xE9" "cd"}}));
    EXPECT_EQ(runsOf("Кино\xED\xA0\x80Кино"), (Runs{{Script::Cyrillic, "Кино\xED\xA0\x80Кино"}}));
    EXPECT_EQ(runsOf("Кино\xF4\x90\x80\x80Кино"), (Runs{{Script::Cyrillic, "Кино\xF4\x90\x80\x80Кино"}}));
    EXPECT_EQ(runsOf("Кино\xD0"), (Runs{{Script::Cyrillic, "Кино"}, {Script::Latin, "\xD0"}}));
}
