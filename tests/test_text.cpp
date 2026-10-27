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
