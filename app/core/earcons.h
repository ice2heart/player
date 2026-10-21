#pragma once

#include "mixer.h"

namespace oap {

// Short synthesized UI sounds. Generated in code so no asset files are needed yet.
enum class Earcon {
    Move,    // moved to another menu item
    Wrap,    // list wrapped around
    Edge,    // nothing there / end of list
    Open,    // entered a submenu
    Close,   // left a submenu
    Error,
    Volume,
    Play,
    Boot,
    Sleep,
    Wake,
};

class Earcons {
public:
    explicit Earcons(Mixer& mixer) : mixer_(mixer) {}
    void play(Earcon earcon);

private:
    Mixer& mixer_;
};

} // namespace oap
