#pragma once

#include "core/mixer.h"
#include "core/services.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwrContext;

namespace oap {

// Decodes one audio file with FFmpeg into the mixer's content stream.
// All public methods are called from the UI thread; decoding runs on its own thread.
class Player : public MediaPlayer {
public:
    explicit Player(Mixer& mixer);
    ~Player() override;

    bool open(const std::string& path, double startAt = 0) override;
    void close() override;
    bool isOpen() const override { return fmt_ != nullptr; }

    void play() override;
    void pause() override;
    bool playing() const override { return playing_; }

    void seek(double seconds) override;
    double position() const override;
    double duration() const override { return duration_; }
    const std::vector<Chapter>& chapters() const override { return chapters_; }

    // End of file reached while playing and everything has been heard.
    bool finished() const override;

private:
    void decodeLoop();
    bool emit(AVFrame* frame, uint64_t epoch);
    bool shouldAbortWrite() const { return stop_ || seekRequest_ >= 0; }

    Mixer& mixer_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVStream* stream_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    std::atomic<double> seekRequest_{-1};
    std::atomic<double> endPts_{0}; // timestamp of the end of the audio already queued
    double skipUntil_ = 0;          // decoder thread: drop audio before this (exact seek)
    double duration_ = 0;
    std::vector<Chapter> chapters_;
    std::vector<float> converted_;
};

} // namespace oap
