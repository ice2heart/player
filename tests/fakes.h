#pragma once

// Test doubles for the interfaces in core/services.h. They record what App asked
// for and let the test set what App will observe (time, playback position, ...).

#include "core/services.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace oap::test {

class FakeClock : public Clock {
public:
    TimePoint now() const override { return now_; }
    std::time_t wallTime() const override { return 1'700'000'000; }
    void advance(std::chrono::milliseconds ms) { now_ += ms; }

private:
    TimePoint now_{std::chrono::hours(1000)};
};

class FakeSpeech : public Speech {
public:
    void say(const std::string& text) override { said.push_back(text); }
    void stop() override { ++stops; }
    bool speaking() const override { return isSpeaking; }
    void setRate(int wordsPerMinute) override { rate = wordsPerMinute; }

    std::string last() const { return said.empty() ? std::string() : said.back(); }
    bool heard(const std::string& text) const { return std::find(said.begin(), said.end(), text) != said.end(); }

    std::vector<std::string> said;
    int stops = 0;
    int rate = 0;
    bool isSpeaking = false;
};

class FakePlayer : public MediaPlayer {
public:
    bool open(const std::string& path, double startAt) override {
        opened.push_back(path);
        file = path;
        isOpen_ = openSucceeds;
        playing_ = false;
        pos = startAt;
        atEnd = false;
        return openSucceeds;
    }
    void close() override {
        isOpen_ = false;
        playing_ = false;
    }
    bool isOpen() const override { return isOpen_; }
    void play() override { playing_ = true; }
    void pause() override { playing_ = false; }
    bool playing() const override { return playing_; }
    void seek(double seconds) override { pos = seconds; }
    double position() const override { return pos; }
    double duration() const override { return length; }
    const std::vector<Chapter>& chapters() const override { return embedded; }
    bool finished() const override { return isOpen_ && playing_ && atEnd; }

    std::vector<std::string> opened;
    std::string file;
    double pos = 0;
    double length = 600;
    std::vector<Chapter> embedded;
    bool atEnd = false;
    bool openSucceeds = true;

private:
    bool isOpen_ = false;
    bool playing_ = false;
};

class FakeSounds : public SoundEffects {
public:
    void play(Earcon earcon) override { played.push_back(earcon); }
    std::optional<Earcon> last() const { return played.empty() ? std::nullopt : std::optional(played.back()); }
    std::vector<Earcon> played;
};

class FakeVolume : public VolumeControl {
public:
    void setVolume(float v) override { volume = v; }
    float volume = -1;
};

} // namespace oap::test
