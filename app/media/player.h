#pragma once

#include "core/mixer.h"
#include "core/services.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVChannelLayout;
struct SwrContext;

namespace oap {

// What the decoder does when av_read_frame() fails. Only AVERROR_EOF (or the
// file's end reached) is the end of the file; other errors are retried, and
// after kMaxReadErrors in a row the file is treated as ended so playback
// cannot hang. `consecutive` counts the errors in a row (reset it on success).
enum class ReadErrorAction { Retry, End };
constexpr int kMaxReadErrors = 50;
ReadErrorAction onReadError(int error, bool atEndOfFile, int& consecutive);

// Opening a stream (any URL) runs on the UI thread, which waits for it, so it
// gives up after this long (docs/streaming.md, step 1.6).
constexpr std::chrono::milliseconds kStreamOpenTimeout = std::chrono::seconds(10);
// A stream read that gets no data for this long fails, and FFmpeg reconnects.
constexpr std::chrono::seconds kStreamReadTimeout{10};
// FFmpeg waits 0, 1, 3, 7 ... s between reconnects and gives up past this delay.
constexpr int kStreamReconnectDelayMax = 4;

// `line` with every URL cut down to its scheme and host, and every other query
// (`?a=b`) cut off: a stream URL carries the server login in its query, and
// FFmpeg prints some URLs and request lines in its messages.
std::string redactUrls(const std::string& line);

// Decodes one audio file with FFmpeg into the mixer's content stream.
// All public methods are called from the UI thread; decoding runs on its own thread.
// A stream is opened on the calling thread and given up after `openTimeout`;
// close() aborts any network wait at once (FFmpeg's interrupt callback).
// FFmpeg's own messages go through redactUrls().
class Player : public MediaPlayer {
public:
    explicit Player(Mixer& mixer, std::chrono::milliseconds openTimeout = kStreamOpenTimeout);
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
    bool failed() const override;

private:
    // FFmpeg's interrupt callback, on whichever thread is waiting for I/O.
    static int interrupted(void* player);
    bool openInput(const std::string& path);
    void decodeLoop();
    bool emit(AVFrame* frame, uint64_t epoch);
    bool shouldAbortWrite() const { return stop_ || seekRequest_ >= 0; }
    // (Re)builds swr_ for this input format. A file may change format mid-stream.
    bool setupResampler(int sampleRate, int sampleFormat, const AVChannelLayout& layout);

    struct ResamplerInput; // the input format swr_ was built for

    Mixer& mixer_;
    const std::chrono::milliseconds openTimeout_;
    // steady_clock time (ns since its epoch) at which an open gives up; 0 when none runs.
    std::atomic<int64_t> openDeadline_{0};
    std::atomic<bool> openTimedOut_{false}; // set by interrupted() when it ends an open
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVStream* stream_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::unique_ptr<ResamplerInput> swrIn_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> failed_{false}; // a stream ended with a network error, not at its end
    bool isUrl_ = false;              // the open input is a stream
    std::atomic<double> seekRequest_{-1};
    std::atomic<double> endPts_{0}; // timestamp of the end of the audio already queued
    double skipUntil_ = 0;          // decoder thread: drop audio before this (exact seek)
    double duration_ = 0;
    std::vector<Chapter> chapters_;
    std::vector<float> converted_;
};

} // namespace oap
