#pragma once

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

namespace oap {

// Everything App needs from the outside world. The real implementations live in
// app/media/ (FFmpeg, eSpeak NG) and app/core/ (Earcons, Mixer); tests use fakes.

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;   // monotonic: timers, long presses
    virtual std::time_t wallTime() const = 0; // spoken time of day
};

class SystemClock : public Clock {
public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
    std::time_t wallTime() const override { return std::time(nullptr); }
};

// Text to speech. stop() cancels both queued and currently playing speech.
class Speech {
public:
    virtual ~Speech() = default;
    virtual void say(const std::string& text) = 0;
    virtual void stop() = 0;
    virtual bool speaking() const = 0;
    virtual void setRate(int wordsPerMinute) = 0;
};

struct Chapter {
    std::string title;
    double start = 0;
};

// One audio file at a time.
class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;
    virtual bool open(const std::string& path, double startAt = 0) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual bool playing() const = 0;

    virtual void seek(double seconds) = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0;
    virtual const std::vector<Chapter>& chapters() const = 0; // embedded chapters, may be empty

    // End of file reached while playing and everything has been heard.
    virtual bool finished() const = 0;
};

// Short UI sounds.
enum class Earcon {
    Move,    // moved to another menu item
    Wrap,    // list wrapped around
    Edge,    // nothing there / end of list
    Open,    // entered a submenu
    Close,   // left a submenu
    Error,
    Volume,
    Play,
    Boot,
    Sleep,
    Wake,
};

class SoundEffects {
public:
    virtual ~SoundEffects() = default;
    virtual void play(Earcon earcon) = 0;
};

class VolumeControl {
public:
    virtual ~VolumeControl() = default;
    virtual void setVolume(float volume) = 0; // 0..1
};

} // namespace oap
