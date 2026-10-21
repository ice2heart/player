#pragma once

#include "mixer.h"
#include "services.h"

namespace oap {

// Short synthesized UI sounds. Generated in code so no asset files are needed yet.
class Earcons : public SoundEffects {
public:
    explicit Earcons(Mixer& mixer) : mixer_(mixer) {}
    void play(Earcon earcon) override;

private:
    Mixer& mixer_;
};

} // namespace oap
