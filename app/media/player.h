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
// FFmpeg waits 0, 1, 3, 7 ... s between reconnects and gives up past this delay:
// with 0 it reconnects once, at once. Player's own retries below take over then.
constexpr int kStreamReconnectDelayMax = 0;
// A stream whose connection is lost mid-track (reconnects failed) is opened again
// at the place it got to, first after kStreamRetryFirstDelay, then after twice the
// last wait, up to kStreamRetryMaxDelay. Once kStreamRetryGiveUp has passed
// since the connection was lost, no retry starts: the stream has failed().
constexpr std::chrono::milliseconds kStreamRetryFirstDelay = std::chrono::seconds(1);
constexpr std::chrono::milliseconds kStreamRetryMaxDelay = std::chrono::seconds(8);
constexpr std::chrono::milliseconds kStreamRetryGiveUp = std::chrono::seconds(30);

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
// A stream that loses its connection while it plays keeps its place and is
// retried with backoff for `retryGiveUp` (buffering() once what came has been
// heard); only then does it fail.
// preload() opens a stream on a thread of its own, like an open but with no
// Player to hand over to yet. open() of that path takes it over: once it has
// opened, its input starts decoding at once (open in open(), as a local file);
// while it still runs, its thread becomes the decoder thread. A dropped preload
// (cancelPreload(), another preload, the destructor) is dropped like an open:
// its thread frees what it opened, or the input it left is freed with its job.
class Player : public MediaPlayer {
public:
    explicit Player(Mixer& mixer, std::chrono::milliseconds openTimeout = kStreamOpenTimeout,
                    std::chrono::milliseconds retryGiveUp = kStreamRetryGiveUp);
    ~Player() override;

    bool open(const std::string& path, double startAt = 0) override;
    bool opening() const override { return state_ == State::Opening; }
    void preload(const std::string& path) override;
    void cancelPreload() override;
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
    bool buffering() const override;

private:
    enum class State { Closed, Opening, Open };
    struct ResamplerInput; // the input format swr_ was built for
    struct Input;          // what an open makes: demuxer, decoder, resampler, duration, chapters
    struct OpenJob;        // one open() or preload() and its thread; shared with a thread that may outlive the Player
    struct Dropout;        // decoder thread: a stream whose connection is lost, and its retries

    // FFmpeg's interrupt callback (its argument is the OpenJob), on whichever thread waits for I/O.
    static int interrupted(void* job);
    // Opens `path` into `in`. Touches no Player, so a dropped open can finish after the Player is gone.
    static bool openInput(const std::string& path, OpenJob& job, std::chrono::milliseconds timeout, Input& in);
    // The thread of a stream's open: opens it, then decodes it for the job's
    // owner, unless the open was dropped first. A preload without an owner yet
    // leaves what it opened in the job and ends.
    static void openThread(std::shared_ptr<OpenJob> job, std::string path, std::chrono::milliseconds timeout);
    // Hands the preload of `path` over to the caller (with its thread), or nothing.
    std::shared_ptr<OpenJob> takePreload(const std::string& path, std::thread& thread);
    // open() of a preloaded stream. False if the preload failed: open as usual then.
    bool adopt(std::shared_ptr<OpenJob> job, std::thread thread, double startAt);
    void start(Input& in, double startAt); // takes over what openInput() made; isOpen() from here on
    void decodeLoop();
    // Decoder thread: the connection of a stream is lost. Waits the dropout's
    // delay (doubled for the next time), then opens the stream again where the
    // queued audio ends. False once the retries have gone on for retryGiveUp_ of
    // playing time.
    bool retryStream(Dropout& dropout);
    bool seekInput(double at); // decoder thread: the demuxer and decoder go to `at`; false if the seek failed
    bool emit(AVFrame* frame, uint64_t epoch);
    bool shouldAbortWrite() const { return stop_ || seekRequest_ >= 0; }
    // (Re)builds `swr` for this input format. A file may change format mid-stream.
    static bool setupResampler(SwrContext*& swr, ResamplerInput& in, int sampleRate, int sampleFormat,
                               const AVChannelLayout& layout);

    Mixer& mixer_;
    const std::chrono::milliseconds openTimeout_;
    const std::chrono::milliseconds retryGiveUp_;
    // Written before state_ becomes Open, and read by the UI thread only once it is Open.
    std::atomic<State> state_{State::Closed};
    std::shared_ptr<OpenJob> job_; // the open the fields below belong to
    std::shared_ptr<OpenJob> preload_; // UI thread: the stream preload() opens, if any
    std::string preloadPath_;
    std::thread preloadThread_;
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
