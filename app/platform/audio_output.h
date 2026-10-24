#pragma once

#include "../core/mixer.h"

#include <memory>
#include <string>
#include <vector>

namespace oap {

// Pulls audio from the mixer and plays it. Backends: CoreAudio (macOS),
// PulseAudio and ALSA (Linux), and null, which consumes audio in real time
// without a sound card (servers, CI). CMake decides which ones are built in.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual bool start(Mixer& mixer) = 0;
    virtual void stop() = 0;

    // "auto" tries the built-in device backends in order. A backend may take an
    // argument after a colon, such as "alsa:hw:1,0". Returns a started output, or
    // nullptr with the reason printed to stderr.
    static std::unique_ptr<AudioOutput> open(const std::string& name, Mixer& mixer);
    // Names accepted by open(), for --help.
    static std::vector<std::string> backends();
};

std::unique_ptr<AudioOutput> makeCoreAudioOutput();
std::unique_ptr<AudioOutput> makePulseOutput();
std::unique_ptr<AudioOutput> makeAlsaOutput(const std::string& device);
std::unique_ptr<AudioOutput> makeNullOutput();

} // namespace oap
