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

Player::Player(Mixer& mixer) : mixer_(mixer) { av_log_set_level(AV_LOG_ERROR); }

Player::~Player() { close(); }

bool Player::open(const std::string& path, double startAt) {
    close();

    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "player: cannot open %s\n", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        close();
        return false;
    }
    const AVCodec* codec = nullptr;
    int index = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0 || !codec) {
        std::fprintf(stderr, "player: no audio stream in %s\n", path.c_str());
        close();
        return false;
    }
    stream_ = fmt_->streams[index];
    dec_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(dec_, stream_->codecpar) < 0 || avcodec_open2(dec_, codec, nullptr) < 0) {
        close();
        return false;
    }

    AVChannelLayout in, out;
    if (dec_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in, dec_->ch_layout.nb_channels);
    else
        av_channel_layout_copy(&in, &dec_->ch_layout);
    av_channel_layout_default(&out, 2);
    int r = swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLT, Mixer::kSampleRate, &in,
                                dec_->sample_fmt, dec_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in);
    if (r < 0 || swr_init(swr_) < 0) {
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
    seekRequest_ = seconds;
    endPts_ = seconds; // so position() is right immediately
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

    while (!stop_) {
        double target = seekRequest_.exchange(-1);
        if (target >= 0) {
            av_seek_frame(fmt_, -1, int64_t(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec_);
            swr_init(swr_);
            epoch = mixer_.content().flush();
            endPts_ = target;
            skipUntil_ = target;
            eof_ = false;
        }
        if (eof_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool flushing = av_read_frame(fmt_, packet) < 0;
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

} // namespace oap
