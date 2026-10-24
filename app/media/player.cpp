#include "player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace oap {

struct Player::ResamplerInput {
    int sampleRate = 0;
    int sampleFormat = -1;
    AVChannelLayout layout{};
    ~ResamplerInput() { av_channel_layout_uninit(&layout); }
};

namespace {
// A stream URL carries the server login, which must never reach a log
// (docs/streaming.md), so only local paths are printed.
std::string printable(const std::string& path) { return path.find("://") == std::string::npos ? path : "a stream"; }

std::string errorText(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof text);
    return text;
}
} // namespace

ReadErrorAction onReadError(int error, bool atEndOfFile, int& consecutive) {
    if (error == AVERROR_EOF || atEndOfFile) return ReadErrorAction::End;
    return ++consecutive > kMaxReadErrors ? ReadErrorAction::End : ReadErrorAction::Retry;
}

Player::Player(Mixer& mixer) : mixer_(mixer) { av_log_set_level(AV_LOG_ERROR); }

Player::~Player() { close(); }

bool Player::open(const std::string& path, double startAt) {
    close();

    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "player: cannot open %s\n", printable(path).c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        close();
        return false;
    }
    const AVCodec* codec = nullptr;
    int index = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0 || !codec) {
        std::fprintf(stderr, "player: no audio stream in %s\n", printable(path).c_str());
        close();
        return false;
    }
    stream_ = fmt_->streams[index];
    dec_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(dec_, stream_->codecpar) < 0 || avcodec_open2(dec_, codec, nullptr) < 0) {
        close();
        return false;
    }

    if (!setupResampler(dec_->sample_rate, dec_->sample_fmt, dec_->ch_layout)) {
        close();
        return false;
    }

    duration_ = fmt_->duration != AV_NOPTS_VALUE ? double(fmt_->duration) / AV_TIME_BASE : 0;
    for (unsigned i = 0; i < fmt_->nb_chapters; ++i) {
        const AVChapter* ch = fmt_->chapters[i];
        const AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
        chapters_.push_back({title ? title->value : "Chapter " + std::to_string(i + 1),
                             double(ch->start) * av_q2d(ch->time_base)});
    }

    mixer_.content().flush();
    endPts_ = 0;
    skipUntil_ = 0;
    if (duration_ > 0) startAt = std::min(startAt, duration_);
    if (startAt > 0) seekRequest_ = startAt;
    stop_ = false;
    eof_ = false;
    thread_ = std::thread(&Player::decodeLoop, this);
    return true;
}

void Player::close() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    mixer_.setContentPaused(true);
    mixer_.content().flush();
    playing_ = false;
    swr_free(&swr_);
    swrIn_.reset();
    avcodec_free_context(&dec_);
    if (fmt_) avformat_close_input(&fmt_);
    stream_ = nullptr;
    chapters_.clear();
    duration_ = 0;
    seekRequest_ = -1;
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
    return isOpen() && playing_ && eof_ && seekRequest_ < 0 && mixer_.content().buffered() == 0;
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
        if (!setupResampler(frame->sample_rate, frame->format, frame->ch_layout)) {
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

bool Player::setupResampler(int sampleRate, int sampleFormat, const AVChannelLayout& layout) {
    if (!swrIn_) swrIn_ = std::make_unique<ResamplerInput>();
    swrIn_->sampleRate = sampleRate;
    swrIn_->sampleFormat = sampleFormat;
    av_channel_layout_uninit(&swrIn_->layout);
    av_channel_layout_copy(&swrIn_->layout, &layout);

    AVChannelLayout in{}, out{}; // zeroed: av_channel_layout_copy frees dst first
    if (layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in, layout.nb_channels);
    else
        av_channel_layout_copy(&in, &layout);
    av_channel_layout_default(&out, 2);
    swr_free(&swr_);
    int r = swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLT, Mixer::kSampleRate, &in,
                                AVSampleFormat(sampleFormat), sampleRate, 0, nullptr);
    av_channel_layout_uninit(&in);
    return r >= 0 && swr_init(swr_) >= 0;
}

} // namespace oap
