#pragma once

#include "core/hal.h"
#include "core/services.h"

namespace oap {

// Backends share these services; both outlive the parts created by makeHal.
struct HalContext {
    HalEvents& events;
    Clock& clock;
};

// Part ownership is added alongside each backend interface.
struct HalParts {};

// Generated for the backends selected at configure time.
HalParts makeHal(HalContext& context);

} // namespace oap
