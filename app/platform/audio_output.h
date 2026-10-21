#pragma once

#include "../core/mixer.h"

#include <memory>

namespace oap {

// Pulls audio from the mixer and plays it. One backend per platform:
// CoreAudio on macOS now, PulseAudio (or ALSA) on Linux later.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual bool start(Mixer& mixer) = 0;
    virtual void stop() = 0;

    static std::unique_ptr<AudioOutput> create();
};

} // namespace oap
