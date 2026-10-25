#include "player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <utility>

namespace oap {

struct Player::ResamplerInput {
    int sampleRate = 0;
    int sampleFormat = -1;
    AVChannelLayout layout{};
    ~ResamplerInput() { av_channel_layout_uninit(&layout); }
};

namespace {
// Anything but a local path: it gets the network options and the open timeout,
// and is never printed.
bool isUrl(const std::string& path) { return path.find("://") != std::string::npos; }

// A stream URL carries the server login, which must never reach a log
// (docs/streaming.md), so only local paths are printed.
std::string printable(const std::string& path) { return isUrl(path) ? "a stream" : path; }

std::string errorText(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof text);
    return text;
}

int64_t steadyNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <class Duration> int64_t microseconds(Duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

// One word of a log line (no spaces or quotes) with its URL or query cut out.
std::string redactWord(const std::string& word) {
    size_t scheme = word.find("://");
    if (scheme == std::string::npos) {
        size_t query = word.find('?');
        bool cut = query != std::string::npos && word.find('=', query) != std::string::npos;
        return cut ? word.substr(0, query) + "?..." : word;
    }
    size_t authority = scheme + 3;
    size_t pathStart = word.find_first_of("/?#", authority);
    size_t at = word.rfind('@');
    if (at != std::string::npos && at >= authority) {
        if (pathStart < at) return word.substr(0, authority) + "..."; // a user name or a path? Keep neither.
        authority = at + 1;                                            // drop user:password@
    }
    std::string out = word.substr(0, scheme + 3) + word.substr(authority, pathStart - authority);
    return pathStart == std::string::npos ? out : out + "/...";
}

// Replaces FFmpeg's default log callback, which prints the message as it is.
// FFmpeg may log from any thread.
void logRedacted(void* context, int level, const char* format, va_list args) {
    if ((level & 0xff) > av_log_get_level()) return; // the bits above are colours
    static std::mutex& mutex = *new std::mutex; // never destroyed: a dropped open may still log at exit
    static int printPrefix = 1; // FFmpeg's state between the parts of one line
    std::lock_guard lock(mutex);
    char line[2048];
    int length = av_log_format_line2(context, level, format, args, line, sizeof line, &printPrefix);
    std::string text = redactUrls(line);
    if (length >= int(sizeof line) && printPrefix) text += '\n'; // cut short: keep the line break
    std::fputs(text.c_str(), stderr);
}
} // namespace

std::string redactUrls(const std::string& line) {
    auto separates = [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '\'' || c == '"'; };
    std::string out;
    for (size_t pos = 0; pos < line.size();) {
        size_t end = pos;
        while (end < line.size() && !separates(line[end])) ++end;
        if (end == pos) {
            out += line[pos++];
            continue;
        }
        out += redactWord(line.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

ReadErrorAction onReadError(int error, bool atEndOfFile, int& consecutive) {
    if (error == AVERROR_EOF || atEndOfFile) return ReadErrorAction::End;
    return ++consecutive > kMaxReadErrors ? ReadErrorAction::End : ReadErrorAction::Retry;
}

// One open() and the thread it runs on. The thread keeps its own reference, so
// the job outlives a Player that dropped the open (close() does not wait for it).
struct Player::OpenJob {
    std::atomic<bool> stop{false};       // close(): FFmpeg gives up any wait at once
    std::atomic<int64_t> deadline{0};    // steady_clock ns at which an open of a stream gives up; 0: none
    std::atomic<bool> timedOut{false};   // set by interrupted() when it ends an open
    std::mutex mutex;                    // for the two below
    bool dropped = false;                // close() came first: the thread frees what it opened and ends
    bool handedOver = false;             // the thread decodes for the Player now: close() waits for it
};

struct Player::Input {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVStream* stream = nullptr;
    SwrContext* swr = nullptr;
    std::unique_ptr<ResamplerInput> swrIn;
    double duration = 0;
    std::vector<Chapter> chapters;
    ~Input() {
        swr_free(&swr);
        avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }
};

Player::Player(Mixer& mixer, std::chrono::milliseconds openTimeout) : mixer_(mixer), openTimeout_(openTimeout) {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(logRedacted);
}

Player::~Player() { close(); }

int Player::interrupted(void* opaque) {
    auto* job = static_cast<OpenJob*>(opaque);
    if (job->stop) return 1; // close() is waiting, or has dropped this open
    int64_t deadline = job->deadline;
    if (deadline == 0 || steadyNanoseconds() < deadline) return 0;
    job->timedOut = true;
    return 1;
}

// Everything that reads from the file or the network before decoding starts. A
// stream is given up after `timeout` in all.
bool Player::openInput(const std::string& path, OpenJob& job, std::chrono::milliseconds timeout, Input& in) {
    in.fmt = avformat_alloc_context();
    if (!in.fmt) return false;
    in.fmt->interrupt_callback = {&Player::interrupted, &job};
    AVDictionary* options = nullptr;
    if (isUrl(path)) {
        // Options another protocol does not know are left unused.
        av_dict_set_int(&options, "rw_timeout", microseconds(kStreamReadTimeout), 0); // no data: the read fails
        av_dict_set_int(&options, "timeout", microseconds(kStreamReadTimeout), 0);    // TCP connect and I/O
        // A dropped connection before the end is resumed at the same byte. Not on a
        // network error while connecting (the server is gone: fail at once) and not
        // for streams without a length (a transcode), which would start again from 0.
        av_dict_set_int(&options, "reconnect", 1, 0);
        av_dict_set_int(&options, "reconnect_delay_max", kStreamReconnectDelayMax, 0);
        job.deadline = steadyNanoseconds() + std::chrono::nanoseconds(timeout).count();
    }
    int err = avformat_open_input(&in.fmt, path.c_str(), nullptr, &options); // frees fmt on failure
    av_dict_free(&options);
    if (err >= 0) err = avformat_find_stream_info(in.fmt, nullptr);
    job.deadline = 0;
    // Cut off, avformat_find_stream_info() may still succeed with what it has, but
    // the input is then at its end: no open.
    bool timedOut = job.timedOut;
    if (job.stop) return false; // dropped: nobody waits for the reason
    if (err < 0 || timedOut) {
        std::fprintf(stderr, "player: cannot open %s: %s\n", printable(path).c_str(),
                     timedOut ? "no answer in time" : errorText(err).c_str());
        return false;
    }

    const AVCodec* codec = nullptr;
    int index = av_find_best_stream(in.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0 || !codec) {
        std::fprintf(stderr, "player: no audio stream in %s\n", printable(path).c_str());
        return false;
    }
    in.stream = in.fmt->streams[index];
    in.dec = avcodec_alloc_context3(codec);
    if (!in.dec || avcodec_parameters_to_context(in.dec, in.stream->codecpar) < 0 ||
        avcodec_open2(in.dec, codec, nullptr) < 0)
        return false;
    in.swrIn = std::make_unique<ResamplerInput>();
    if (!setupResampler(in.swr, *in.swrIn, in.dec->sample_rate, in.dec->sample_fmt, in.dec->ch_layout)) return false;

    in.duration = in.fmt->duration != AV_NOPTS_VALUE ? double(in.fmt->duration) / AV_TIME_BASE : 0;
    for (unsigned i = 0; i < in.fmt->nb_chapters; ++i) {
        const AVChapter* ch = in.fmt->chapters[i];
        const AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
        in.chapters.push_back({title ? title->value : "Chapter " + std::to_string(i + 1),
                               double(ch->start) * av_q2d(ch->time_base)});
    }
    return true;
}

void Player::openAndDecode(std::shared_ptr<OpenJob> job, Player* self, std::string path, double startAt,
                           std::chrono::milliseconds timeout) {
    Input in;
    bool ok = openInput(path, *job, timeout, in);
    {
        std::lock_guard lock(job->mutex);
        if (job->dropped) return; // the Player has moved on and may be gone: `in` frees itself
        job->handedOver = true;   // from here on close() waits for this thread
    }
    if (!ok) {
        self->state_ = State::Closed;
        return;
    }
    self->start(in, startAt);
    self->decodeLoop();
}

void Player::start(Input& in, double startAt) {
    fmt_ = std::exchange(in.fmt, nullptr);
    dec_ = std::exchange(in.dec, nullptr);
    stream_ = in.stream;
    swr_ = std::exchange(in.swr, nullptr);
    swrIn_ = std::move(in.swrIn);
    duration_ = in.duration;
    chapters_ = std::move(in.chapters);

    mixer_.content().flush();
    endPts_ = 0;
    skipUntil_ = 0;
    if (duration_ > 0) startAt = std::min(startAt, duration_);
    if (startAt > 0) seekRequest_ = startAt;
    eof_ = false;
    state_ = State::Open; // last: the UI thread reads the fields above once it sees this
}

bool Player::open(const std::string& path, double startAt) {
    close();
    stop_ = false;
    isUrl_ = isUrl(path);
    failed_ = false;
    job_ = std::make_shared<OpenJob>();
    if (isUrl_) {
        state_ = State::Opening;
        thread_ = std::thread(&Player::openAndDecode, job_, this, path, startAt, openTimeout_);
        return true;
    }
    job_->handedOver = true; // a local file opens here: close() waits for its decoder thread
    {
        Input in; // freed before job_, which its interrupt callback points to
        if (openInput(path, *job_, openTimeout_, in)) {
            start(in, startAt);
            thread_ = std::thread(&Player::decodeLoop, this);
            return true;
        }
    }
    job_.reset();
    return false;
}

void Player::close() {
    stop_ = true; // also aborts a network wait of the decoder thread (interrupted())
    if (job_) {
        job_->stop = true;
        std::lock_guard lock(job_->mutex);
        job_->dropped = true;
        // Still opening: FFmpeg may be in a wait no interrupt reaches (a DNS
        // lookup). The thread no longer touches this Player and cleans up itself.
        if (!job_->handedOver && thread_.joinable()) thread_.detach();
    }
    if (thread_.joinable()) thread_.join();
    state_ = State::Closed;
    mixer_.setContentPaused(true);
    mixer_.content().flush();
    playing_ = false;
    swr_free(&swr_);
    swrIn_.reset();
    avcodec_free_context(&dec_);
    if (fmt_) avformat_close_input(&fmt_);
    job_.reset(); // after fmt_, whose interrupt callback points to it
    stream_ = nullptr;
    chapters_.clear();
    duration_ = 0;
    seekRequest_ = -1;
}

const std::vector<Chapter>& Player::chapters() const {
    static const std::vector<Chapter> none;
    return isOpen() ? chapters_ : none;
}

void Player::play() {
    if (!isOpen()) return;
    playing_ = true;
    mixer_.setContentPaused(false);
}

void Player::pause() {
    playing_ = false;
    mixer_.setContentPaused(true);
}

void Player::seek(double seconds) {
    if (!isOpen()) return;
    seconds = std::max(0.0, seconds);
    if (duration_ > 0) seconds = std::min(seconds, duration_);
    seekRequest_ = seconds; // position() reports it until the decoder has seeked
}

double Player::position() const {
    if (!isOpen()) return 0;
    if (seekRequest_ >= 0) return seekRequest_;
    double buffered = double(mixer_.content().buffered()) / Mixer::kSampleRate;
    return std::max(0.0, endPts_ - buffered);
}

bool Player::finished() const {
    return isOpen() && playing_ && eof_ && !failed_ && seekRequest_ < 0 && mixer_.content().buffered() == 0;
}

bool Player::failed() const {
    return isOpen() && playing_ && eof_ && failed_ && seekRequest_ < 0 && mixer_.content().buffered() == 0;
}

void Player::decodeLoop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint64_t epoch = mixer_.content().epoch();
    int readErrors = 0; // in a row

    while (!stop_) {
        double target = seekRequest_;
        if (target >= 0) {
            if (av_seek_frame(fmt_, -1, int64_t(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(dec_);
                swr_init(swr_);
                epoch = mixer_.content().flush();
                endPts_ = target;
                skipUntil_ = target;
                eof_ = false;
                failed_ = false;
                if (fmt_->pb) fmt_->pb->error = 0; // the seek got through: an earlier error is over
                readErrors = 0;
            } else {
                // Keep the queued audio and position. FFmpeg drops its read buffers even when the
                // seek fails, so the decoder starts afresh on whatever comes next.
                std::fprintf(stderr, "player: cannot seek to %.1f s\n", target);
                avcodec_flush_buffers(dec_);
            }
            // Cleared after endPts_ is set, so position() never shows the old one.
            // A newer request stays pending for the next round.
            seekRequest_.compare_exchange_strong(target, -1);
        }
        if (eof_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int err = av_read_frame(fmt_, packet);
        if (err >= 0) {
            readErrors = 0;
        } else if (onReadError(err, fmt_->pb && avio_feof(fmt_->pb), readErrors) == ReadErrorAction::Retry) {
            if (readErrors == 1)
                std::fprintf(stderr, "player: read error: %s, retrying\n", errorText(err).c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        } else if (readErrors > kMaxReadErrors) {
            std::fprintf(stderr, "player: read error: %s, treating it as the end of the file\n",
                         errorText(err).c_str());
        }
        bool flushing = err < 0;
        // A stream that ends with a network error (reconnects gave up) has failed: it did not end.
        if (flushing && isUrl_ && fmt_->pb && fmt_->pb->error < 0 && fmt_->pb->error != AVERROR_EOF) failed_ = true;
        if (!flushing && packet->stream_index != stream_->index) {
            av_packet_unref(packet);
            continue;
        }
        avcodec_send_packet(dec_, flushing ? nullptr : packet);
        av_packet_unref(packet);

        bool aborted = false;
        while (avcodec_receive_frame(dec_, frame) == 0) {
            if (!emit(frame, epoch)) {
                aborted = true;
                break;
            }
        }
        av_frame_unref(frame);
        if (flushing && !aborted) eof_ = true;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}

bool Player::emit(AVFrame* frame, uint64_t epoch) {
    double t = endPts_;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        t = double(frame->best_effort_timestamp) * av_q2d(stream_->time_base);

    if (frame->sample_rate != swrIn_->sampleRate || frame->format != swrIn_->sampleFormat ||
        av_channel_layout_compare(&frame->ch_layout, &swrIn_->layout) != 0) {
        if (!setupResampler(swr_, *swrIn_, frame->sample_rate, frame->format, frame->ch_layout)) {
            std::fprintf(stderr, "player: cannot convert audio format, skipping it\n");
            return true;
        }
    }

    int maxOut = swr_get_out_samples(swr_, frame->nb_samples);
    if (maxOut <= 0) return true;
    converted_.resize(size_t(maxOut) * AudioStream::kChannels);
    auto* out = reinterpret_cast<uint8_t*>(converted_.data());
    int n = swr_convert(swr_, &out, maxOut, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (n <= 0) return true;

    // After a backward seek the decoder starts a bit early: drop audio before the target.
    size_t skip = 0;
    if (skipUntil_ > t) skip = std::min<size_t>(size_t(n), size_t((skipUntil_ - t) * Mixer::kSampleRate));
    if (skip == size_t(n)) return true;

    const float* p = converted_.data() + skip * AudioStream::kChannels;
    size_t left = size_t(n) - skip;
    double cursor = t + double(skip) / Mixer::kSampleRate;
    while (left > 0) {
        if (shouldAbortWrite()) return false;
        long written = mixer_.content().write(p, left, epoch);
        if (written < 0) return false;
        p += written * AudioStream::kChannels;
        left -= size_t(written);
        cursor += double(written) / Mixer::kSampleRate;
        endPts_ = cursor;
        if (left > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

bool Player::setupResampler(SwrContext*& swr, ResamplerInput& input, int sampleRate, int sampleFormat,
                            const AVChannelLayout& layout) {
    input.sampleRate = sampleRate;
    input.sampleFormat = sampleFormat;
    av_channel_layout_uninit(&input.layout);
    av_channel_layout_copy(&input.layout, &layout);

    AVChannelLayout in{}, out{}; // zeroed: av_channel_layout_copy frees dst first
    if (layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in, layout.nb_channels);
    else
        av_channel_layout_copy(&in, &layout);
    av_channel_layout_default(&out, 2);
    swr_free(&swr);
    int r = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, Mixer::kSampleRate, &in,
                                AVSampleFormat(sampleFormat), sampleRate, 0, nullptr);
    av_channel_layout_uninit(&in);
    return r >= 0 && swr_init(swr) >= 0;
}

} // namespace oap
