#include "tts.h"

#include <espeak-ng/speak_lib.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include "core/text.h"

#include <cstdio>

namespace oap {

namespace {
// The language codes of every installed voice ("ru", "cmn", "ja"...). A voice's `languages` is a
// list of (priority byte, NUL-terminated code) pairs ended by an empty one; "en-gb" also counts as "en".
VoiceSet installedVoices() {
    VoiceSet found;
    const espeak_VOICE** voices = espeak_ListVoices(nullptr);
    for (size_t v = 0; voices && voices[v]; ++v) {
        const char* p = voices[v]->languages;
        while (p && *p) {
            ++p; // the priority byte
            std::string code = p;
            p += code.size() + 1;
            found.insert(code);
            found.insert(code.substr(0, code.find('-')));
        }
    }
    return found;
}

Tts* g_instance = nullptr; // eSpeak's callback has no user pointer for synchronous mode
}

Tts::Tts(Mixer& mixer) : mixer_(mixer) {}

Tts::~Tts() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (g_instance == this) {
        espeak_Terminate();
        g_instance = nullptr;
    }
    swr_free(&swr_);
}

bool Tts::start(const std::string& voice) {
    espeakRate_ = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 50, nullptr, 0);
    if (espeakRate_ <= 0) {
        std::fprintf(stderr, "tts: espeak_Initialize failed\n");
        return false;
    }
    g_instance = this;
    espeak_SetSynthCallback(reinterpret_cast<t_espeak_callback*>(&Tts::synthCallback));
    if (espeak_SetVoiceByName(voice.c_str()) != EE_OK)
        std::fprintf(stderr, "tts: voice '%s' not found, using default\n", voice.c_str());

    voices_ = installedVoices(); // before the worker starts: it only reads this afterwards

    AVChannelLayout mono{}, stereo{};
    av_channel_layout_default(&mono, 1);
    av_channel_layout_default(&stereo, 2);
    if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, Mixer::kSampleRate,
                            &mono, AV_SAMPLE_FMT_S16, espeakRate_, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        std::fprintf(stderr, "tts: resampler init failed\n");
        return false;
    }

    thread_ = std::thread(&Tts::run, this);
    return true;
}

void Tts::say(const std::string& text) {
    std::printf("say: %s\n", text.c_str());
    std::fflush(stdout);
    if (text.empty()) return;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({text, mixer_.speech().epoch()});
    }
    cv_.notify_one();
}

void Tts::stop() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    mixer_.speech().flush(); // makes the epoch of the running utterance stale
}

bool Tts::speaking() const {
    std::lock_guard lock(mutex_);
    return busy_ || !queue_.empty() || mixer_.speech().buffered() > 0;
}

void Tts::run() {
    for (;;) {
        Utterance u;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            u = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        currentEpoch_ = u.epoch;
        if (u.epoch == mixer_.speech().epoch()) {
            swr_init(swr_); // forget leftovers from a cancelled utterance
            espeak_SetParameter(espeakRATE, rate_, 0);
            SpeechMarkup markup = speechMarkup(u.text, voices_);
            unsigned flags = espeakCHARS_UTF8 | (markup.ssml ? espeakSSML : 0);
            espeak_Synth(markup.text.c_str(), markup.text.size() + 1, 0, POS_CHARACTER, 0, flags,
                         nullptr, nullptr);
            onSamples(nullptr, 0); // drain the resampler
        }
        std::lock_guard lock(mutex_);
        busy_ = false;
    }
}

int Tts::synthCallback(short* samples, int count, void*) {
    if (!g_instance || !samples || count == 0) return 0;
    return g_instance->onSamples(samples, count);
}

// Returns 1 to abort synthesis (speech was cancelled).
int Tts::onSamples(const short* samples, int count) {
    int maxOut = swr_get_out_samples(swr_, count);
    if (maxOut <= 0) return 0;
    converted_.resize(static_cast<size_t>(maxOut) * AudioStream::kChannels);
    auto* out = reinterpret_cast<uint8_t*>(converted_.data());
    auto* in = reinterpret_cast<const uint8_t*>(samples);
    int n = swr_convert(swr_, &out, maxOut, samples ? &in : nullptr, count);
    if (n <= 0) return 0;
    bool ok = mixer_.speech().writeAll(converted_.data(), static_cast<size_t>(n), currentEpoch_,
                                       [this] { return quit_.load(); });
    return ok ? 0 : 1;
}

} // namespace oap
