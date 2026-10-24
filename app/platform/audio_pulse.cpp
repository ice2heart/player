// PulseAudio output through the simple API. Also works on PipeWire systems
// through pipewire-pulse.

#include "audio_thread.h"

#include <pulse/error.h>
#include <pulse/simple.h>

namespace oap {

namespace {

class PulseOutput : public ThreadedAudioOutput {
public:
    ~PulseOutput() override { stopThread(); }

protected:
    bool openDevice() override {
        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_FLOAT32NE;
        spec.rate = Mixer::kSampleRate;
        spec.channels = AudioStream::kChannels;

        // Keep about 50 ms in the server so key presses are heard quickly.
        const uint32_t bytesPerFrame = sizeof(float) * AudioStream::kChannels;
        pa_buffer_attr attr{};
        attr.maxlength = uint32_t(-1);
        attr.tlength = bytesPerFrame * Mixer::kSampleRate / 20;
        attr.prebuf = uint32_t(-1);
        attr.minreq = uint32_t(-1);
        attr.fragsize = uint32_t(-1);

        int error = 0;
        pa_ = pa_simple_new(nullptr, "Open Audio Player", PA_STREAM_PLAYBACK, nullptr, "playback", &spec,
                            nullptr, &attr, &error);
        if (!pa_) {
            std::fprintf(stderr, "audio: pulse: %s\n", pa_strerror(error));
            return false;
        }
        return true;
    }

    bool write(const float* data, size_t frames) override {
        int error = 0;
        if (pa_simple_write(pa_, data, frames * sizeof(float) * AudioStream::kChannels, &error) < 0) {
            std::fprintf(stderr, "audio: pulse: %s\n", pa_strerror(error));
            return false;
        }
        return true;
    }

    void closeDevice() override {
        if (pa_) pa_simple_free(pa_);
        pa_ = nullptr;
    }

private:
    pa_simple* pa_ = nullptr;
};

} // namespace

std::unique_ptr<AudioOutput> makePulseOutput() { return std::make_unique<PulseOutput>(); }

} // namespace oap
