#include "core/text.h"

#include <gtest/gtest.h>

using oap::spokenDuration;

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
