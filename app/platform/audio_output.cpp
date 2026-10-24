// Picks an audio backend by name. The OAP_AUDIO_* definitions come from CMake
// and say which backends were compiled in.

#include "audio_output.h"

#include <cstdio>

namespace oap {

namespace {

struct Backend {
    const char* name;
    bool device; // tried by "auto"
    std::unique_ptr<AudioOutput> (*make)(const std::string& arg);
};

const Backend kBackends[] = {
#ifdef OAP_AUDIO_COREAUDIO
    {"coreaudio", true, [](const std::string&) { return makeCoreAudioOutput(); }},
#endif
#ifdef OAP_AUDIO_PULSE
    {"pulse", true, [](const std::string&) { return makePulseOutput(); }},
#endif
#ifdef OAP_AUDIO_ALSA
    {"alsa", true, [](const std::string& device) { return makeAlsaOutput(device.empty() ? "default" : device); }},
#endif
    {"null", false, [](const std::string&) { return makeNullOutput(); }},
};

} // namespace

std::vector<std::string> AudioOutput::backends() {
    std::vector<std::string> names{"auto"};
    for (const auto& b : kBackends) names.push_back(b.name);
    return names;
}

std::unique_ptr<AudioOutput> AudioOutput::open(const std::string& spec, Mixer& mixer) {
    std::string name = spec, arg;
    if (auto colon = spec.find(':'); colon != std::string::npos) {
        name = spec.substr(0, colon);
        arg = spec.substr(colon + 1);
    }
    for (const auto& b : kBackends) {
        if (name != "auto" && name != b.name) continue;
        if (name == "auto" && !b.device) continue;
        auto output = b.make(arg);
        if (output->start(mixer)) {
            std::printf("audio: %s\n", b.name);
            return output;
        }
        if (name != "auto") return nullptr;
    }
    if (name == "auto")
        std::fprintf(stderr, "audio: no output device works; use --audio null to run without sound\n");
    else
        std::fprintf(stderr, "audio: backend '%s' is not built in\n", name.c_str());
    return nullptr;
}

} // namespace oap
