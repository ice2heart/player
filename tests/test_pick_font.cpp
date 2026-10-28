#include "frontend/pick_font.h"

#include <gtest/gtest.h>

#include <set>

using namespace oap;

namespace {
auto existing(std::set<std::string> files) {
    return [files](const std::string& path) { return files.count(path) > 0; };
}
} // namespace

TEST(PickFont, FirstCandidateWinsWhenItExists) {
    // --font PATH is put first by the caller
    auto font = pickFont({"/mine.ttf", "/a.ttf", "/b.ttf"}, existing({"/mine.ttf", "/a.ttf", "/b.ttf"}));
    EXPECT_EQ(font, "/mine.ttf");
}

TEST(PickFont, FirstThatExistsWhenEarlierOnesDoNot) {
    auto font = pickFont({"/mine.ttf", "/a.ttf", "/b.ttf", "/c.ttf"}, existing({"/b.ttf", "/c.ttf"}));
    EXPECT_EQ(font, "/b.ttf");
}

TEST(PickFont, NoneWhenNothingExists) {
    EXPECT_FALSE(pickFont({"/a.ttf", "/b.ttf"}, existing({})).has_value());
    EXPECT_FALSE(pickFont({}, existing({"/a.ttf"})).has_value());
}

TEST(PickFont, EmptyPathIsSkipped) {
    // no --font given: the caller may pass an empty first entry
    auto font = pickFont({"", "/a.ttf"}, [](const std::string& p) { return p.empty() || p == "/a.ttf"; });
    EXPECT_EQ(font, "/a.ttf");
}
