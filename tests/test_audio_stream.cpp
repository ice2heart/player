#include "core/audio_stream.h"

#include <gtest/gtest.h>

#include <vector>

using oap::AudioStream;

namespace {
std::vector<float> frames(size_t count, float start) {
    std::vector<float> out;
    for (size_t i = 0; i < count; ++i) {
        out.push_back(start + float(i));
        out.push_back(-(start + float(i)));
    }
    return out;
}
} // namespace

TEST(AudioStream, ReadsBackWhatWasWrittenInOrder) {
    AudioStream s(8);
    auto in = frames(5, 1);
    EXPECT_EQ(s.write(in.data(), 5, s.epoch()), 5);
    EXPECT_EQ(s.buffered(), 5u);
    std::vector<float> out(10);
    EXPECT_EQ(s.read(out.data(), 5), 5u);
    EXPECT_EQ(out, in);
    EXPECT_EQ(s.buffered(), 0u);
}

TEST(AudioStream, WriteStopsAtCapacity) {
    AudioStream s(4);
    auto in = frames(6, 1);
    EXPECT_EQ(s.write(in.data(), 6, s.epoch()), 4);
    EXPECT_EQ(s.buffered(), 4u);
}

TEST(AudioStream, WrapsAroundTheRingBuffer) {
    AudioStream s(4);
    auto a = frames(3, 1);
    s.write(a.data(), 3, s.epoch());
    std::vector<float> out(6);
    s.read(out.data(), 3);
    auto b = frames(4, 10); // starts at index 3, wraps to 0
    EXPECT_EQ(s.write(b.data(), 4, s.epoch()), 4);
    std::vector<float> got(8);
    EXPECT_EQ(s.read(got.data(), 4), 4u);
    EXPECT_EQ(got, b);
}

TEST(AudioStream, FlushEmptiesAndRejectsStaleWriters) {
    AudioStream s(8);
    uint64_t old = s.epoch();
    auto in = frames(3, 1);
    s.write(in.data(), 3, old);
    uint64_t now = s.flush();
    EXPECT_NE(now, old);
    EXPECT_EQ(s.buffered(), 0u);
    EXPECT_EQ(s.write(in.data(), 3, old), -1);
    EXPECT_FALSE(s.writeAll(in.data(), 3, old, {}));
    EXPECT_EQ(s.buffered(), 0u);
    EXPECT_TRUE(s.writeAll(in.data(), 3, now, {}));
    EXPECT_EQ(s.buffered(), 3u);
}

TEST(AudioStream, WriteAllStopsWhenAborted) {
    AudioStream s(2);
    auto in = frames(5, 1);
    int calls = 0;
    EXPECT_FALSE(s.writeAll(in.data(), 5, s.epoch(), [&] { return ++calls > 1; }));
    EXPECT_EQ(s.buffered(), 2u);
}
