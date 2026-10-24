// ALSA output, for devices without a sound server. The device name is an ALSA
// PCM such as "default" or "plughw:1,0" (--audio alsa:plughw:1,0).

#include "audio_thread.h"

#include <alsa/asoundlib.h>

#include <string>

namespace oap {

namespace {

class AlsaOutput : public ThreadedAudioOutput {
public:
    explicit AlsaOutput(std::string device) : device_(std::move(device)) {}
    ~AlsaOutput() override { stopThread(); }

protected:
    bool openDevice() override {
        int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            std::fprintf(stderr, "audio: alsa: cannot open '%s': %s\n", device_.c_str(), snd_strerror(err));
            pcm_ = nullptr;
            return false;
        }
        // Float stereo at 48 kHz, converted by ALSA's plug layer if the card needs it.
        // 50 ms latency keeps key presses responsive.
        err = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_FLOAT, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 AudioStream::kChannels, Mixer::kSampleRate, 1, 50000);
        if (err < 0) {
            std::fprintf(stderr, "audio: alsa: '%s': %s\n", device_.c_str(), snd_strerror(err));
            closeDevice();
            return false;
        }
        return true;
    }

    bool write(const float* data, size_t frames) override {
        while (frames > 0) {
            snd_pcm_sframes_t n = snd_pcm_writei(pcm_, data, frames);
            if (n < 0) {
                n = snd_pcm_recover(pcm_, int(n), 1); // underrun or resume after suspend
                if (n < 0) {
                    std::fprintf(stderr, "audio: alsa: %s\n", snd_strerror(int(n)));
                    return false;
                }
                continue;
            }
            data += n * AudioStream::kChannels;
            frames -= size_t(n);
        }
        return true;
    }

    void closeDevice() override {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
        }
        pcm_ = nullptr;
    }

private:
    std::string device_;
    snd_pcm_t* pcm_ = nullptr;
};

} // namespace

std::unique_ptr<AudioOutput> makeAlsaOutput(const std::string& device) {
    return std::make_unique<AlsaOutput>(device);
}

} // namespace oap
