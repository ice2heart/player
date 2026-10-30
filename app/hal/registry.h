#pragma once

#include "core/hal.h"
#include "core/services.h"

#include <memory>

namespace oap {

// Backends share these services; both outlive the parts created by makeHal.
struct HalContext {
    HalEvents& events;
    Clock& clock;
};

struct HalParts {
    std::unique_ptr<Power> power;
    std::unique_ptr<Storage> storage;
};

// Generated for the backends selected at configure time.
HalParts makeHal(HalContext& context);

} // namespace oap
