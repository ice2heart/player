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

// Opening a stream (any URL) runs in the background and gives up after this
// long, so "Loading" never lasts longer (docs/streaming.md, steps 1.6 and 2.1).
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
// A local file is opened on the calling thread. A stream is opened on the
// decoder thread, so open() returns at once and opening() is true until the
// open has worked (isOpen()) or failed; it gives up after `openTimeout`.
// close() and a newer open() drop an open still running: its result is never
// seen, and they do not wait for it (FFmpeg's interrupt callback aborts network
// waits; the thread of an open that cannot be aborted, such as a DNS lookup,
// is left to end by itself). FFmpeg's own messages go through redactUrls().
class Player : public MediaPlayer {
public:
    explicit Player(Mixer& mixer, std::chrono::milliseconds openTimeout = kStreamOpenTimeout);
    ~Player() override;

    bool open(const std::string& path, double startAt = 0) override;
    bool opening() const override { return state_ == State::Opening; }
    void close() override;
    bool isOpen() const override { return state_ == State::Open; }

    void play() override;
    void pause() override;
    bool playing() const override { return playing_; }

    void seek(double seconds) override;
    double position() const override;
    double duration() const override { return isOpen() ? duration_ : 0; }
    const std::vector<Chapter>& chapters() const override;

    // End of file reached while playing and everything has been heard.
    bool finished() const override;
    bool failed() const override;

private:
    enum class State { Closed, Opening, Open };
    struct ResamplerInput; // the input format swr_ was built for
    struct Input;          // what an open makes: demuxer, decoder, resampler, duration, chapters
    struct OpenJob;        // one open() and its thread; shared with a thread that may outlive the Player

    // FFmpeg's interrupt callback (its argument is the OpenJob), on whichever thread waits for I/O.
    static int interrupted(void* job);
    // Opens `path` into `in`. Touches no Player, so a dropped open can finish after the Player is gone.
    static bool openInput(const std::string& path, OpenJob& job, std::chrono::milliseconds timeout, Input& in);
    // The decoder thread of a stream: opens it, then decodes it unless close() came first.
    static void openAndDecode(std::shared_ptr<OpenJob> job, Player* self, std::string path, double startAt,
                              std::chrono::milliseconds timeout);
    void start(Input& in, double startAt); // takes over what openInput() made; isOpen() from here on
    void decodeLoop();
    bool emit(AVFrame* frame, uint64_t epoch);
    bool shouldAbortWrite() const { return stop_ || seekRequest_ >= 0; }
    // (Re)builds `swr` for this input format. A file may change format mid-stream.
    static bool setupResampler(SwrContext*& swr, ResamplerInput& in, int sampleRate, int sampleFormat,
                               const AVChannelLayout& layout);

    Mixer& mixer_;
    const std::chrono::milliseconds openTimeout_;
    // Written before state_ becomes Open, and read by the UI thread only once it is Open.
    std::atomic<State> state_{State::Closed};
    std::shared_ptr<OpenJob> job_; // the open the fields below belong to
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
