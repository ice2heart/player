#include "core/scrobble_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace oap;

TEST(ScrobbleFile, EntriesComeBackInOrder) {
    std::vector<ScrobbleEntry> entries = {{"host-1", "t1", 1700000000000}, {"other-2", "t2", 1700000001000}};
    EXPECT_EQ(parseScrobbles(formatScrobbles(entries)), entries);
}

TEST(ScrobbleFile, NoEntriesIsAnEmptyFile) {
    EXPECT_EQ(formatScrobbles({}), "");
    EXPECT_TRUE(parseScrobbles("").empty());
}

TEST(ScrobbleFile, TabsAndLineBreaksInAnIdSurvive) {
    std::vector<ScrobbleEntry> entries = {{"host-1", "a\tb\nc\\d\re", 5}};
    const std::string text = formatScrobbles(entries);
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2) << "header and one line";
    EXPECT_EQ(parseScrobbles(text), entries);
}

TEST(ScrobbleFile, AFileLongerThanTheLimitIsCutToTheNewestEntriesWhenItIsRead) {
    std::string text = "oap-scrobbles\t1\n";
    for (std::size_t i = 0; i < kMaxScrobbles + 1; ++i) text += "s\tt" + std::to_string(i) + "\t" + std::to_string(i + 1) + "\n";
    const auto back = parseScrobbles(text);
    ASSERT_EQ(back.size(), kMaxScrobbles);
    EXPECT_EQ(back.front().trackId, "t1");
    EXPECT_EQ(back.back().trackId, "t" + std::to_string(kMaxScrobbles));
}

TEST(ScrobbleFile, DamagedLinesAreSkippedAndAnotherFileGivesNothing) {
    const auto back = parseScrobbles("oap-scrobbles\t1\nbroken\ns\tt1\tnotatime\ns\tt2\t0\n\tt3\t5\ns\tt4\t7\n");
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0], (ScrobbleEntry{"s", "t4", 7}));
    EXPECT_TRUE(parseScrobbles("oap-scrobbles\t1\ns\tt\t 12\ns\tt\t+12\ns\tt\t12x\ns\tt\t99999999999999999999\n").empty())
        << "a time is digits that fit";
    EXPECT_TRUE(parseScrobbles("oap-scrobbles\t2\ns\tt1\t5\n").empty()) << "another version";
    EXPECT_TRUE(parseScrobbles("s\tt1\t5\n").empty()) << "no header";
    std::string tooLong = "oap-scrobbles\t1\n";
    while (tooLong.size() <= kMaxScrobbleFileBytes) tooLong += "s\tt\t5\n"; // each line is fine
    EXPECT_TRUE(parseScrobbles(tooLong).empty()) << "too long";
}
