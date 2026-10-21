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
